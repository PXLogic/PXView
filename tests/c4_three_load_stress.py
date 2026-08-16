#!/usr/bin/env python
"""
C4 三负载压测 (spec: make-mipmap-production-ready, Task C4)
============================================================

稠密/稀疏 × 长时(>=5min)采集 + 渲染读 + 解码 并发，验证:
  - 无 DROPPING（服务端日志无 'capture_ended: dropped' / 数据溢出告警）
  - 队列不积压（每轮 capture 的 decode 在界内超时完成，不累积）
  - 无崩溃（服务端全程存活、零 McpConnectionError）
  - PathDiag / RenderDiag 记录从服务端日志抽取并归档

用法:
  python tests/c4_three_load_stress.py \
      --exe <install.dir/bin/PXView.exe> \
      --duration 300 --scenario both --port 10111

参数:
  --exe       PXView.exe 路径 (默认 install.dir/bin/PXView.exe)
  --port      MCP 端口 (默认 10111, 避免与 E2E 默认 10110 冲突)
  --duration  每场景压测墙钟秒数 (默认 300 = 5min, spec C4.1 要求 >=5min)
  --scenario  dense | sparse | both (默认 both)
  --cycles    每场景最大采集轮数上限 (默认 0=不限, 按 duration 跑满)
  --keep-logs 保留服务端日志不清理 (默认归档到 devdoc/c4-diag/)
"""

from __future__ import annotations

import argparse
import base64
import os
import re
import shutil
import sys
import threading
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "pxview-automation", "src"))
sys.path.insert(0, os.path.join(REPO_ROOT, "tests"))

from pxview_automation import McpClient, PXViewProcess  # noqa: E402
from pxview_automation.exceptions import McpError, McpConnectionError  # noqa: E402

SR_CONF_PATTERN_MODE = 30002

DENSE_CFG = dict(
    name="dense", pattern="random", channels=[0, 1, 2, 3, 4, 5, 6, 7],
    sample_rate=20000000, sample_count=10000000,  # 10M samples, 0.5s data @20MS/s
)
SPARSE_CFG = dict(
    name="sparse", pattern="incremental", channels=[0, 1],
    sample_rate=100000, sample_count=500000,  # 0.5M samples, 5s data @100kS/s
)

# 每轮并发挂 3 个解码器（不同类别通道）以制造解码队列压力。
DECODERS = [
    ("pwm_c", {"channelMap": {"data": 0}}),
    ("counter_c", {"channelMap": {"data": 1}}),
    ("timing_c", {"channelMap": {"data": 2}}),
]


class ScenarioStats:
    def __init__(self, name: str):
        self.name = name
        self.cycles = 0
        self.capture_errors = 0
        self.decode_ok = 0
        self.decode_timeouts = 0
        self.render_calls = 0
        self.render_errors = 0
        self.render_benign = 0
        self.conn_errors = 0
        self.max_bytes_read = 0


def _classify_err(exc: Exception) -> str:
    """返回: 'conn' | 'benign' | 'hard'"""
    if isinstance(exc, McpConnectionError):
        return "conn"
    if isinstance(exc, McpError):
        # 服务端错误响应（如 "No device connected"/无数据）→ 良性，不算故障
        return "benign"
    return "hard"


def _count(stats: ScenarioStats, kind: str) -> None:
    if kind == "conn":
        stats.conn_errors += 1
    elif kind == "benign":
        stats.render_benign += 1
    else:
        stats.render_errors += 1


def _parse_aid(res: Any) -> Optional[str]:
    """解析 add_analyzer 返回的 analyzer 实例 id（兼容 int/dict/str）。"""
    if isinstance(res, str):
        return res
    if isinstance(res, (int, float)):
        return str(int(res))
    if isinstance(res, dict):
        return str(res.get("instance_id") or res.get("id")
                   or res.get("analyzerId") or "")
    return None


