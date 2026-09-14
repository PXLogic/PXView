#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PXView MCP→GUI 配置同步修复 · 人工检查表（交互式）

用途：
    配合 PXView GUI（install.dir/bin/PXView.exe），逐项人工验证
    V1.6.4 "MCP 配置写入与 Qt GUI 不同步" 修复是否生效。

用法：
    cd tests
    python check_mcp_gui_sync.py
    python check_mcp_gui_sync.py --url http://127.0.0.1:10110/

流程：
    1. 启动 PXView GUI（建议连接 demo 设备），MCP 服务默认监听 10110 端口。
    2. 运行本脚本，脚本会自动发送对应的 MCP 命令（也打印出来便于手工复现）。
    3. 你在 GUI 里人工核对"核对点"，回到终端按回车记录结果。
       [回车]=通过  f=失败  s=跳过  m=重发本步骤 MCP 命令  q=退出
    4. 全部走完后输出汇总；有失败项时退出码为 1（便于 CI/日志留痕）。

注意：
    - 涉及切换工作模式的步骤会改动设备状态，结束后有"恢复"步骤。
    - 若某步骤的 MCP 命令报错（如设备不支持该键），记录失败原因即可。
"""

import argparse
import json
import sys
import urllib.request

# ──────────────────────────── 可按设备调整的参数 ────────────────────────────
DEFAULT_URL = "http://127.0.0.1:10110/"
# 注意：探针参数 channelIndex 必须用 get_channels 返回的 sr_channel index。
# demo 设备 DSO/模拟通道不是从 0 开始（0..31 是逻辑通道 D0..D31，
# A0=32..A4=36，O0=37..O1=38）。vfactor 仅 DSO 类型通道（O0/O1）持久化，
# demo 的 ANALOG 通道接受但忽略 factor。
PROBE_CHANNEL_INDEX = 37       # O0（DSO 类型，可验证 vfactor 持久化）
RENAME_CHANNEL_INDEX = 0       # 用于 configure_channel 改名的通道
TEST_SAMPLE_RATE = 1_000_000   # set_sample_config 测试用采样率
TEST_SAMPLE_LIMIT = 1_000_000  # set_sample_config 测试用采样深度

# SR_CONF_* 数值键（libsigrok.h）
KEY_CAPTURE_RATIO = 30001      # 触发位置（0..100%）
KEY_RLE = 30003                # RLE 压缩开关（bool）

# ──────────────────────────────── MCP 客户端 ────────────────────────────────
_next_id = [0]


def mcp_call(url, tool, args, timeout=15):
    """POST JSON-RPC 2.0 tools/call 到 PXView MCP 端口，返回 (ok, 文本)。"""
    _next_id[0] += 1
    payload = {
        "jsonrpc": "2.0",
        "id": _next_id[0],
        "method": "tools/call",
        "params": {"name": tool, "arguments": args},
    }
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = json.loads(resp.read().decode("utf-8", "replace"))
    except Exception as e:  # noqa: BLE001 — 交互脚本需容忍一切网络错误
        return False, f"请求失败: {e}"
    if "error" in body and body["error"]:
        return False, f"MCP 错误: {body['error']}"
    result = body.get("result")
    text = json.dumps(result, ensure_ascii=False)
    # MCP 结果可能是 {content:[{type:text,text:...}], isError:true}
    if isinstance(result, dict):
        if result.get("isError"):
            return False, text
        content = result.get("content") or []
        if content and isinstance(content[0], dict) and "text" in content[0]:
            text = content[0]["text"]
    return True, text


# ─────────────────────────────── 检查步骤定义 ───────────────────────────────
# cmds: [(工具名, 参数字典), ...] — 步骤开始时自动依次发送
# verify: 人工核对点（在 GUI 里看什么）

def build_steps():
    cfg = {
        "probe_ch": PROBE_CHANNEL_INDEX,
        "rename_ch": RENAME_CHANNEL_INDEX,
        "rate": TEST_SAMPLE_RATE,
        "limit": TEST_SAMPLE_LIMIT,
    }
    return [
        # ── 准备 ──
        {
            "title": "MCP 连通性",
            "cmds": [("get_devices", {})],
            "verify": [
                "MCP 返回设备列表（JSON 数组或对象），无报错",
                "GUI 无异常弹出/崩溃",
            ],
            "note": "若失败，先确认 PXView GUI 已启动且 MCP 端口 10110 可达。",
        },
        {
            "title": "切到 Logic 模式（准备）",
            "cmds": [("switch_work_mode", {"mode": 0})],
            "verify": [
                "GUI 切换到 Logic 模式：采样栏/触发栏/DeviceOptionsDock 正常刷新",
                "无崩溃、无残留控件",
            ],
        },

        # ── 触发同步（修复核心）──
        {
            "title": "MCP configure_trigger → TriggerDock 滑条回填（原空函数修复）",
            "cmds": [("configure_trigger",
                      {"stageCount": 1,
                       "configJson": json.dumps({"mode": 0, "trigger_pos": 30})})],
            "verify": [
                "★ 核心检查：TriggerDock 的触发位置滑条/输入框自动变为 30",
                "Simple 触发单选按钮处于选中状态",
            ],
            "note": "修复前 update_view() 为空函数，滑条不会动。",
        },
        {
            "title": "GUI 反向：DeviceOptionsDock 触发前采样比例 → TriggerDock 同步",
            "cmds": [],  # 纯 GUI 操作
            "manual": "在 DeviceOptionsDock 里把「触发前采样比例」改为 70",
            "verify": [
                "TriggerDock 滑条立即同步为 70（CAPTURE_RATIO 双向同步钩子）",
            ],
        },
        {
            "title": "MCP set_config(CAPTURE_RATIO) → TriggerDock 回填 + Core 同步",
            "cmds": [("set_config",
                      {"key": KEY_CAPTURE_RATIO, "type": "uint64", "value": 45})],
            "verify": [
                "TriggerDock 滑条自动变为 45",
            ],
            "note": "修复前该路径只写驱动且零广播，GUI 不刷新、采集启动时被冲掉。",
        },

        # ── 通用 set_config 广播 ──
        {
            "title": "MCP set_config(RLE) → DeviceOptionsDock 刷新",
            "cmds": [("set_config",
                      {"key": KEY_RLE, "type": "bool", "value": True})],
            "verify": [
                "DeviceOptionsDock 的 RLE 控件刷新为勾选/true",
                "GUI 无异常（修复前零广播，dock 显示旧值）",
            ],
            "note": "若当前设备无 RLE 键，MCP 会返回 ConfigInvalid —— 属设备能力问题，"
                    "可换其它 GUI 可见的键（如 VTH/OperationMode）重试（按 m 重发前改脚本参数）。",
        },

        # ── 通道配置 ──
        {
            "title": "MCP configure_channel 改名 → GUI header 刷新",
            "cmds": [("configure_channel",
                      {"channelIndex": cfg["rename_ch"],
                       "name": "MCP_TEST_CH"})],
            "verify": [
                f"GUI header 第 {cfg['rename_ch']} 通道名变为 MCP_TEST_CH",
            ],
        },
        {
            "title": "MCP configure_channel 禁用/启用 → GUI 勾选刷新",
            "cmds": [("configure_channel",
                      {"channelIndex": cfg["rename_ch"], "enabled": False})],
            "verify": [
                f"GUI header 第 {cfg['rename_ch']} 通道勾选被取消",
            ],
            "after_cmds": [("configure_channel",
                            {"channelIndex": cfg["rename_ch"], "enabled": True})],
            "after_note": "验证后自动重新启用该通道（看 GUI 勾选恢复）。",
        },

        # ── 采样配置 ──
        {
            "title": "MCP set_sample_config → 采样栏刷新",
            "cmds": [("set_sample_config",
                      {"sampleRate": cfg["rate"],
                       "sampleLimit": cfg["limit"]})],
            "verify": [
                f"采样栏显示采样率 {cfg['rate']:,} Hz / 深度 {cfg['limit']:,}（或等效刻度）",
                "DeviceOptionsDock/MeasureDock 无异常刷新",
            ],
        },

        # ── 采集联动 ──
        {
            "title": "采集启动后触发位置生效（CAPTURE_RATIO 不被冲掉）",
            "cmds": [],
            "manual": "在 GUI 点一次「开始采集」（demo 设备，单次模式）",
            "verify": [
                "波形触发点落在约 45% 位置（与上一步 set_config 的值一致）",
                "采集结束后 TriggerDock 滑条仍是 45（未被 Core 陈旧值覆盖）",
            ],
        },

        # ── DSO 模式 ──
        {
            "title": "切到 DSO 模式（准备）",
            "cmds": [("switch_work_mode", {"mode": 2})],
            "verify": [
                "GUI 切换到 DSO 模式，DSO 触发 dock/测量 dock 正常出现",
            ],
            "note": "工作模式编号：0=Logic, 1=Analog, 2=DSO, 3=MSO。",
        },
        {
            "title": "MCP configure_probe → 探针控件刷新（原 3 参数不下发修复）",
            "cmds": [("configure_probe",
                      {"channelIndex": cfg["probe_ch"], "vdiv": 0.5,
                       "coupling": 1, "vfactor": 10, "mapDefault": True})],
            "verify": [
                f"★ 核心检查：GUI header 里 O0 通道（sr_channel index={cfg['probe_ch']}）"
                "的伏特/格变为 0.5V、耦合 DC、探针倍率 ×10、map 默认勾选",
                "MCP 返回 success（修复前 vdiv/coupling/mapDefault 根本没写驱动）",
            ],
            "note": "channelIndex 是 get_channels 返回的 sr_channel index，不是 GUI "
                    "显示序号；demo 的 A0=32..O1=38。vfactor 仅 DSO 类型通道持久化。",
        },
        {
            "title": "MCP get_probe_config 回读（原 double 读 uint64 失败修复）",
            "cmds": [("configure_probe", {"channelIndex": cfg["probe_ch"]})],
            "verify": [
                "返回 vdiv≈0.5、vfactor≈10、coupling=1(DC)、mapDefault=true",
                "（修复前 vfactor 因类型不匹配恒为默认值）",
            ],
        },
        {
            "title": "MCP set_dso_trigger_config → DsoTriggerDock 刷新",
            "cmds": [("configure_trigger",
                      {"source": 1, "slope": 1, "horizPos": 30})],
            "verify": [
                "DsoTriggerDock 的触发源变为 CH0、边沿变为下降沿、位置滑条变为 30",
            ],
            "note": "horizPos 单位与 GUI 滑条一致（0..100 整数百分比）。",
        },

        # ── 恢复 ──
        {
            "title": "恢复：切回 Logic 模式 + 复位触发位置",
            "cmds": [("switch_work_mode", {"mode": 0}),
                     ("configure_trigger",
                      {"stageCount": 1,
                       "configJson": json.dumps({"mode": 0, "trigger_pos": 10})})],
            "verify": [
                "GUI 回到 Logic 模式，TriggerDock 滑条回到 10",
            ],
        },
    ]


# ──────────────────────────────── 交互主循环 ────────────────────────────────
def ask(prompt):
    try:
        return input(prompt).strip().lower()
    except (EOFError, KeyboardInterrupt):
        return "q"


def run_step(step, url, send):
    """执行一个检查步骤。返回 'pass' / 'fail' / 'skip'。"""
    print("\n" + "─" * 72)
    print(f"  {step['title']}")
    print("─" * 72)

    cmds = step.get("cmds", [])
    if cmds:
        for tool, args in cmds:
            print(f"  → MCP: {tool}({json.dumps(args, ensure_ascii=False)})")
            if send:
                ok, text = mcp_call(url, tool, args)
                shown = text if len(text) <= 300 else text[:300] + " …"
                print(f"    {'OK ' if ok else 'ERR'} {shown}")
            else:
                print("    (no-send 模式，未发送)")

    if step.get("manual"):
        print(f"  ☞ 请在 GUI 手工操作: {step['manual']}")
    for i, v in enumerate(step.get("verify", []), 1):
        print(f"    核对点 {i}: {v}")
    if step.get("note"):
        print(f"    注: {step['note']}")

    while True:
        ans = ask("  [回车]=通过 / f=失败 / s=跳过 / m=重发MCP / q=退出 > ")
        if ans == "":
            # 通过后执行恢复命令（如有）
            for tool, args in step.get("after_cmds", []):
                print(f"  → MCP(恢复): {tool}({json.dumps(args, ensure_ascii=False)})")
                if send:
                    ok, text = mcp_call(url, tool, args)
                    print(f"    {'OK ' if ok else 'ERR'} {text[:200]}")
            if step.get("after_note"):
                print(f"    注: {step['after_note']}")
            return "pass"
        if ans == "f":
            comment = ask("    失败现象/备注（可空）> ")
            step["comment"] = comment
            return "fail"
        if ans == "s":
            return "skip"
        if ans == "m" and cmds:
            for tool, args in cmds:
                print(f"  → MCP(重发): {tool}({json.dumps(args, ensure_ascii=False)})")
                if send:
                    ok, text = mcp_call(url, tool, args)
                    print(f"    {'OK ' if ok else 'ERR'} {text[:300]}")
            continue
        if ans == "q":
            return "quit"
        print("    无效输入。")


def main():
    ap = argparse.ArgumentParser(description="PXView MCP→GUI 同步修复人工检查表")
    ap.add_argument("--url", default=DEFAULT_URL, help=f"MCP 地址（默认 {DEFAULT_URL}）")
    ap.add_argument("--no-send", action="store_true",
                    help="不实际发送 MCP 命令，仅打印（纯手工模式）")
    args = ap.parse_args()

    try:  # Windows 控制台中文输出兜底
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        pass

    send = not args.no_send
    print("╔" + "═" * 70 + "╗")
    print("║  PXView V1.6.4 · MCP→GUI 配置同步修复 · 人工检查表")
    print("╚" + "═" * 70 + "╝")
    print(f"  MCP 地址: {args.url}   发送模式: {'自动' if send else '仅打印(--no-send)'}")

    # 连通性预检
    if send:
        ok, text = mcp_call(args.url, "get_devices", {}, timeout=5)
        if ok:
            print("  预检: MCP 可达 ✓")
        else:
            print(f"  预检: MCP 不可达 ✗ ({text})")
            print("  请先启动 install.dir/bin/PXView.exe（GUI），再运行本脚本。")
            if ask("  仍要继续（纯手工核对）？[y/N] > ") != "y":
                return 2

    steps = build_steps()
    results = []  # (序号, 步骤, 结果)
    for i, step in enumerate(steps, 1):
        print(f"\n[{i}/{len(steps)}]", end="")
        step["no"] = i
        r = run_step(step, args.url, send)
        if r == "quit":
            print("\n  …提前退出。")
            break
        results.append((i, step["title"], r))

    # 汇总
    print("\n" + "═" * 72)
    print("  检查汇总")
    print("═" * 72)
    counts = {"pass": 0, "fail": 0, "skip": 0}
    for no, title, r in results:
        mark = {"pass": "✓ 通过", "fail": "✗ 失败", "skip": "— 跳过"}[r]
        print(f"  {mark}  [{no:2d}] {title}")
        if r == "fail":
            print(f"         备注: {title} → {steps[no - 1].get('comment', '')}")
        counts[r] += 1
    total = len(results)
    print(f"\n  合计: {total}  通过: {counts['pass']}  "
          f"失败: {counts['fail']}  跳过: {counts['skip']}")
    if counts["fail"]:
        print("  存在失败项 —— 请把上面的汇总与备注反馈给开发。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
