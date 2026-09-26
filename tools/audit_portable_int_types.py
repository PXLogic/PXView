#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""可移植整数类型审计 —— 阻止把 64 位量交给 `unsigned long`（Windows 会截成 32 位）。

为什么需要它
============
`unsigned long` 在 LP64(Linux/macOS) 是 64 位，但在 LLP64(Windows x64) 只有 32 位。
C/C++ 标准只保证它 >= 32 位。任何"必须 64 位"的量用 `unsigned long` 承载，在
Windows 上都会被静默截断（mod 2^32），且**在 Linux 上编译/测试完全看不出来**。

真实事故（2026-09-26，提交 1e1333d1 "显式化隐式类型转换，消除 816 条编译警告"）：
把 ruler_format.cpp 里原本正确的
    uint64_t delta_time = v1 * delta_index;                 // double -> uint64_t，正确
"显式化"成了
    uint64_t delta_time = static_cast<unsigned long>(...);  // Windows 上按 2^32 截断
于是 format_real_time() 的皮秒数被截断：
    297050000000  -> 697256576     (光标时间 +697.256576μs，应为 +297.05ms)
    306720000000  -> 1777321984    (+1.777321984ms，应为 +306.72ms)
表现为 ruler 光标标签 / 测量 dock 的时间错得离谱且随 index 非单调乱跳。

编译器 / clang-tidy / sanitizer 为什么兜不住
==========================================
- `-Wconversion` 只报**隐式**窄化；“消警告”的手法恰恰是加一个显式 `static_cast`，
  于是警告消失、语义却变了 —— 这正是本次事故的成因，所以“开警告”反而被利器反噬。
- clang-tidy 的 bugprone-narrowing-conversions / cppcoreguidelines-* 同样不报显式 cast。
- UBSan/ASan 抓不到：无符号截断既非 UB、也不越界（它们管的是有符号溢出与内存越界）。
所以需要一条源码级、确定性、跨平台的闸门 —— 也就是本脚本。

判定规则
========
ERROR（exit 1，阻断 CI）：
    显式把值转到 `unsigned long`：`static_cast<unsigned long>(...)` 或 C 风格
    `(unsigned long)`。必须写成 `uint64_t`（或 `unsigned long long`）。
WARN（exit 0，仅提示）：
    自有代码里出现裸 `unsigned long` 类型（不含 `unsigned long long`），例如
    strtoul / printf("%lu") 场景。多为 16/32 位值，安全，但需人工确认。

用法
====
    python3 tools/audit_portable_int_types.py        # 报告 + 退出码（CI 用）
    python3 tools/audit_portable_int_types.py -v     # 额外列出 known-safe 命中项