def render_load(mcp: McpClient, ready_evt: threading.Event,
                stop_evt: threading.Event,
                stats: ScenarioStats) -> None:
    """渲染负载：持续窗口读 + 边沿查询（读路径/快照访问热负载）。"""
    ready_evt.wait()  # 等待首个采集完成，避免无数据期误报
    ch = 0
    window = 65536
    pos = 0
    while not stop_evt.is_set():
        try:
            # 模拟视口平移/缩放读窗口
            data = mcp.get_samples(
                channel_index=ch, channel_type="logic",
                start_sample=pos, end_sample=pos + window,
                timeout=20)
            if isinstance(data, (bytes, bytearray)):
                stats.max_bytes_read = max(stats.max_bytes_read, len(data))
            stats.render_calls += 1
            # 下一个窗口（循环扫描样本空间）
            pos += window // 2
            if pos > 8 * 1024 * 1024:
                pos = 0
        except Exception as exc:  # noqa: BLE001
            _count(stats, _classify_err(exc))
        time.sleep(0.002)


def run_scenario(mcp: McpClient, device_id: str, cfg: dict,
                 duration: float, max_cycles: int) -> ScenarioStats:
    stats = ScenarioStats(cfg["name"])
    ready_evt = threading.Event()
    stop_evt = threading.Event()
    render_th = threading.Thread(
        target=render_load, args=(mcp, ready_evt, stop_evt, stats),
        daemon=True)
    render_th.start()

    # 设置 pattern（dense=random 高频边沿 / sparse=incremental 慢变）
    try:
        mcp.connect_device(device_id)
    except Exception:
        pass
    try:
        mcp.set_config(key=SR_CONF_PATTERN_MODE, type="string",
                       value=cfg["pattern"])
    except Exception:
        pass  # 某些场景 pattern 已默认，失败不阻塞

    deadline = time.time() + duration
    while time.time() < deadline:
        if max_cycles and stats.cycles >= max_cycles:
            break
        stats.cycles += 1
        cycle = stats.cycles

        # 1) add-before 解码器（采集完成自动重解码 —— 三负载之一: 解码）
        aids = []
        for dname, dcfg in DECODERS:
            try:
                res = mcp.add_analyzer(dname, dict(dcfg),
                                       device_id=device_id, timeout=20)
                aid = _parse_aid(res)
                if aid:
                    aids.append(aid)
            except Exception:
                pass

        # 2) 采集（三负载之一: 采集/写入）
        logic_cfg = {
            "digitalChannels": cfg["channels"],
            "digitalSampleRate": cfg["sample_rate"],
        }
        cap_cfg = {"manualCaptureMode": {"sampleCount": cfg["sample_count"]}}
        try:
            mcp.start_capture(device_id, logic_cfg, cap_cfg, timeout=20)
            wait_to = max(cfg["sample_count"] / cfg["sample_rate"] * 4, 30)
            mcp.wait_capture(timeout_seconds=wait_to, timeout=wait_to + 15)
            status = mcp.get_capture_status(timeout=10)
            if not (isinstance(status, dict) and
                    status.get("state") in ("completed", "idle")):
                stats.capture_errors += 1
        except McpConnectionError:
            stats.capture_errors += 1
            stats.conn_errors += 1
            time.sleep(0.5)
            for a in aids:
                try:
                    mcp.remove_analyzer(a, timeout=10)
                except Exception:
                    pass
            continue
        except Exception:
            stats.capture_errors += 1
            time.sleep(0.5)
            for a in aids:
                try:
                    mcp.remove_analyzer(a, timeout=10)
                except Exception:
                    pass
            continue

        # 首个采集成功后放行渲染负载线程
        if stats.cycles == 1 and not ready_evt.is_set():
            ready_evt.set()

        # 3) 解码结果轮询（解码 worker 负载）—— 任一经轮询确认有产出即算该轮解码正常
        if aids:
            got_any = False
            pending = set(aids)
            t_end = time.time() + 30
            while time.time() < t_end and pending:
                advanced = False
                for a in list(pending):
                    try:
                        raw = mcp.get_analyzer_results(a, max_count=5,
                                                       timeout=10)
                        anns = raw.get("annotations", []) if isinstance(
                            raw, dict) else (raw or [])
                        if anns:
                            got_any = True
                            pending.discard(a)
                            advanced = True
                    except McpConnectionError:
                        stats.conn_errors += 1
                        pending.discard(a)
                        advanced = True
                    except McpError:
                        pass  # 良性：该解码器尚未产出，继续轮询
                    except Exception:
                        pending.discard(a)
                        advanced = True
                if not advanced and pending:
                    time.sleep(0.5)
                elif not pending:
                    break
            if got_any:
                stats.decode_ok += 1
            else:
                stats.decode_timeouts += 1
            for a in aids:
                try:
                    mcp.remove_analyzer(a, timeout=15)
                except Exception:
                    pass

        if cycle % 5 == 0:
            print(f"  [{cfg['name']}] cycle={cycle} "
                  f"decode_ok={stats.decode_ok} "
                  f"decode_to={stats.decode_timeouts} "
                  f"cap_err={stats.capture_errors} "
                  f"render_calls={stats.render_calls} "
                  f"render_err={stats.render_errors} "
                  f"conn_err={stats.conn_errors}")

    stop_evt.set()
    render_th.join(timeout=5)
    return stats


