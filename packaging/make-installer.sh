#!/usr/bin/env bash
#==============================================================================
#= make-installer.sh -- build a self-extracting installer from install.dir
#=
#= Produces a single file the user can just run:
#=
#=   sudo ./PXView-Linux-x86_64-Installer-<version>.run
#=
#= Structure of the produced file:
#=
#=   [ shell header ]        locates the archive marker, untars to $TMPDIR
#=   __PXVIEW_ARCHIVE_BELOW__
#=   [ tar archive ]         pxview-install/install.sh + pxview-install/data/
#=
#= The header then execs install.sh, which does the real work (files, udev
#= rules, icons, desktop entries, uninstaller). See packaging/install.sh.
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
echo " [1/4] 暂存安装数据..."
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
# 2. Compress
#------------------------------------------------------------------------------
echo " [2/4] 压缩..."
if command -v zstd >/dev/null 2>&1; then
    SUFFIX=zst
    # Flag is written into the self-extracting header verbatim, so it must be
    # explicit: `tar -a` (auto-detect) is a GNU-tar-ism and some tar builds
    # ignore it while extracting, which silently yields an empty payload.
    TAR_FLAG=--zstd
    tar -C "$STAGING" --zstd -cf "$STAGING/payload.tar.zst" pxview-install
else
    SUFFIX=gz
    TAR_FLAG=-z
    tar -C "$STAGING" -czf "$STAGING/payload.tar.gz" pxview-install
fi
PAYLOAD="$STAGING/payload.tar.$SUFFIX"
echo "   $(du -h "$PAYLOAD" | cut -f1)"

#------------------------------------------------------------------------------
# 3. Emit the self-extracting header
#------------------------------------------------------------------------------
echo " [3/4] 生成自解压头..."
cat > "$OUTPUT" <<'HEADER'
#!/usr/bin/env bash
#==============================================================================
#= PXView self-extracting installer for Linux x86_64.
#=
#=   sudo ./PXView-Linux-x86_64-Installer-<version>.run
#=
#= The payload is appended below the __PXVIEW_ARCHIVE_BELOW__ marker. Everything
#= after that marker is a tar archive; everything before it is this script.
#==============================================================================
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "错误: 需要 root 权限，请用 sudo 运行本安装程序。" >&2
    exit 1
fi

TMP="$(mktemp -d "${TMPDIR:-/tmp}/pxview-extract.XXXXXX")"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

MARKER='__PXVIEW_ARCHIVE_BELOW__'
ARCHIVE_LINE="$(grep -an -m1 "^${MARKER}\$" "$0" | cut -d: -f1 || true)"
if [ -z "$ARCHIVE_LINE" ]; then
    echo "错误: 安装包已损坏(找不到归档标记)。" >&2
    exit 1
fi

echo "==> 正在解包到 $TMP ..."
tail -n +"$((ARCHIVE_LINE + 1))" "$0" | tar -C "$TMP" -x -f - @@TAR_FLAG@@

exec bash "$TMP/pxview-install/install.sh" "$@"
HEADER
# Inject the explicit tar decompression flag chosen at build time. Using a
# literal flag (--zstd / -z) instead of `tar -a` keeps the extraction portable
# across tar implementations; `tar -a` (auto-detect) is a GNU-tar-ism that some
# tar builds silently ignore while extracting, yielding an empty payload.
sed -i "s/@@TAR_FLAG@@/$TAR_FLAG/" "$OUTPUT"

#------------------------------------------------------------------------------
# 4. Append the archive
#------------------------------------------------------------------------------
echo " [4/4] 追加归档..."
{
    echo "__PXVIEW_ARCHIVE_BELOW__"
    cat "$PAYLOAD"
} >> "$OUTPUT"

chmod 755 "$OUTPUT"

echo
echo "================================"
echo " 安装器已生成"
echo "================================"
ls -la "$OUTPUT"
echo
echo " 安装:  sudo $OUTPUT"
echo " 指定目录: sudo PXVIEW_PREFIX=/opt/PXView $OUTPUT"
echo
