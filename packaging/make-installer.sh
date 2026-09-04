#!/usr/bin/env bash
#==============================================================================
#= make-installer.sh -- build a self-extracting installer from install.dir
#=
#= Produces a single file the user can just run:
#=
#=   sudo ./PXView-Linux-x86_64-Installer-<version>.run
#=
#= The artifact is generated with makeself (the standard self-extracting
#= archive used by NVIDIA/CUDA et al.), which provides:
#=
#=   * embedded MD5 integrity check (CRC disabled), verified automatically at
#=     runtime (also available on demand via: ./PXView-*.run --check)
#=   * standard runtime flags: --target <dir> to extract without running,
#=     --noexec, --keep, --info, ...
#=   * a root pre-check BEFORE extracting anything (--needroot)
#=   * automatic temp-dir cleanup
#=
#= Payload layout (what makeself archives):
#=
#=   pxview-install/install.sh    the real installer (see packaging/install.sh)
#=   pxview-install/data/         the install tree (ninja install output)
#=
#= Build-time dependency: the makeself CLI (apt install makeself).
#=
#= Usage:
#=   PXVIEW_VERSION=1.5.9 packaging/make-installer.sh
#=   PXVIEW_APPDIR=install.dir/usr PXVIEW_VERSION=1.5.9 packaging/make-installer.sh
#==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

APPDIR="${PXVIEW_APPDIR:-$ROOT/install.dir/usr}"
OUTPUT_DIR="${PXVIEW_OUTPUT_DIR:-$ROOT}"

#------------------------------------------------------------------------------
# Version: env wins, otherwise read it from CMakeLists.txt
#------------------------------------------------------------------------------
detect_version() {
    if [ -n "${PXVIEW_VERSION:-}" ]; then
        printf '%s' "$PXVIEW_VERSION"
        return
    fi
    local cm="$ROOT/CMakeLists.txt"
    [ -f "$cm" ] || return
    local major minor micro
    major="$(sed -n 's/^[[:space:]]*set(DS_VERSION_MAJOR[[:space:]]*\([0-9]*\).*/\1/p' "$cm" | head -1)"
    minor="$(sed -n 's/^[[:space:]]*set(DS_VERSION_MINOR[[:space:]]*\([0-9]*\).*/\1/p' "$cm" | head -1)"
    micro="$(sed -n 's/^[[:space:]]*set(DS_VERSION_MICRO[[:space:]]*\([0-9]*\).*/\1/p' "$cm" | head -1)"
    if [ -n "$major" ] && [ -n "$minor" ] && [ -n "$micro" ]; then
        printf '%s.%s.%s' "$major" "$minor" "$micro"
    fi
}

VERSION="$(detect_version)"
[ -n "$VERSION" ] || { echo "ERROR: 无法获取版本号，请设置 PXVIEW_VERSION。" >&2; exit 1; }

[ -d "$APPDIR" ] || { echo "ERROR: 找不到 AppDir: $APPDIR (先运行 ninja install)" >&2; exit 1; }
[ -f "$APPDIR/bin/PXView" ] || { echo "ERROR: $APPDIR/bin/PXView 不存在。" >&2; exit 1; }
[ -f "$SCRIPT_DIR/install.sh" ] || { echo "ERROR: 缺少 packaging/install.sh" >&2; exit 1; }

OUTPUT="$OUTPUT_DIR/PXView-Linux-x86_64-Installer-$VERSION.run"
ARCH="$(uname -m)"

echo "================================"
echo " PXView 安装器打包"
echo " 版本:     $VERSION"
echo " 架构:     $ARCH"
echo " AppDir:   $APPDIR"
echo "================================"
echo

#------------------------------------------------------------------------------
# 1. Stage the payload
#------------------------------------------------------------------------------
STAGING="$(mktemp -d "${TMPDIR:-/tmp}/pxview-installer.XXXXXX")"
cleanup() { rm -rf "$STAGING"; }
trap cleanup EXIT

STAGE="$STAGING/pxview-install"
mkdir -p "$STAGE/data"
echo " [1/2] 暂存安装数据..."
cp -a "$APPDIR/." "$STAGE/data/"
echo "   $(du -sh "$STAGE/data" 2>/dev/null | cut -f1)"

sed "s/@PXVIEW_VERSION@/$VERSION/g" "$SCRIPT_DIR/install.sh" > "$STAGE/install.sh"
chmod 755 "$STAGE/install.sh"

# The bundled udev rules live under <prefix>/lib/udev/rules.d for non-system
# prefixes; make sure they are really there, because install.sh copies from
# that path (this is the step users used to have to do by hand).
if [ ! -d "$STAGE/data/lib/udev/rules.d" ] || \
   [ -z "$(ls -A "$STAGE/data/lib/udev/rules.d" 2>/dev/null)" ]; then
    echo "   [警告] 数据包中没有 lib/udev/rules.d，设备权限规则不会被安装"
fi

#------------------------------------------------------------------------------
# 2. Generate the self-extracting archive with makeself
#------------------------------------------------------------------------------
MAKSELF_CMD="$(command -v makeself.sh || command -v makeself || true)"
[ -n "$MAKSELF_CMD" ] || {
    echo "ERROR: 未找到 makeself。请先安装: apt install makeself (或 pacman -S makeself)" >&2
    exit 1
}

# zstd keeps the payload small; gzip is the fallback when the build box lacks
# it. Either way the TARGET machine needs the matching decompressor at install
# time (tar --zstd vs tar -z) -- same tradeoff as the previous hand-rolled
# header. Switch to --gzip unconditionally if old-distro support reports
# (CentOS 7 et al. without zstd) ever come in.
#
# Checksums: MD5 is embedded by default (there is no --md5 flag; only
# --nomd5 can turn it off). --nocrc drops the legacy CRC so runtime
# verification is exactly one MD5 pass over the payload -- the fastest
# corruption check. Add --sha256 back only if tamper-evidence is ever needed
# (against accidental corruption MD5 is sufficient; real tamper-evidence
# requires GPG signing either way).
echo " [2/2] 生成自解压安装器 (makeself)..."
COMPR_ARGS=(--gzip)
if command -v zstd >/dev/null 2>&1; then
    COMPR_ARGS=(--zstd)
fi

"$MAKSELF_CMD" "${COMPR_ARGS[@]}" --nocrc --needroot \
    "$STAGE" "$OUTPUT" "PXView $VERSION Linux x86_64 Installer" ./install.sh

chmod 755 "$OUTPUT"

echo
echo "================================"
echo " 安装器已生成"
echo "================================"
ls -la "$OUTPUT"
echo
echo " 安装:  sudo $OUTPUT"
echo " 指定目录: sudo PXVIEW_PREFIX=/opt/PXView $OUTPUT"
echo " 完整性自检: $OUTPUT --check"
echo
