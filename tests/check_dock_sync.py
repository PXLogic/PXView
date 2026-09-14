#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DeviceOptionsDock 通道同步专项检查

你在 GUI 里核对后按回车记录结果。
  [回车]=正常  f=不正常  r=重试本步骤  s=跳过  q=退出
用法:
    python tests/check_dock_sync.py            # 需先启动 PXView GUI
    python tests/check_dock_sync.py --ch 3     # 指定测试通道（默认 0）
"""

import argparse
import json
import sys
import time
import urllib.request


def call(name, args, url="http://127.0.0.1:10110/", timeout=20):
    payload = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
               "params": {"name": name, "arguments": args}}
    req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = json.loads(r.read().decode())
    res = body.get("result", {})
    if isinstance(res, dict) and res.get("isError"):
        raise RuntimeError(json.dumps(res, ensure_ascii=False)[:200])
    c = res.get("content") if isinstance(res, dict) else None
    return c[0]["text"] if c and "text" in c[0] else json.dumps(res, ensure_ascii=False)


def ch_state(idx):
    chs = json.loads(call("get_channels", {}))
    for c in chs:
        if c["index"] == idx:
            return c
    return None


def ask():
    return input("    [回车]=正常 / f=不正常 / r=重试 / s=跳过 / q=退出 > ").strip().lower()


def step(no, title, checks, action=None, manual=None, pause=2.5, results=None):
    while True:
        print(f"\n[{no}] {title}")
        if action:
            try:
                action()
            except Exception as e:
                print(f"    MCP 调用失败: {e}")
        if manual:
            print(f"    ☞ 请在 GUI 手工操作: {manual}")
        time.sleep(pause)
        for i, c in enumerate(checks, 1):
            print(f"    核对{i}: {c}")
        a = ask()
        if a == "":
            results.append((no, title, "pass"))
            return True
        if a == "f":
            results.append((no, title, "fail"))
            return False
        if a == "r":
            print("    ---- 重试本步骤 ----")
            continue
        if a == "s":
            results.append((no, title, "skip"))
            return True
        if a == "q":
            raise KeyboardInterrupt
        print("    无效输入")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ch", type=int, default=0, help="测试用通道号（默认 0）")
    args = ap.parse_args()
    ch = args.ch
    results = []

    # 连通性
    try:
        call("get_work_mode", {})
    except Exception as e:
        print(f"MCP 不可达: {e}\n请先启动 PXView GUI。")
        return 2

    print(f"== DeviceOptionsDock 通道同步检查（测试通道 {ch}）==")
    cur = ch_state(ch)
    print(f"当前通道 {ch}: name={cur['name'] if cur else '?'} enabled={cur['enabled'] if cur else '?'}")

    try:
        step(1, "MCP 禁用通道 → header + dock 同步", [
            f"header 里通道 {ch} 的行消失（其余通道上移补位）",
            "DeviceOptionsDock 数字通道网格里该按钮变灰",
            "波形区没有残留色块/空槽",
        ], action=lambda: call("configure_channel", {"channelIndex": ch, "enabled": False}),
           results=results)

        step(2, "MCP 重新启用 → header + dock 同步", [
            f"header 里通道 {ch} 的行回到原位置",
            "dock 网格里该按钮恢复彩色",
            "波形区重新出现该通道行",
        ], action=lambda: call("configure_channel", {"channelIndex": ch, "enabled": True}),
           results=results)

        step(3, "GUI 反向：在 dock 网格里手动点击该通道按钮（禁用）", [
            "header 行消失、波形区无残留",
            "MCP 读回状态一致（脚本自动核对）",
        ], manual=f"点击 dock 数字通道网格中的 {ch} 按钮使其变灰",
           action=lambda: time.sleep(0.5), results=results)
        cur = ch_state(ch)
        print(f"    MCP 读回: enabled={cur['enabled'] if cur else '?'}"
              f"  （应为 False，若不一致说明 dock 写入路径有问题）")

        step(4, "GUI 反向：再点击该按钮（启用）", [
            "header 行回到原位",
            "MCP 读回状态一致",
        ], manual=f"再点击 dock 网格中的 {ch} 按钮使其变彩色",
           action=lambda: time.sleep(0.5), results=results)
        cur = ch_state(ch)
        print(f"    MCP 读回: enabled={cur['enabled'] if cur else '?'}（应为 True）")

        step(5, "dock「全部禁用」按钮", [
            "header 所有逻辑通道行全部消失",
            "dock 网格全部变灰",
            "波形区无残留色块",
        ], manual="点击 dock 的「全部禁用」按钮", results=results)

        step(6, "dock「全部启用」按钮 → 通道顺序核对", [
            "所有通道行按 原顺序 回来（D0,D1,D2...在各自原位，没有通道跑到尾部）",
            "dock 网格全部恢复彩色",
        ], manual="点击 dock 的「全部启用」按钮", results=results)

        step(7, "MCP 改名 + 禁用组合（复测灰行残留）", [
            f"通道 {ch} 行消失、无灰行残留",
            "dock 网格变灰",
        ], action=lambda: (call("configure_channel",
                                {"channelIndex": ch, "name": f"MCP_SYNC_{ch}"}),
                           call("configure_channel",
                                {"channelIndex": ch, "enabled": False})),
           results=results)

        step(8, "清理：恢复通道名与启用状态", [
            "GUI 恢复正常",
        ], action=lambda: (call("configure_channel",
                                {"channelIndex": ch, "enabled": True}),
                           call("configure_channel",
                                {"channelIndex": ch, "name": f"D{ch}"})),
           results=results)

    except KeyboardInterrupt:
        print("\n提前退出。")

    print("\n== 汇总 ==")
    fail = 0
    for no, title, r in results:
        mark = {"pass": "✓", "fail": "✗", "skip": "—"}[r]
        if r == "fail":
            fail += 1
        print(f"  {mark} [{no}] {title}")
    print(f"失败: {fail}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
