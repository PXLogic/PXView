#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跨线程标志审计工具 —— 找出"非 atomic 标量成员"及其引用点所属函数。

为什么需要它
------------
本库的约定是：会被多个线程访问的标志必须是 std::atomic（见
pv/data/snapshot/snapshot.h 的 _memory_failed 注释）。2026-09 的缺口审计里，
_glitch_filtered / _glitch_filter_auto_apply 就是这样被找出来并改掉的。
人工 grep 只能看见"名字出现在哪些文件"，看不出"写它的是哪个线程"，
所以本工具多做一步：把每个引用点**归属到它所在的函数**，配合下面这张
线程角色表，就能判断是否存在"worker 写 + GUI 读"（或两个 worker 互写）。

线程角色表（判断依据，随代码演进需复核）
----------------------------------------
  main            GUI/主线程：View、Dock、Dialog、Api/MCP 入口（已 marshal）、
                  DsTimer 回调、SigSession 的**命令**入口（exec_capture 等）
  datafeed        libsigrok 数据馈送线程：IDataCallback / IDataCallback 事件
                  （DataUpdated、RevEndPacket、EndCollectWork…）同步回调进 SigSession::on_event
  dx_worker       DecodeTaskManager 的解码线程池
  filter_worker   FilterProcessor 的 _filter_pool 线程
  device_worker   采集/设备线程（SessionStopped、DeviceSessionStopped 等回调）

注意：DocumentRegistry 的拷贝线程已不存在 —— copy_data_to_document 现在是零拷贝
shared_ptr 交接（documentregistry.cpp 的 "join_copy_thread removed" 注释），因此
SessionDocument 的写入方跑在**调用者线程**（event_dispatcher 的订阅者 = 主线程）。

用法
----
    python tests/scan_cross_thread_flags.py             # 全量表格
    python tests/scan_cross_thread_flags.py _repeat_    # 只看名字含该子串的成员
    python tests/scan_cross_thread_flags.py --workers   # 只看有 worker 侧引用的成员

`--workers` 是真正要看的那一档：只有"至少有一处引用落在 worker 文件里"的成员
才可能是竞争候选，其余全是主线程内部访问（例如 View 的成员被 dock 读写）。

两个已知的粗筛局限（结论必须回读代码确认，不要直接采信本工具）：
  1. `-w` 按**名字**匹配，会混淆不同类里的同名成员
     （SessionDocument::_samplerate / SessionSnapshot::_samplerate /
     SearchResultModel::_samplerate）。
  2. 所属函数靠"向上找最近的定义行"猜测，超长函数会猜错
     （曾把 sigsession.cpp:2798 报成 get_capture_owner_document，实际属
     on_rev_end_packet）。定位到候选后务必回读上下文。
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 被扫描的共享类（多线程访问的候选面）
HEADERS = [
    'PXView/pv/data/snapshot/snapshot.h',
    'PXView/pv/data/snapshot/logicsnapshot.h',
    'PXView/pv/data/snapshot/analogsnapshot.h',
    'PXView/pv/data/snapshot/dsosnapshot.h',
    'PXView/pv/data/snapshot/leaf_block_pool.h',
    'PXView/pv/data/document/sessiondata.h',
    'PXView/pv/data/document/sessiondocument.h',
    'PXView/pv/data/stack/decoderstack.h',
    'PXView/pv/data/stack/spectrumstack.h',
    'PXView/pv/data/stack/mathstack.h',
    'PXView/pv/data/model/signalmodel.h',
    'PXView/pv/core/cursorregistry.h',
    'PXView/pv/core/documentregistry.h',
    'PXView/pv/core/decodetaskmanager.h',
    'PXView/pv/core/capturemanager.h',
    'PXView/pv/core/datafeedparser.h',
    'PXView/pv/core/filterprocessor.h',
    'PXView/pv/core/measurecalculator.h',
    'PXView/pv/session/sigsession.h',
]

