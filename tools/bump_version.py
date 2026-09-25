#!/usr/bin/env python3
"""PXView 版本号统一改写脚本（跨平台，CI 里也用这一个）。

用法:
    python tools/bump_version.py <版本号> [--root <仓库根>]

`<版本号>` 可以是：
    * 发布版本，如 1.6.7
    * 开发构建版本，如 1.6.6-rc0（带后缀）

改写规则（为什么后缀不写进数字字段见文件末尾说明）：
    1. CMakeLists.txt
         DS_VERSION_MAJOR/MINOR/MICRO 只取前三段数字（它们进 config.h 与 CPack 的
         CPACK_PACKAGE_VERSION_PATCH，必须是数字）；后缀写进 DS_VERSION_SUFFIX，
         也就是 App 内显示的版本串（About / --version / MCP serverInfo / 启动日志）。
    2. window_nisi.nsi            PRODUCT_VERSION = 完整版本号
    3. web/package.json           version       = 完整版本号
       web/src-tauri/Cargo.toml   version       = 完整版本号
       web/src-tauri/tauri.conf.json version    = 完整版本号
    4. README.md                  badge version-<基础版本>-（只写数字段，badge 不体现后缀）
    5. .github/workflows/build.yml、republish-release.yml 的 workflow_dispatch
       default 值 = 基础版本号（只是手动触发时的输入框默认值）

所有文件按原样保留 BOM 与行尾；只替换行内内容，不新增/删除行。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# 必须改成功：会影响构建产物里的版本号
REQUIRED = [
    'CMakeLists.txt',
    'web/package.json',
    'web/src-tauri/Cargo.toml',
    'web/src-tauri/tauri.conf.json',
]
# 尽力而为：改不到只告警（例如文件被重命名）
OPTIONAL = [
    'window_nisi.nsi',
    'README.md',
    '.github/workflows/build.yml',
    '.github/workflows/republish-release.yml',
]


def split_version(version: str) -> tuple[str, str]:
    """→ (基础版本 X.Y.Z, 后缀 如 -rc0 或空)。基础版本必须是三段数字。"""
    m = re.fullmatch(r'(\d+)\.(\d+)\.(\d+)(-[0-9A-Za-z.\-]+)?', version.strip())
    if not m:
        sys.exit(f'ERROR: 版本号格式不合法：{version!r}（应为 X.Y.Z 或 X.Y.Z-<后缀>）')
    return f'{m.group(1)}.{m.group(2)}.{m.group(3)}', m.group(4) or ''


def read_text(path: Path) -> tuple[str, bool, str]:
    """→ (文本, 是否带 BOM, 行尾)。行尾按文件里出现更多的那个。"""
    raw = path.read_bytes()
    bom = raw.startswith(b'\xef\xbb\xbf')
    text = raw.decode('utf-8-sig')
    eol = '\r\n' if raw.count(b'\r\n') >= raw.count(b'\n') - raw.count(b'\r\n') else '\n'
    return text, bom, eol


def write_text(path: Path, text: str, bom: bool) -> None:
    path.write_bytes((b'\xef\xbb\xbf' if bom else b'') + text.encode('utf-8'))


class Result:
    def __init__(self) -> None:
        self.changed: list[str] = []
        self.warned: list[str] = []

    def apply(self, path: Path, sub) -> bool:
        if not path.is_file():
            self.warned.append(f'{path}: 文件不存在')
            return False
        text, bom, _ = read_text(path)
        new_text, n = sub(text)
        if n == 0:
            self.warned.append(f'{path}: 未匹配到需要改写的版本号')
            return False
        if new_text != text:
            write_text(path, new_text, bom)
            self.changed.append(f'{path} ({n} 处)')
        return True


def main() -> int:
    ap = argparse.ArgumentParser(description='PXView 版本号统一改写')
    ap.add_argument('version', help='版本号，如 1.6.7 或 1.6.6-rc0')
    ap.add_argument('--root', default=None, help='仓库根目录（默认取本脚本的上级目录）')
    args = ap.parse_args()

    root = Path(args.root).resolve() if args.root else Path(__file__).resolve().parent.parent
    base, suffix = split_version(args.version)
    full = base + suffix
    res = Result()

    def sub_all(pattern: str, repl: str, count: int = 0):
        def f(text: str):
            return re.subn(pattern, repl, text, count=count)
        return f

    # 1. CMakeLists.txt：数字字段只吃基础版本；后缀进 DS_VERSION_SUFFIX
    def cmake_sub(text: str):
        text, n1 = re.subn(r'(?m)^(\s*set\(DS_VERSION_MAJOR\s+)\d+(\))', rf'\g<1>{base.split(".")[0]}\g<2>', text)
        text, n2 = re.subn(r'(?m)^(\s*set\(DS_VERSION_MINOR\s+)\d+(\))', rf'\g<1>{base.split(".")[1]}\g<2>', text)
        text, n3 = re.subn(r'(?m)^(\s*set\(DS_VERSION_MICRO\s+)\d+(\))', rf'\g<1>{base.split(".")[2]}\g<2>', text)
        pat = r'(?m)^(\s*set\(DS_VERSION_SUFFIX\s+")[^"]*(")'
        if re.search(pat, text):
            text, n4 = re.subn(pat, rf'\g<1>{suffix}\g<2>', text)
        else:  # 老树没有这一行：插在 DS_VERSION_STRING 之前
            _, _, eol = read_text(root / 'CMakeLists.txt')
            text, n4 = re.subn(
                r'(?m)^(\s*set\(DS_VERSION_STRING\b)',
                f'set(DS_VERSION_SUFFIX "{suffix}" CACHE STRING "开发构建的版本后缀，如 -rc0；正式版留空"){eol}\\g<1>',
                text)
        return text, n1 + n2 + n3 + n4
    res.apply(root / 'CMakeLists.txt', cmake_sub)

    # 2. NSIS 默认版本号（CI 还会用 /DPRODUCT_VERSION 覆盖，这里保持文件自洽）
    res.apply(root / 'window_nisi.nsi',
              sub_all(r'(?m)^(\s*!define PRODUCT_VERSION\s+")[^"]*(")', rf'\g<1>{full}\g<2>', 1))

    # 3. web/tauri：npm 与 cargo 都接受 semver 预发布串，直接用完整版本号
    res.apply(root / 'web/package.json',
              sub_all(r'(?m)^(\s*"version"\s*:\s*")[^"]*(")', rf'\g<1>{full}\g<2>', 1))
    res.apply(root / 'web/src-tauri/Cargo.toml',
              sub_all(r'(?m)^(\s*version\s*=\s*")[^"]*(")', rf'\g<1>{full}\g<2>', 1))
    res.apply(root / 'web/src-tauri/tauri.conf.json',
              sub_all(r'(?m)^(\s*"version"\s*:\s*")[^"]*(")', rf'\g<1>{full}\g<2>', 1))

    # 4. README badge（只写数字段；`-green.svg` 那截必须原样保留）
    res.apply(root / 'README.md',
              sub_all(r'(badge/version-)(\d+(?:\.\d+)*)(?=-|\))', rf'\g<1>{base}', 1))

    # 5. 两个 workflow 的 workflow_dispatch 默认版本号（数字段）
    for wf in ('.github/workflows/build.yml', '.github/workflows/republish-release.yml'):
        res.apply(root / wf,
                  sub_all(r"(?m)^(\s*default:\s*')\d+\.\d+\.\d+(')", rf'\g<1>{base}\g<2>', 1))

    print(f'版本号 -> {full}（基础 {base} / 后缀 {suffix or "无"}）')
    for c in res.changed:
        print(f'  [OK]   {c}')
    for w in res.warned:
        print(f'  [WARN] {w}')

    missing = [p for p in REQUIRED if any(w.startswith(str(root / p)) for w in res.warned)]
    if missing:
        print('ERROR: 必须改写的文件没改成功:', ', '.join(missing), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
