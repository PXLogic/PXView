#!/usr/bin/env python3
"""判定 PXLogic 驱动是否在 scan 阶段按 logic_mode 区分设备变体。

不依赖硬件，纯静态核对六件事：
  1. supported_PX[] 表里同一 (vid,pid,usb_speed) 下是否真的存在多个变体
     （ch32 / ch16 Pro / ch16 Plus / ch16 Base 共用 0x16C0:0x05DC）
  2. logic_check_conf_profile() 是否真的去读 logic_mode 寄存器（8192 + 22*4）
  3. 该函数 claim 之前是否调用了 libusb_set_raw_io_default(hdl, 0) ——
     fork 版 libusb 的 RAW_IO 默认开启，16 字节寄存器读会被 WinUSB 拒绝
  4. 读不到时是否标成 PX_LOGIC_MODE_UNKNOWN（而不是回落 0 = 冒充 32 通道），
     以及设备已被本进程打开时是否复用其 profile->logic_mode
  5. scan() 选表时是否用 logic_mode 参与匹配、未识别时是否如实标 "model unknown"
  6. hw_usb_open() 的 profile 修正是否"整套"跟着换（sdi->model + 探针列表），
     以及是否把 model_unknown 也当成触发条件

用法：python tests/check_pxlogic_profile_scan.py [--c FILE] [--h FILE]
退出码：0 一致 / 1 不一致 / 2 解析失败
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PXLOGIC_C = REPO / "libsigrok/src/hardware/pxlogic/pxlogic.c"
PXLOGIC_H = REPO / "libsigrok/src/hardware/pxlogic/pxlogic.h"

# 表项头部：{vid, pid, LIBUSB_SPEED_x, logic_mode, "vendor", "model",
ENTRY_RE = re.compile(
    r'\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*'
    r'(LIBUSB_SPEED_\w+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"'
)
MASK_RE = re.compile(r'\(1\s*<<\s*(\w+)\)')
DEFAULT_RE = re.compile(r'SR_Gn\(\d+\)\s*,\s*0\s*,\s*(\w+)\s*,')


def strip_comments(src):
    """去掉 /* */ 与 // 注释，避免注释里提到的函数名干扰顺序判断。"""
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    src = re.sub(r'//[^\n]*', ' ', src)
    return src


def body_of(src, signature):
    """取出某个函数体（按大括号配对，从 signature 后第一个 { 开始）。"""
    i = src.find(signature)
    if i < 0:
        return None
    i = src.find("{", i + len(signature))
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    return None


def parse_profiles(hdr):
    start = hdr.find("supported_PX[] = {")
    if start < 0:
        return None
    end = hdr.find("\n};", start)
    body = hdr[start:end if end > 0 else len(hdr)]

    hits = list(ENTRY_RE.finditer(body))
    profiles = []
    for n, m in enumerate(hits):
        stop = hits[n + 1].start() if n + 1 < len(hits) else len(body)
        caps = body[m.end():stop]
        dflt = DEFAULT_RE.search(caps)
        profiles.append({
            "vid": int(m.group(1), 16),
            "pid": int(m.group(2), 16),
            "speed": m.group(3),
            "logic_mode": int(m.group(4)),
            "vendor": m.group(5),
            "model": m.group(6),
            "mask": MASK_RE.findall(caps),
            "default": dflt.group(1) if dflt else None,
        })
    return profiles


