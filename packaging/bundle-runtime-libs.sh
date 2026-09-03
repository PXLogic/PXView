#!/usr/bin/env bash
#==============================================================================
#= bundle-runtime-libs.sh -- populate an install tree with its runtime deps
#=
#= Replaces linuxdeploy for the .sh installer pipeline.
#=
#= Why not linuxdeploy
#= -------------------
#= linuxdeploy walks the ldd tree of everything it can find in the AppDir. It
#= reaches usr/bin/PXView-Agent (the Tauri binary) through the .desktop file we
#= hand it with -d, and that tree is the entire WebKitGTK stack:
#=   libwebkit2gtk-4.1 (~95 MB), libjavascriptcoregtk-4.1, libsoup-3.0,
#=   libgtk-3, libgdk-3, libgstreamer-1.0 + libgst*, libenchant-2, libmanette
#= Bundling those is fatal: WebKitGTK forks helper executables
#= (WebKitWebProcess / WebKitNetworkProcess / WebKitGPUProcess +
#= injected-bundle/) that live in /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1/.
#= They are executables, so no bundler copies them, and they keep resolving the
#= *system* libraries while the UI process resolves the *bundled* ones. That ABI
#= mix kills the WebProcess -- blank window, "WebKitWebProcess closed
#= unexpectedly", "GStreamer element appsink not found". The headless PXView
#= child keeps serving MCP 10110 throughout, so only the user's eyes notice.
#=
#= The rule this script enforces
#= -----------------------------
#= Bundle ONLY what the desktop stack does not own. Anything that can also be
#= reached from webkit2gtk inside the Agent process must come from the target
#= system, because the Agent and PXView share one <prefix>/lib directory while
#= loading different dependency graphs:
#=
#=   PXView (Qt)     -> bundled libglib (build machine's copy)
#=   Agent  (Tauri)  -> system webkit2gtk -> system libglib
#=
#= Two copies of glib in one process is undefined behaviour. So GLib, GTK,
#= GStreamer, Pango, Cairo, X11, Wayland, Soup, OpenSSL and friends are never
#= copied -- they are guaranteed present on any system that can run webkit2gtk
#= in the first place, which is Tauri's stated minimum (Ubuntu 22.04+).
#=
#= What IS bundled (whitelist)
#= ---------------------------
#=   Qt 6 runtime + plugins   built by aqtinstall, version-specific; the app is
#=                            compiled against 6.11 and must NOT fall back to
#=                            the distro's 6.4 (Qt6 is forward-ABI only)
#=   libsigrok / libpython    private to this app
#=   boost, fftw3, libusb,    app-specific deps webkit never touches
#=   libzip, nettle, pcap,
#=   ftdi, gmp, hogweed
#=   libstdc++ / libgcc_s     forward-compat for older distros (build on 24.04,
#=                            run on 22.04)
#=
#= Qt plugin discovery
#= -------------------
#= RUNPATH finds *libraries*, never plugins. Qt locates plugins through
#= QLibraryInfo, which returns the paths compiled into Qt (the aqtinstall
#= prefix). So a relocatable install needs a qt.conf next to the binary -- and
#= qt.conf is the right tool rather than QT_PLUGIN_PATH in a wrapper, because
#= the .desktop files written by install.sh exec the binary directly and would
#= bypass any wrapper. Tauri's Agent is not a Qt app and ignores qt.conf, so
#= there is no chance of it picking up the bundled Qt.
#=
#= Usage
#= -----
#=   packaging/bundle-runtime-libs.sh [staging-root]      # default: install.dir
#=
#= Environment
#=   QT_DIR          path to the Qt installation (the .../gcc_64 dir).
#=                   Auto-detected from $ROOT/Qt/*/gcc_64 when unset.
#=   PXVIEW_VERSION  version string, only used for log output.
#==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

STAGE="${1:-$ROOT/install.dir}"
PREFIX="$STAGE/usr"

step() { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '    [警告] %s\n' "$*" >&2; }
die()  { printf '\n错误: %s\n' "$*" >&2; exit 1; }

[ -d "$PREFIX" ]        || die "找不到安装树: $PREFIX (先运行 ninja install)"
[ -f "$PREFIX/bin/PXView" ] || die "$PREFIX/bin/PXView 不存在"