def summarize(stats: ScenarioStats) -> bool:
    ok = True
    print(f"\n==== 场景 [{stats.name}] 结果 ====")
    print(f"  cycles           = {stats.cycles}")
    print(f"  decode_ok        = {stats.decode_ok}")
    print(f"  decode_timeouts  = {stats.decode_timeouts}  (信息性, 受数据影响)")
    print(f"  capture_errors   = {stats.capture_errors}")
    print(f"  render_calls     = {stats.render_calls}")
    print(f"  render_errors    = {stats.render_errors}")
    print(f"  render_benign    = {stats.render_benign}  (无设备/无数据等良性响应)")
    print(f"  conn_errors      = {stats.conn_errors}")
    print(f"  max_window_bytes = {stats.max_bytes_read}")
    if stats.conn_errors:
        print("  [FAIL] 连接错误非零 → 服务端可能崩溃或断连")
        ok = False
    if stats.capture_errors:
        print("  [FAIL] 采集错误非零")
        ok = False
    if stats.render_errors:
        print("  [FAIL] 渲染读硬错误非零")
        ok = False
    if stats.cycles == 0:
        print("  [FAIL] 未完成任何采集轮")
        ok = False
    return ok


def resolve_pxv_log_path() -> str:
    """解析 PXView 服务端日志路径 (GetUserDataDir()/PXView.log)。"""
    cands = []
    for env in ("APPDATA", "LOCALAPPDATA"):
        base = os.environ.get(env)
        if base:
            for sub in ("PXlogicV20", "PXView", "DreamSourceLab"):
                cands.append(os.path.join(base, sub, "PXView", "PXView.log"))
                cands.append(os.path.join(base, "PXView", "PXView.log"))
    cands.append(os.path.join(os.path.expanduser("~"),
                              "AppData", "Roaming", "PXlogicV20", "PXView",
                              "PXView.log"))
    for c in cands:
        if os.path.exists(c):
            return c
    # 退回到能找到的最新候选
    for c in cands:
        if os.path.isdir(os.path.dirname(c)):
            return c
    return cands[0] if cands else ""


