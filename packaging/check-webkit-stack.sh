#!/usr/bin/env bash
#==============================================================================
#= check-webkit-stack.sh -- fail the build if the desktop/WebKitGTK stack got
#=                          bundled into an install tree.
#=
#= Why this check exists
#= ---------------------
#= Any dependency bundler (linuxdeploy, or a naive ldd walk) that reaches
#= PXView-Agent -- the Tauri binary -- copies the entire WebKitGTK stack:
#=   libwebkit2gtk-4.1.so.0 (~95 MB), libjavascriptcoregtk-4.1.so.0,
#=   libsoup-3.0.so.0, libgtk-3.so.0, libgdk-3.so.0,
#=   libgstreamer-1.0.so.0 + libgst{app,audio,base,fft,gl,pbutils,tag,video},
#=   libenchant-2.so.2, libmanette-0.2.so.0, ...
#=
#= WebKitGTK cannot be redistributed that way. The helper processes it forks are
#= not libraries, so no bundler copies them:
#=   /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1/
#=       WebKitWebProcess  WebKitNetworkProcess  WebKitGPUProcess
#=       injected-bundle/
#= The result is an ABI mix: the Tauri UI process resolves the *bundled*
#= libwebkit2gtk + glib + libsoup + libgstreamer, while the helper processes
#= stay on the system and resolve the *system* ones.
#=
#= Observed symptom
#= ----------------
#=   * window opens blank white
#=   * (WebKitWebProcess:PID): ERROR **: WebProcess didn't exit as expected
#=                                      after the UI process connection was closed
#=   * "Sorry, the program \"WebKitWebProcess\" closed unexpectedly"
#=   * "GStreamer element appsink not found" (bundled libgstreamer cannot load
#=     the system GStreamer element plugins)
#=   * AND YET the app looks healthy: the UI process stays alive and the
#=     headless PXView it spawned keeps serving MCP port 10110. A port- or
#=     process-level health check cannot detect this.
#=
#= The wider rule
#= --------------
#= WebKitGTK is only the loudest symptom. PXView and PXView-Agent share ONE
#= <prefix>/lib but load different dependency graphs. Anything that can be
#= reached from both sides must come from the target system, or one process
#= ends up with two copies of the same library:
#=
#=   PXView (Qt)    -> bundled libglib   (build machine's copy)
#=   Agent  (Tauri) -> system webkit2gtk -> system libglib
#=
#= So the forbidden list covers the whole desktop stack, not just WebKit:
#= GLib, GTK, GStreamer, Pango, Cairo, ATK/AT-SPI, appindicator, Soup,
#= X11/xcb/Wayland and the font stack. All of them are guaranteed present on
#= any system that can run webkit2gtk at all, which is Tauri's stated minimum
#= (Ubuntu 22.04+ / Debian 12+ / Fedora 36+).
#=
#= Usage
#= -----
#=   packaging/check-webkit-stack.sh <install-tree>
#=
#= <install-tree> may be either the staging root (install.dir, layout usr/lib)
#= or the prefix itself (install.dir/usr, layout lib).
#=
#= Exits non-zero if any forbidden library is found.
#==============================================================================
set -euo pipefail

TREE="${1:?usage: check-webkit-stack.sh <install-tree>}"

# Libraries that must come from the target system, never from the bundle.
# Order is irrelevant; the loop de-duplicates.
PRUNE_GLOBS=(
    # -- WebKitGTK proper ---------------------------------------------------
    'libwebkit2gtk-4*.so*'
    'libjavascriptcoregtk-4*.so*'
    'libwebkit2gtkinjectedbundle.so*'
    # -- Networking / spell checking / gamepad pulled in by WebKit ----------
    'libsoup-*.so*'
    'libenchant-*.so*'
    'libmanette-*.so*'
    # -- GTK ----------------------------------------------------------------
    'libgtk-3.so*'
    'libgtk-4.so*'
    'libgdk-3.so*'
    'libgdk-4.so*'
    'libgdk_pixbuf-*.so*'
    'librsvg-*.so*'
    # -- GLib: the dangerous one, shared by Qt and WebKit alike -------------
    'libglib-2.0.so*'
    'libgobject-2.0.so*'
    'libgthread-2.0.so*'
    'libgio-2.0.so*'
    'libgmodule-2.0.so*'
    # -- GStreamer ----------------------------------------------------------
    'libgstreamer-*.so*'
    'libgst*.so*'
    # -- Text / graphics ----------------------------------------------------
    'libpango*.so*'
    'libpangocairo*.so*'
    'libcairo*.so*'
    'libharfbuzz.so*'
    'libfontconfig.so*'
    'libfreetype.so*'
    'libepoxy.so*'
    # -- Accessibility / tray -----------------------------------------------
    'libatk-*.so*'
    'libatk-bridge*.so*'
    'libatspi*.so*'
    'libayatana*.so*'
    'libdbusmenu*.so*'
    # -- Display servers ----------------------------------------------------
    'libX11*.so*'
    'libXext.so*'
    'libXrender.so*'
    'libXfixes.so*'
    'libXcursor.so*'
    'libXdamage.so*'
    'libXcomposite.so*'
    'libXrandr.so*'
    'libXinerama.so*'
    'libXi.so*'
    'libXtst.so*'
    'libxcb*.so*'
    'libxkbcommon*.so*'
    'libwayland*.so*'
)