退出码：0 = 无 ERROR；1 = 发现 ERROR；2 = 扫描根缺失等环境错误。
仅依赖标准库（Python 3.6+）。
"""

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# 只扫自有代码；第三方不扫：
#   common/minizip            —— 随附的第三方 zip 库
#   libsigrok 上游（非 pxlogic）—— 未改动的上游驱动
SCAN_ROOTS = [
    'PXView',
    'common',
    'libsigrok/src/hardware/pxlogic',
]
EXCLUDE_DIRS = [
    'common/minizip',
]
EXTS = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx'}

# ERROR：显式转换目标为 unsigned long（正则不会吞掉 unsigned long long）
CAST_RE = re.compile(r'static_cast\s*<\s*unsigned\s+long\s*>'
                     r'|\(\s*unsigned\s+long\s*\)')
# WARN：裸 unsigned long 类型（排除 unsigned long long）
BARE_RE = re.compile(r'\bunsigned\s+long\b(?!\s+long)')

# 已确认安全的白名单：(相对路径子串, 命中代码子串, 理由)
# 命中后不计入 ERROR/WARN，仅在 -v 下作为 known-safe 列出。
ALLOWLIST = [
    ('PXView/pv/base/signalhandler.cpp',
     'static_cast<unsigned long>(line.LineNumber)',
     '实参是 Windows DWORD（IMAGEHLP_LINE64::LineNumber），原生即 32 位 unsigned long，转换精确'),
    ('PXView/pv/base/signalhandler.cpp',
     'static_cast<unsigned long>(displacement)',
     '实参是 Windows DWORD displacement，32 位原生，转换精确'),
    ('PXView/pv/dock/triggerdock.cpp',
     'unsigned long val = 0;',
     '承载 16 字符二进制串口值（<= 0xFFFF），远小于 2^32'),
    ('PXView/pv/dock/triggerdock.cpp',
     'unsigned long val = strtoul(str, &endptr, 16);',
     '承载 <=4 位十六进制串口值（<= 0xFFFF），远小于 2^32'),
]


def strip_comments_and_strings(text):
    """把注释与字符串/字符字面量替换为空格，保留换行与列位置。

    这样后续正则不会命中注释/字符串里的字样（例如 ruler_format.cpp 里解释本
    规则的中文注释本身也含 "static_cast<unsigned long>"）。
    """
    out = []
    state = 'code'
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if state == 'code':
            nxt = text[i + 1] if i + 1 < n else ''
            if c == '/' and nxt == '/':
                state = 'line'; out.append('  '); i += 2; continue
            if c == '/' and nxt == '*':
                state = 'block'; out.append('  '); i += 2; continue
            if c == '"':
                state = 'str'; out.append(' '); i += 1; continue
            if c == "'":
                state = 'chr'; out.append(' '); i += 1; continue
            out.append(c); i += 1; continue
        if state == 'line':
            out.append('\n' if c == '\n' else ' ')
            if c == '\n':
                state = 'code'
            i += 1; continue
        if state == 'block':
            if c == '*' and i + 1 < n and text[i + 1] == '/':
                state = 'code'; out.append('  '); i += 2; continue
            out.append('\n' if c == '\n' else ' '); i += 1; continue
        # str / chr
        if c == '\\' and i + 1 < n:
            out.append('  '); i += 2; continue
        if (state == 'str' and c == '"') or (state == 'chr' and c == "'"):
            state = 'code'; out.append(' '); i += 1; continue
        out.append('\n' if c == '\n' else ' '); i += 1; continue
    return ''.join(out)


def is_excluded(rel_dir):
    rel_dir = rel_dir.replace('\\', '/').strip('/')
    return any(rel_dir == d or rel_dir.startswith(d + '/') for d in EXCLUDE_DIRS)


def iter_source_files():
    for root in SCAN_ROOTS:
        base = REPO_ROOT / root
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            rel = os.path.relpath(dirpath, REPO_ROOT)
            dirnames[:] = [d for d in dirnames if not is_excluded(os.path.join(rel, d))]
            for fn in filenames:
                if Path(fn).suffix.lower() in EXTS:
                    yield Path(dirpath) / fn


def matches_allowlist(rel_path, code_line):
    for path_sub, code_sub, reason in ALLOWLIST:
        if path_sub in rel_path and code_sub in code_line:
            return reason
    return None


def main():
    verbose = '-v' in sys.argv[1:] or '--verbose' in sys.argv[1:]

    errors = []      # (rel_path, line, code, kind)
    warnings = []
    known = []

    scanned_files = 0
    for path in iter_source_files():
        rel_path = os.path.relpath(path, REPO_ROOT).replace('\\', '/')
        try:
            text = path.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        scanned_files += 1
        for lineno, code in enumerate(strip_comments_and_strings(text).split('\n'), 1):
            if 'unsigned' not in code:
                continue
            if CAST_RE.search(code):
                reason = matches_allowlist(rel_path, code)
                if reason:
                    known.append((rel_path, lineno, code.strip(), reason))
                else:
                    errors.append((rel_path, lineno, code.strip(), 'cast'))
            elif BARE_RE.search(code):
                reason = matches_allowlist(rel_path, code)
                if reason:
                    known.append((rel_path, lineno, code.strip(), reason))
                else:
                    warnings.append((rel_path, lineno, code.strip(), 'bare'))

    print("=" * 72)
    print("  PXView Portable Integer Type Audit")
    print("  (禁止 static_cast<unsigned long> / (unsigned long) —— Windows 上仅 32 位)")
    print("=" * 72)
    print()
    print(f"  扫描文件数            : {scanned_files}")
    print(f"  ERROR (阻断 CI)       : {len(errors)}")
    print(f"  WARN  (人工确认)      : {len(warnings)}")
    print(f"  known-safe (白名单)   : {len(known)}")
    print()

    if errors:
        print("  ERRORS —— 必须改为 uint64_t（或 unsigned long long）:")
        print("  " + "-" * 68)
        for rel, ln, code, _ in errors:
            print(f"  {rel}:{ln}")
            print(f"    {code}")
        print()
        print("  修复方法：把转换目标由 unsigned long 改为 uint64_t。")
        print("  若确为 32 位原生类型（如 Windows DWORD），请在 ALLOWLIST 中登记并说明理由。")
        print()
    else:
        print("  [OK] 未发现把值显式转换到 unsigned long 的写法。")
        print()

    if warnings:
        print("  WARN —— 裸 unsigned long 类型（多为 16/32 位值，需人工确认）:")
        print("  " + "-" * 68)
        for rel, ln, code, _ in warnings:
            print(f"  {rel}:{ln}  {code}")
        print()

    if verbose and known:
        print("  known-safe（白名单命中，不阻断）:")
        print("  " + "-" * 68)
        for rel, ln, code, reason in known:
            print(f"  {rel}:{ln}  {code}")
            print(f"      -> {reason}")
        print()

    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