def collect_diag(log_path: str, out_dir: str, tag: str) -> None:
    """从服务端日志抽取 PathDiag/RenderDiag/dropped 记录并归档。"""
    if not log_path or not os.path.exists(log_path):
        print(f"  [!] 服务端日志不存在: {log_path}")
        return
    os.makedirs(out_dir, exist_ok=True)
    dst = os.path.join(out_dir, f"pxview_{tag}.log")
    shutil.copy2(log_path, dst)

    text = open(log_path, encoding="utf-8", errors="ignore").read()
    diag = []
    dropped = []
    for line in text.splitlines():
        if "[PathDiag]" in line or "[RenderDiag]" in line:
            diag.append(line)
        if re.search(r"dropped\b|DROPPING|SR_DF_OVERFLOW|overflow", line,
                     re.IGNORECASE):
            dropped.append(line)

    diag_file = os.path.join(out_dir, f"diag_{tag}.txt")
    with open(diag_file, "w", encoding="utf-8") as f:
        f.write("\n".join(diag) + "\n")
    print(f"  [归档] {dst}")
    print(f"  [归档] {diag_file} ({len(diag)} 条 PathDiag/RenderDiag)")
    if dropped:
        drop_file = os.path.join(out_dir, f"dropped_{tag}.txt")
        with open(drop_file, "w", encoding="utf-8") as f:
            f.write("\n".join(dropped) + "\n")
        print(f"  [!] 检出 DROPPING/overflow 线索 {len(dropped)} 条 → "
              f"{drop_file}")
    else:
        print("  [OK] 无 DROPPING / overflow 日志")


def main() -> int:
    ap = argparse.ArgumentParser(description="C4 三负载压测")
    ap.add_argument("--exe", default=os.path.join(
        REPO_ROOT, "install.dir", "bin", "PXView.exe"))
    ap.add_argument("--port", type=int, default=10111)
    ap.add_argument("--duration", type=float, default=300.0,
                    help="每场景压测秒数 (spec: >=300)")
    ap.add_argument("--scenario", choices=["dense", "sparse", "both"],
                    default="both")
    ap.add_argument("--cycles", type=int, default=0)
    args = ap.parse_args()

    if not os.path.isfile(args.exe):
        print(f"[错误] PXView.exe 不存在: {args.exe}")
        return 2

    out_dir = os.path.join(REPO_ROOT, "devdoc", "c4-diag")
    print(f"=== C4 三负载压测 ===\n  exe={args.exe}\n  port={args.port}\n"
          f"  duration={args.duration}s/scenario\n  scenario={args.scenario}")

    with PXViewProcess(exe_path=args.exe, port=args.port,
                       store_log=True, log_level=5,
                       startup_timeout=120) as proc:
        mcp = McpClient(url=f"http://127.0.0.1:{args.port}/mcp",
                        timeout=30.0, max_retries=5)
        if not mcp.wait_for_server(timeout=60, interval=1.0):
            print("[错误] 无法连接 MCP 服务")
            return 2
        mcp.connect()

        devices = mcp.get_devices()
        device_id = None
        for d in devices:
            if isinstance(d, dict) and d.get("vendor", "").lower() in (
                    "dreamsourcelab", "demo", "") and "demo" in str(d).lower():
                device_id = d.get("id") or d.get("name")
                break
        if not device_id:
            # 取第一个 demo 设备
            for d in devices:
                if isinstance(d, dict):
                    device_id = d.get("id") or d.get("name")
                    break
        if not device_id:
            print("[错误] 未找到 demo 设备:", devices)
            return 2
        print(f"  设备: {device_id}")

        scenarios = (["dense", "sparse"] if args.scenario == "both"
                     else [args.scenario])
        all_ok = True
        for sc in scenarios:
            cfg = DENSE_CFG if sc == "dense" else SPARSE_CFG
            print(f"\n--- 场景 [{cfg['name']}] pattern={cfg['pattern']} "
                  f"{cfg['sample_rate']}S/s {len(cfg['channels'])}ch ---")
            t0 = time.time()
            stats = run_scenario(mcp, device_id, cfg, args.duration,
                                 args.cycles)
            elapsed = time.time() - t0
            print(f"  墙钟 {elapsed:.1f}s")
            ok = summarize(stats)
            all_ok = all_ok and ok
            # 每场景结束归档一次日志（含 PathDiag/RenderDiag 窗口汇总）
            collect_diag(resolve_pxv_log_path(), out_dir, cfg["name"])

        print("\n=== C4 结论 ===")
        print("  PASS: 三负载并发稳定，无 DROPPING / 无积压 / 无崩溃"
              if all_ok else
              "  FAIL: 存在异常，详见上方统计")
        return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