# 已知跑在非主线程的文件（粗筛；精确判定看「所属函数」再对照上面的角色表）
WORKER_FILES = {
    'datafeedparser.cpp': 'datafeed',
    'filterprocessor.cpp': 'filter_worker',
    'decoderstack.cpp': 'dx_worker',
    'capturemanager.cpp': 'device_worker',
    'logicsnapshot.cpp': 'datafeed',
    'logicsnapshot_glitch_filter.cpp': 'filter_worker',
    # sessiondocument.cpp 曾是 copy_thread；零拷贝改造后写入方 = 调用者线程（主线程），
    # 保留在表里只为把这一档排除出 --workers 列表。
    'sessiondocument.cpp': 'main',
    'measurecalculator.cpp': 'main',
}

MEMBER_RE = re.compile(
    r'^\s*(?:mutable\s+)?(?:volatile\s+)?'
    r'(bool|int|int8_t|int16_t|int32_t|int64_t|uint8_t|uint16_t|uint32_t|uint64_t|'
    r'size_t|double|float|std::size_t)\s+(_\w+)\s*(?:=[^;]*)?;')

# 函数定义/方法定义行（用于把引用点归属到所属函数）
FUNC_RE = re.compile(
    r'^[A-Za-z_][\w:<>,&\*\s]*\b(\w+::)?(\w+)\s*\([^;]*$|^(void|bool|int|int64_t|uint64_t|'
    r'std::\w+|auto|QString|json)\s+\w+::\w+\s*\(')


def enclosing_function(path, line_no):
    """向上找最近的方法/函数定义行，返回 'Class::method' 或裸函数名。"""
    try:
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    except OSError:
        return '?'
    for i in range(min(line_no - 1, len(lines) - 1), max(0, line_no - 400), -1):
        s = lines[i].rstrip()
        if not s or s.lstrip().startswith(('//', '*', '/*', '#')):
            continue
        if s.rstrip().endswith(';'):
            continue
        if not s.endswith('{') and not s.endswith(')'):
            continue
        if '(' not in s:
            continue
        if re.search(r'\b(if|for|while|switch|catch|return|else)\b\s*\(', s):
            continue
        m = re.search(r'([\w:]+)\s*\(', s)
        if m:
            return m.group(1)
    return '?'


def main():
    args = [a for a in sys.argv[1:]]
    workers_only = '--workers' in args
    args = [a for a in args if a != '--workers']
    needle = args[0] if args else None
    os.chdir(ROOT)
    found = []
    for h in HEADERS:
        if not os.path.exists(h):
            print('missing header:', h)
            continue
        for i, line in enumerate(open(h, encoding='utf-8', errors='replace'), 1):
            m = MEMBER_RE.match(line.rstrip())
            if m and (needle is None or needle in m.group(2)):
                found.append((h, i, m.group(1), m.group(2)))

    print('scanned %d headers -> %d non-atomic scalar members%s%s'
          % (len(HEADERS), len(found), '' if needle is None else ' (filter=%r)' % needle,
             ' (workers-only)' if workers_only else ''))
    print()

    for h, i, typ, name in found:
        r = subprocess.run(['rg', '-n', '--no-heading', '-w', name, 'PXView'],
                           capture_output=True, text=True)
        refs = [l for l in r.stdout.split('\n') if l.strip()]
        parsed = []
        for ref in refs:
            parts = ref.split(':', 2)
            if len(parts) < 3:
                continue
            f, ln, code = parts[0], int(parts[1]), parts[2]
            base = os.path.basename(f)
            role = WORKER_FILES.get(base, '')
            parsed.append((f, ln, code.strip(), base, role))

        worker_refs = [p for p in parsed if p[4] and p[4] != 'main']
        other_refs = [p for p in parsed if not (p[4] and p[4] != 'main')]
        if workers_only and not worker_refs:
            continue

        print('== %s %s  (%s:%d)%s'
              % (name, typ, h, i,
                 '' if not worker_refs else
                 '   <-- %d worker ref, %d main ref' % (len(worker_refs), len(other_refs))))
        shown = worker_refs + ([] if workers_only else other_refs)
        for f, ln, code, base, role in shown:
            fn = enclosing_function(f, ln)
            tag = ('[%s]' % role) if role and role != 'main' else ''
            print('   %-52s %-40s %s %s' % (f.replace('\\', '/') + ':' + str(ln),
                                            fn[:38], tag, code[:70]))
        print()


if __name__ == '__main__':
    main()