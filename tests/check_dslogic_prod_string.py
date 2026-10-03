#!/usr/bin/env python3
"""判定 DSLogic/DSCope 驱动的「已刷固件」判据是否与固件实际字符串一致。

不依赖硬件，纯静态核对三件事：
  1. protocol.h 里的 DSL_PROD_STRING_* 常量值
  2. 各 .fw 二进制里实际声明的 UTF-16LE product 字符串
  3. scan() 是否至少接受其中一个（否则 has_firmware 恒假）

用法：python tests/check_dslogic_prod_string.py [--res DIR]
退出码：0 一致 / 1 不一致 / 2 解析失败
"""
import argparse, os, re, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HDR = REPO/"libsigrok/src/hardware/dreamsourcelab-dslogic/protocol.h"
API = REPO/"libsigrok/src/hardware/dreamsourcelab-dslogic/api.c"
RES = REPO/"install.dir/share/PXView/res"

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--res",type=Path,default=RES)
    a=ap.parse_args()

    hdr=HDR.read_text(encoding="utf-8",errors="replace")
    api=API.read_text(encoding="utf-8",errors="replace")

    def dv(n):
        m=re.search(r'#define\s+'+n+r'\s+"([^"]*)"',hdr)
        return m.group(1) if m else None

    dsview=dv("DSL_PROD_STRING_DSVIEW"); up=dv("DSL_PROD_STRING_UPSTREAM")
    if not dsview:
        print("[fatal] protocol.h 找不到 DSL_PROD_STRING_DSVIEW",file=sys.stderr); return 2

    # 固件实际声明的字符串
    declared=set()
    if not a.res.is_dir():
        print(f"[fatal] res 目录不存在: {a.res}",file=sys.stderr); return 2
    for f in sorted(a.res.iterdir()):
        if f.suffix!=".fw": continue
        data=f.read_bytes()
        for m in re.finditer(rb'(?:[\x20-\x7e]\x00){10,}',data):
            s=m.group().decode("utf-16-le",errors="replace")
            if "Instrument" in s: declared.add(s)
    if not declared:
        print("[fatal] 未能从任何 .fw 中解出 product 字符串",file=sys.stderr); return 2

    accepted={x for x in (dsview,up) if x}
    # scan() 是否真的用了这些常量
    used = api.count("DSL_PROD_STRING_DSVIEW")>0 and "has_firmware" in api

    print(f"驱动接受: {sorted(accepted)}")
    print(f"固件声明: {sorted(declared)}")
    bad=[s for s in sorted(declared) if s not in accepted]
    if not used:
        print("\n[baled] scan() 未引用 DSL_PROD_STRING_DSVIEW —— 判据可能又漂回硬编码串")
        return 1
    if bad:
        print("\n=== 不一致：以下固件字符串驱动不接受 ===")
        for s in bad: print(f"  {s!r}")
        print("  => has_firmware 恒假，已刷固件的设备会被反复重刷固件。")
        return 1
    print("\n一致：所有固件声明的 product 字符串都在驱动接受集合内。")
    return 0

if __name__=="__main__":
    sys.exit(main())