#------------------------------------------------------------------------------
# Locate Qt
#------------------------------------------------------------------------------
if [ -z "${QT_DIR:-}" ]; then
    for cand in "$ROOT"/Qt/*/gcc_64; do
        [ -d "$cand/lib" ] || continue
        QT_DIR="$cand"
        break
    done
fi
if [ -z "${QT_DIR:-}" ] || [ ! -d "${QT_DIR:-}/lib" ]; then
    # Fall back to whatever qmake is on PATH (system Qt builds).
    if command -v qmake6 >/dev/null 2>&1; then
        QT_DIR="$(qmake6 -query QT_INSTALL_PREFIX)"
    elif command -v qmake >/dev/null 2>&1; then
        QT_DIR="$(qmake -query QT_INSTALL_PREFIX)"
    fi
fi
[ -n "${QT_DIR:-}" ] && [ -d "${QT_DIR:-}/lib" ] || die "找不到 Qt 安装目录，请设置 QT_DIR=.../gcc_64"

printf 'PXView 运行时打包\n'
info "安装树: $PREFIX"
info "Qt:     $QT_DIR"
echo

mkdir -p "$PREFIX/lib" "$PREFIX/plugins"

#------------------------------------------------------------------------------
# 1. Qt runtime libraries
#------------------------------------------------------------------------------
step "拷贝 Qt 运行时库"
qt_count=0
shopt -s nullglob
for pattern in 'libQt6*.so*' 'libicu*.so*'; do
    for lib in "$QT_DIR/lib"/$pattern; do
        [ -e "$lib" ] || continue
        cp -a "$lib" "$PREFIX/lib/"
        qt_count=$((qt_count + 1))
    done
done
shopt -u nullglob
[ "$qt_count" -gt 0 ] || die "在 $QT_DIR/lib 下没有找到任何 Qt 库"
info "已拷贝 $qt_count 个 Qt/ICU 库"

#------------------------------------------------------------------------------
# 2. Qt plugins
#------------------------------------------------------------------------------
step "拷贝 Qt 插件"
if [ -d "$QT_DIR/plugins" ]; then
    cp -a "$QT_DIR/plugins/." "$PREFIX/plugins/"
    info "插件目录:"
    for d in "$PREFIX/plugins"/*/; do
        [ -d "$d" ] || continue
        info "  $(basename "$d")/  ($(ls -1 "$d" | wc -l | tr -d ' ') 个)"
    done
else
    warn "$QT_DIR/plugins 不存在，Qt 可能无法加载平台插件"
fi

#------------------------------------------------------------------------------
# 3. qt.conf -- how a relocatable tree finds its Qt plugins
#------------------------------------------------------------------------------
step "写入 qt.conf"
# Relative paths resolve against the directory holding qt.conf ($PREFIX/bin),
# so Prefix=.. -> $PREFIX. Qt reads this before any compiled-in path, and the
# Tauri Agent (not a Qt app) ignores it entirely.
cat > "$PREFIX/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Libraries = lib
Plugins = plugins
EOF
info "$PREFIX/bin/qt.conf -> Prefix=.. (即 $PREFIX)"

#------------------------------------------------------------------------------
# 4. GCC runtime -- build on a newer distro, run on an older one
#------------------------------------------------------------------------------
step "拷贝 GCC 运行时"
for libname in libstdc++.so.6 libgcc_s.so.1; do
    src="$(gcc -print-file-name="$libname" 2>/dev/null || true)"
    if [ -n "$src" ] && [ -f "$src" ]; then
        cp -f "$src" "$PREFIX/lib/"
        info "$libname <- $src"
    else
        warn "找不到 $libname"
    fi
done
if [ -f "$PREFIX/lib/libstdc++.so.6" ]; then
    info "GLIBCXX 版本: $(strings "$PREFIX/lib/libstdc++.so.6" | grep -o 'GLIBCXX_[0-9.]*' | sort -Vu | tail -1)"
fi

#------------------------------------------------------------------------------
# 5. Python standard library -- matched to the libpython actually linked
#------------------------------------------------------------------------------
step "打包 Python 标准库"
PY_VERSION=$(ldd "$PREFIX/bin/PXView" 2>/dev/null \
    | awk '/libpython/{print $1}' | head -1 \
    | sed -E 's/^libpython([0-9]+\.[0-9]+).*/\1/')
if [ -z "$PY_VERSION" ]; then
    warn "未读到链接的 libpython 版本，回退 python3"
    PY_VERSION=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")' 2>/dev/null || true)