def parse_enum(hdr):
    start = hdr.find("enum PX_CHANNEL_ID {")
    if start < 0:
        return None
    end = hdr.find("}", start)
    names = re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:=|,)', hdr[start:end])
    return {n: i for i, n in enumerate(names)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--c", type=Path, default=PXLOGIC_C)
    ap.add_argument("--h", type=Path, default=PXLOGIC_H)
    a = ap.parse_args()

    src = a.c.read_text(encoding="utf-8", errors="replace")
    hdr = a.h.read_text(encoding="utf-8", errors="replace")

    profiles = parse_profiles(hdr)
    enum = parse_enum(hdr)
    if not profiles:
        print("[fatal] 未能从 pxlogic.h 解析出 supported_PX[]", file=sys.stderr)
        return 2
    if not enum:
        print("[fatal] 未能从 pxlogic.h 解析出 enum PX_CHANNEL_ID", file=sys.stderr)
        return 2

    print(f"表项数: {len(profiles)}，通道模式枚举 {len(enum)} 项")

    # --- 1. (vid,pid,speed,logic_mode) 唯一性 --------------------------------
    seen = {}
    dup = []
    for p in profiles:
        k = (p["vid"], p["pid"], p["speed"], p["logic_mode"])
        if k in seen:
            dup.append(k)
        seen[k] = p
    if dup:
        print("\n=== 不一致：设备表存在重复的 (vid,pid,speed,logic_mode) ===")
        for k in dup:
            print(f"  0x{k[0]:04X}:0x{k[1]:04X} {k[2]} logic_mode={k[3]}")
        print("  => 变体无法被 logic_mode 唯一区分。")
        return 1

    # --- 2. 同一 (vid,pid,speed) 下的变体分组 --------------------------------
    groups = {}
    for p in profiles:
        groups.setdefault((p["vid"], p["pid"], p["speed"]), []).append(p)
    multi = {k: v for k, v in groups.items() if len(v) > 1}
    for k, v in sorted(multi.items()):
        print(f"  0x{k[0]:04X}:0x{k[1]:04X} {k[2]}: " +
              ", ".join(f"logic_mode={p['logic_mode']} -> {p['model']}" for p in v))
    if not multi:
        print("\n[note] 表中没有共用 (vid,pid,speed) 的多变体，scan 无需读 logic_mode。")
        return 0

    problems = []

    # --- 3. scan 阶段必须真的读 logic_mode 寄存器 ---------------------------
    ckp = body_of(src, "logic_check_conf_profile(")
    if not ckp:
        problems.append("找不到 logic_check_conf_profile() 函数体")
    else:
        code = strip_comments(ckp)
        if not re.search(r'usb_rd_reg\s*\(', code):
            problems.append(
                "logic_check_conf_profile() 没有调用 usb_rd_reg —— scan 阶段读不到 "
                "logic_mode，多变体设备会被当成表里第一个（logic_mode=0）的型号")
        elif not re.search(r'8192\s*\+\s*22\s*\*\s*4', code):
            problems.append(
                "logic_check_conf_profile() 读的寄存器地址不是 logic_mode(8192 + 22*4)")

        # ★ claim 之前必须关掉 RAW_IO。fork 版 libusb 的 libusb_open() 把每个
        # handle 的 raw_io_default 初始化为 1，claim 时会给所有 IN 端点打开
        # RAW_IO；RAW_IO 要求传输长度是最大包长（USB3.0=1024）的整数倍，
        # 16 字节寄存器读会被 WinUSB 直接拒绝（ERROR_INVALID_FUNCTION=1）。
        raw = code.find("libusb_set_raw_io_default")
        claim = code.find("libusb_claim_interface")
        if raw < 0:
            problems.append(
                "logic_check_conf_profile() claim 之前没有调用 "
                "libusb_set_raw_io_default(hdl, 0) —— RAW_IO 默认开启，"
                "16 字节寄存器读必然失败（日志表现为 "
                "\"detected I/O error 1\" / LIBUSB_ERROR_IO）")
        elif claim >= 0 and raw > claim:
            problems.append(
                "logic_check_conf_profile() 的 libusb_set_raw_io_default 出现在 "
                "libusb_claim_interface 之后 —— RAW_IO 管道策略是在 claim 里设的，"
                "必须在 claim 之前关")

        # ★ 设备已被本进程打开时必须复用它的 logic_mode，不能直接回落 0。
        # 拔插后 libusb_open 会报 LIBUSB_ERROR_ACCESS（PXView 掉线时不关句柄），
        # 回落 0 就会把 16 Pro 显示成 "channel 32"。
        if "profile->logic_mode" not in code:
            problems.append(
                "logic_check_conf_profile() 没有从已打开的活动实例复用 "
                "profile->logic_mode —— 设备被本进程占用（拔插后 libusb_open "
                "报 LIBUSB_ERROR_ACCESS）时会回落成 0，型号显示错误")

        # ★ "读不到" 不能与 "确实是 logic_mode=0（ch32 变体）" 混为一谈。
        # 读不到就回落 0 = 冒充 32 通道机型，是本 bug 反复出现的放大器。
        if not re.search(r'\*logic_mode\s*=\s*PX_LOGIC_MODE_UNKNOWN', code):
            problems.append(
                "logic_check_conf_profile() 没有把读不到的情况标成 "
                "PX_LOGIC_MODE_UNKNOWN —— 回落 0 等于冒充 32 通道机型")
        if re.search(r'\*logic_mode\s*=\s*0\s*;', code):
            problems.append(
                "logic_check_conf_profile() 仍在把 *logic_mode 置 0 —— 0 是设备表里"
                "合法的 ch32 变体值，不能用它表示\"没读到\"")

    # --- 4. scan() 选表时必须用 logic_mode 匹配 ------------------------------
    scan = body_of(src, "static GSList *scan(")
    if not scan:
        problems.append("找不到 scan() 函数体")
    else:
        scode = strip_comments(scan)
        if not re.search(r'logic_mode\s*==\s*supported_PX\[\w+\]\.logic_mode', scode):
            problems.append("scan() 选表时没有比较 logic_mode —— 变体区分失效")

        # ★ 未识别时必须如实标出来，不能冒用表里第一条的型号。
        if "model_unknown" not in scode or "model unknown" not in scode:
            problems.append(
                "scan() 没有处理\"型号未识别\"：应把 devc->model_unknown 置真并给设备"
                "一个中性名（如 \"PX-Logic U3 (model unknown)\"），而不是冒用表里"
                "第一条的 \"channel 32\"")

    # --- 5. hw_usb_open() 的修正必须同步型号名与探针 -------------------------
    openf = body_of(src, "static int hw_usb_open(")
    if not openf:
        problems.append("找不到 hw_usb_open() 函数体")
    else:
        ocode = strip_comments(openf)
        if not re.search(r'8192\s*\+\s*22\s*\*\s*4', ocode):
            problems.append("hw_usb_open() 没有兜底读 logic_mode（scan 读失败时无从修正）")
        if "sr_dev_inst_model_set" not in ocode:
            problems.append(
                "hw_usb_open() 修正 profile 后没有更新 sdi->model —— 设备选项栏/设备列表"
                "仍显示旧型号（16 Pro 显示成 channel 32）")
        if not re.search(r'setup_probes\s*\(', ocode):
            problems.append(
                "hw_usb_open() 修正 profile 后没有重建探针 —— 通道数仍是旧 profile 的数量")
        # scan 标了 model_unknown 时，即使读回来的 logic_mode 与占位 profile 相同
        # （真的就是 ch32、值 0），也必须走修正把型号名换回来。
        if "model_unknown" not in ocode:
            problems.append(
                "hw_usb_open() 的修正条件没有考虑 devc->model_unknown —— 扫描阶段没识别出"
                "型号、而设备恰好真是 32 通道时，名字会永远停在 \"model unknown\"")

    # --- 6. 每个 profile 的 default_channelmode 必须在自己支持的通道模式里 ----
    for p in profiles:
        d = p["default"]
        if d is None:
            problems.append(f"{p['model']}: 未解析到 default_channelmode")
            continue
        if d not in enum:
            problems.append(f"{p['model']}: default_channelmode {d} 不在 enum PX_CHANNEL_ID 中")
            continue
        if d not in p["mask"]:
            problems.append(
                f"{p['model']}: default_channelmode {d} 不在自身 dev_caps.channels 掩码内")

    if problems:
        print("\n=== 不一致 ===")
        for s in problems:
            print(f"  - {s}")
        print("\n  => 多变体设备会在设备列表 / 设备选项栏里显示成错误型号，"
              "通道数也按错误 profile 建立。")
        return 1

    print("\n一致：scan 阶段读 logic_mode，选表与 profile 修正均完整。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