# Locate the lib dir(s): accept both the staging root and the prefix itself.
LIBDIRS=()
for cand in "$TREE/usr/lib" "$TREE/lib"; do
    [ -d "$cand" ] || continue
    LIBDIRS+=("$cand")
done

if [ "${#LIBDIRS[@]}" -eq 0 ]; then
    echo "ERROR: $TREE 下既没有 usr/lib 也没有 lib，不是一个安装树" >&2
    exit 1
fi

# We only look at the top level of each lib dir (plus its multiarch subdir).
# lib/pythonX.Y/lib-dynload ships its own .so files and must not be mistaken
# for bundled desktop libraries.
#
# sort -u de-duplicates: 'libgst*' also matches 'libgstreamer-*', and a
# multiarch subdir can repeat what the top level already has.
hits="$(
    for dir in "${LIBDIRS[@]}"; do
        for scan in "$dir" "$dir/x86_64-linux-gnu"; do
            [ -d "$scan" ] || continue
            for pattern in "${PRUNE_GLOBS[@]}"; do
                # shellcheck disable=SC2254
                for lib in "$scan"/$pattern; do
                    if [ -f "$lib" ]; then
                        printf '%s\n' "$lib"
                    fi
                done
            done
        done
    done | sort -u
)"

if [ -n "$hits" ]; then
    while IFS= read -r lib; do
        echo "ERROR: 桌面/WebKitGTK 栈被误打进安装包: $lib" >&2
    done <<< "$hits"

    cat >&2 <<'EOF'

桌面/WebKitGTK/GStreamer 栈绝不能打包进安装树。任何依赖打包器只要触及
PXView-Agent（Tauri 二进制），就会把 libwebkit2gtk 全家复制进来，而 WebKit 派生的
helper 可执行文件（WebKitWebProcess / WebKitNetworkProcess / WebKitGPUProcess +
injected-bundle/，位于 /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1/）不是库，永远不会被
复制。于是 UI 进程用打包的库、helper 进程用系统的库，这套 ABI 混搭会直接搞死
WebProcess，表现就是 Agent 窗口全白。

更严格的规则是：PXView 与 PXView-Agent 共用同一个 <prefix>/lib，但两者的依赖图不同。
凡是两边都可能加载到的库（GLib/GTK/GStreamer/Pango/Cairo/X11/Wayland/Soup...）都必须
由目标系统提供，否则同一个进程里会出现两份 libglib。

Fix: 打包时只拷贝白名单内的依赖（见 packaging/bundle-runtime-libs.sh 的 BUNDLE_GLOBS）。
PXView-Agent 只作为普通文件随安装包分发，启动时从目标系统解析 webkit2gtk-4.1 ——
这正是 Tauri 的要求。
EOF
    exit 1
fi

# The Agent still has to be there and runnable -- it ships as a plain file,
# never through a dependency bundler.
agent=""
for cand in "$TREE/usr/bin/PXView-Agent" "$TREE/bin/PXView-Agent"; do
    [ -e "$cand" ] || continue
    agent="$cand"
    break
done

if [ -n "$agent" ]; then
    [ -x "$agent" ] || { echo "ERROR: $agent 不可执行" >&2; exit 1; }
    echo "  [OK] 未捆绑桌面/WebKitGTK 栈；PXView-Agent 以普通文件分发"
else
    echo "  [OK] 未捆绑桌面/WebKitGTK 栈（该安装树中没有 PXView-Agent）"
fi