fi
if [ -n "$PY_VERSION" ]; then
    PYEXE=$(ls /usr/bin/python$PY_VERSION /usr/local/bin/python$PY_VERSION 2>/dev/null | head -1 || true)
    if [ -n "$PYEXE" ] && [ -x "$PYEXE" ]; then
        PYSTDLIB=$("$PYEXE" -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')
        info "Python $PY_VERSION 标准库: $PYSTDLIB"
        mkdir -p "$PREFIX/lib/python$PY_VERSION"
        cp -a "$PYSTDLIB"/. "$PREFIX/lib/python$PY_VERSION/" 2>/dev/null || true
        rm -rf "$PREFIX/lib/python$PY_VERSION/test"
        find "$PREFIX/lib/python$PY_VERSION" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true
        if [ ! -d "$PREFIX/lib/python$PY_VERSION/encodings" ]; then
            die "Python $PY_VERSION 标准库未正确打包 (源 $PYSTDLIB)"
        fi
        info "已打包，大小: $(du -sh "$PREFIX/lib/python$PY_VERSION" | cut -f1)"
    else
        warn "找不到 python$PY_VERSION 解释器，跳过标准库打包"
    fi
else
    warn "无法确定 Python 版本，跳过标准库打包"
fi

#------------------------------------------------------------------------------
# 6. Whitelisted system dependencies
#------------------------------------------------------------------------------
step "收集白名单内的系统依赖"

# Only these are copied. Everything else stays on the target system.
BUNDLE_GLOBS=(
    'libsigrok*.so*'
    'libsigrokdecode*.so*'
    'libpython3.*.so*'
    'libboost_*.so*'
    'libfftw3*.so*'
    'libusb-1.0.so*'
    'libzip.so*'
    'libnettle.so*'
    'libhogweed.so*'
    'libgmp.so*'
    'libpcap.so*'
    'libftdi1.so*'
    'libstdc++.so*'
    'libgcc_s.so*'
)

matches_whitelist() {
    local base="$1" g
    for g in "${BUNDLE_GLOBS[@]}"; do
        # shellcheck disable=SC2254
        case "$base" in
            $g) return 0 ;;
        esac
    done
    return 1
}

bundled=0
skipped=""

# Every ELF that carries our own code: the main binary, our shared libraries
# and the compiled C decoders.
collect_targets() {
    local f
    for f in \
        "$PREFIX/bin/PXView" \
        "$PREFIX/bin/irmp" \
        "$PREFIX"/lib/libsigrok*.so* \
        "$PREFIX"/share/libsigrokdecode/c_decoders/*.so \
        "$PREFIX"/bin/webui/*.so ; do
        [ -f "$f" ] || continue
        # Skip symlinks -- ldd on the real file is enough. Written as an if,
        # not `[ -L ] && continue`: under `set -e` a bare AND-list that
        # evaluates false aborts the whole script.
        if [ -L "$f" ]; then
            continue
        fi
        printf '%s\n' "$f"
    done
}

resolve_deps() {
    # Print the absolute path of every library an ELF resolves to, skipping
    # "not found" entries and the vDSO.
    ldd "$1" 2>/dev/null | awk '
        $2 == "=>" && $3 != "not" { print $3; next }
        NF == 2 && $1 ~ /^\//    { print $1 }
    ' | sort -u
}

already_copied() { [ -e "$PREFIX/lib/$1" ]; }

while read -r target; do
    [ -n "$target" ] || continue
    while read -r dep; do
        [ -n "$dep" ] || continue
        [ -f "$dep" ] || continue
        base="$(basename "$dep")"

        # Never pull anything from the tree we are building, or from Qt
        # (already handled above).
        case "$dep" in
            "$PREFIX"/*|"$ROOT"/*|"$QT_DIR"/*) continue ;;
        esac

        if ! matches_whitelist "$base"; then
            skipped="$skipped $base"
            continue
        fi
        if already_copied "$base"; then
            continue
        fi

        cp -a "$dep" "$PREFIX/lib/" 2>/dev/null || continue
        # Bring the SONAME symlink family along (libusb-1.0.so.0 -> .0.4.0).
        for link in "$(dirname "$dep")"/${base%%.so*}.so*; do
            [ -L "$link" ] || continue
            cp -a "$link" "$PREFIX/lib/" 2>/dev/null || true
        done
        info "  + $base"
        bundled=$((bundled + 1))
    done < <(resolve_deps "$target")
done < <(collect_targets)

info "共打包 $bundled 个白名单依赖"

if [ -n "$skipped" ]; then
    step "以下依赖按要求不打包 (由目标系统提供)"
    printf '%s\n' "$skipped" | tr ' ' '\n' | sed '/^$/d' | sort -u | while read -r s; do
        info "  - $s"
    done
fi

#------------------------------------------------------------------------------
# 7. Self-check
#------------------------------------------------------------------------------
step "校验没有捆绑 WebKitGTK/桌面栈"
bash "$SCRIPT_DIR/check-webkit-stack.sh" "$STAGE"

step "运行时打包完成"
info "安装树: $PREFIX"
info "总大小: $(du -sh "$PREFIX" | cut -f1)"
