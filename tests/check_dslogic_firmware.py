#!/usr/bin/env python3
"""DSLogic / DSCope 固件完整性静态检查。

背景
----
`libsigrok/src/hardware/dreamsourcelab-dslogic/` 支持 25 个设备 profile
（14 个 DSLogic + 11 个 DSCope，含 USB2/USB3 重复项）。每个 profile 声明两个
固件名：

  1. `.fw`  — FX2 USB 重枚举固件，由 scan() 里的 ezusb_upload_firmware() 在
              设备首次插入（尚未跑固件）时上传。设备跑完它才会以
              "DreamSourceLab USB-based Instrument" 重新枚举成可用设备。
              注意：`.fw` **不是每型号一份**。FX2（Cypress CY7C68013A）只负责
              重枚举，全族共用一份；真正区分型号的是 `.bin`。
              世上只存在 4 个 `.fw`：DSLogic.fw / DSLogicPro.fw / DSCope.fw /
              DSCope20.fw。旧 fork dsl.h 里那 25 个不同的 `.fw` 名字有 21 个
              从未被任何人分发过，照抄过来必然上传失败。
  2. `.bin` — FPGA bitstream，由 dslogic_fpga_firmware_upload() 在 dev_open()
              里上传。**这一项才是逐型号独立的**，21 个 bitstream 与 profile
              一一对应。

如果 `.fw` 缺失，设备永远不重枚举，但 libusb 句柄已经被 scan() 打开过，
于是用户看到的是「设备被占用」+「驱动有问题」这类**误导性**报错，而不是
真正的「固件文件缺失」。这个脚本就是为了在编译期/提交期把这种缺口暴露出来。

用法
----
    python tests/check_dslogic_firmware.py            # 检查默认 res 目录
    python tests/check_dslogic_firmware.py --res DIR  # 指定 res 目录
    python tests/check_dslogic_firmware.py --quiet    # 只输出摘要

退出码
------
    0  全部固件齐备
    1  存在缺失固件
    2  解析失败 / 找不到源文件
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
API_C = REPO_ROOT / "libsigrok" / "src" / "hardware" / "dreamsourcelab-dslogic" / "api.c"
DEFAULT_RES = REPO_ROOT / "install.dir" / "share" / "PXView" / "res"

TABLE_START = "static const struct DSL_profile supported_device[] = {"


def split_entries(block: str) -> list[str]:
    """按大括号深度切分 profile 表格，返回每个顶层 {...} 的内容。"""
    entries: list[str] = []
    depth = 0
    cur = ""
    started = False
    for ch in block:
        if ch == "{":
            depth += 1
            if depth == 1:
                cur = ""
                started = True
                continue
        elif ch == "}":
            depth -= 1
            if depth == 0:
                if started:
                    entries.append(cur)
                started = False
                continue
        if started:
            cur += ch
    return entries


def parse_profiles(api_path: Path) -> list[dict]:
    try:
        src = api_path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise SystemExit(f"[fatal] 无法读取 {api_path}: {exc}") from exc

    try:
        start = src.index(TABLE_START) + len(TABLE_START)
    except ValueError:
        raise SystemExit(f'[fatal] 在 {api_path} 中找不到 "{TABLE_START}"') from None
    try:
        end = src.index("\n};", start)
    except ValueError:
        raise SystemExit("[fatal] profile 表格没有正常闭合（找不到 '\\n};'）") from None

    profiles: list[dict] = []
    for entry in split_entries(src[start:end]):
        if "DS_VENDOR_ID" not in entry and "0x2A0E" not in entry:
            continue
        pid_m = re.search(r"(?:DS_VENDOR_ID|0x2A0E)\s*,\s*(0x[0-9A-Fa-f]+)", entry)
        if not pid_m:
            continue
        strs = re.findall(r'"([^"]*)"', entry)
        if len(strs) < 4:
            continue
        velocity = "USB3 (SUPER)" if "LIBUSB_SPEED_SUPER" in entry else "USB2 (HIGH)"
        profiles.append(
            {
                "pid": pid_m.group(1),
                "vendor": strs[0],
                "model": strs[1],
                "fw": strs[2],          # FX2 重枚举固件
                "bit33": strs[3],       # FPGA bitstream
                "bit50": strs[4] if len(strs) > 4 else strs[3],
                "speed": velocity,
            }
        )
    return profiles


def main() -> int:
    ap = argparse.ArgumentParser(description="DSLogic/DSCope 固件完整性检查")
    ap.add_argument("--res", type=Path, default=DEFAULT_RES,
                    help=f"res 固件目录（默认 {DEFAULT_RES}）")
    ap.add_argument("--quiet", action="store_true", help="只输出摘要与缺失项")
    args = ap.parse_args()

    profiles = parse_profiles(API_C)
    if not profiles:
        print("[fatal] 未解析到任何 profile", file=sys.stderr)
        return 2

    res_dir: Path = args.res
    if not res_dir.is_dir():
        print(f"[fatal] res 目录不存在: {res_dir}", file=sys.stderr)
        return 2
    present = {p.name for p in res_dir.iterdir() if p.is_file()}

    # 统计：同一个 .fw 可能被多个 profile 共用（如 USB2/USB3 双条目）
    fw_users: dict[str, list[str]] = {}
    bin_users: dict[str, list[str]] = {}
    for prof in profiles:
        tag = f"{prof['pid']} {prof['model']} [{prof['speed']}]"
        fw_users.setdefault(prof["fw"], []).append(tag)
        for bit in {prof["bit33"], prof["bit50"]}:
            bin_users.setdefault(bit, []).append(tag)

    missing_fw = sorted(f for f in fw_users if f not in present)
    missing_bin = sorted(b for b in bin_users if b not in present)

    if not args.quiet:
        print(f"源文件 : {API_C.relative_to(REPO_ROOT)}")
        print(f"res 目录: {res_dir.relative_to(REPO_ROOT) if res_dir.is_relative_to(REPO_ROOT) else res_dir}")
        print(f"profile : {len(profiles)} 个")
        print()
        print(f"{'FX2 固件 (.fw)':<34}{'状态':<14}使用它的 profile")
        print("-" * 96)
        for fw in sorted(fw_users):
            state = "OK" if fw in present else "*** 缺失 ***"
            print(f"{fw:<34}{state:<14}{', '.join(fw_users[fw])}")
        print()
        print(f"{'FPGA 固件 (.bin)':<34}{'状态':<14}使用它的 profile")
        print("-" * 96)
        for bit in sorted(bin_users):
            state = "OK" if bit in present else "*** 缺失 ***"
            print(f"{bit:<34}{state:<14}{', '.join(bin_users[bit])}")
        print()

    print(f"FX2  固件: 需要 {len(fw_users):>2} 个，缺失 {len(missing_fw):>2} 个")
    print(f"FPGA 固件: 需要 {len(bin_users):>2} 个，缺失 {len(missing_bin):>2} 个")

    if missing_fw:
        print()
        print("=== 缺失的 FX2 固件（设备会无法识别 / 报「占用」或「驱动问题」）===")
        for fw in missing_fw:
            print(f"  {fw}")
            for user in fw_users[fw]:
                print(f"      - {user}")
        print()
        print("修复：把对应 .fw 放进")
        print(f"      {res_dir}")
        print("    只存在 4 个合法 .fw：DSLogic.fw / DSLogicPro.fw /")
        print("    DSCope.fw / DSCope20.fw（可从 DSView 官方安装包 DSView/res/ 取）。")
        print("    若缺的名字不在这 4 个里，说明 profile 表的 .firmware 字段被改回了")
        print("    旧 fork 的逐型号命名——那是错的，应改回族内共用的 4 个之一。")

    if missing_bin:
        print()
        print("=== 缺失的 FPGA bitstream ===")
        for bit in missing_bin:
            print(f"  {bit}")

    if missing_fw or missing_bin:
        return 1

    print("\n全部固件齐备。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
