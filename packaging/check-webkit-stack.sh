#!/usr/bin/env bash
#==============================================================================
#= check-webkit-stack.sh -- fail the build if the WebKitGTK stack got bundled
#= into an AppDir.
#=
#= Why this check exists
#= ---------------------
#= linuxdeploy walks the ldd tree of every executable passed with -e. For the
#= Tauri Agent that tree is the entire WebKitGTK stack:
#=   libwebkit2gtk-4.1.so.0 (~95 MB), libjavascriptcoregtk-4.1.so.0,
#=   libsoup-3.0.so.0, libgtk-3.so.0, libgdk-3.so.0,
#=   libgstreamer-1.0.so.0 + libgst{app,audio,base,fft,gl,pbutils,tag,video},
#=   libenchant-2.so.2, libmanette-0.2.so.2, ...
#=
#= WebKitGTK cannot be redistributed that way. The helper processes it forks are
#= not libraries, so linuxdeploy never copies them:
#=   /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1/
#=       WebKitWebProcess  WebKitNetworkProcess  WebKitGPUProcess
#=       injected-bundle/
#= The result is an ABI mix: the Tauri UI process resolves the *bundled*
#= libwebkit2gtk + glib + libsoup + libgstreamer through AppRun's
#= LD_LIBRARY_PATH, while the helper processes stay on the system and resolve
#= the *system* ones.
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
#= Usage
#= -----
#=   packaging/check-webkit-stack.sh <AppDir>
#=
#= Exits non-zero if any bundled copy of the stack is found.
#==============================================================================
set -euo pipefail

APPDIR="${1:?usage: check-webkit-stack.sh <AppDir>}"

# Libraries that must come from the target system, never from the bundle.
PRUNE_GLOBS=(
    'libwebkit2gtk-4.1.so*'
    'libjavascriptcoregtk-4.1.so*'
    'libwebkit2gtkinjectedbundle.so*'
    'libsoup-3.0.so*'
    'libgtk-3.so*'
    'libgdk-3.so*'
    'libgstreamer-1.0.so*'
    'libgst*.so*'
    'libenchant-2.so*'
    'libmanette-0.2.so*'
)

found=0
for dir in "$APPDIR/usr/lib" "$APPDIR/usr/lib/x86_64-linux-gnu"; do
    [ -d "$dir" ] || continue
    for pattern in "${PRUNE_GLOBS[@]}"; do
        for lib in "$dir"/$pattern; do
            [ -f "$lib" ] || continue
            echo "ERROR: WebKitGTK stack leaked into the AppImage: $lib" >&2
            found=$((found + 1))
        done
    done
done

if [ "$found" -gt 0 ]; then
    cat >&2 <<'EOF'

The WebKitGTK/GTK3/GStreamer stack must not be bundled. Passing the Tauri
Agent to linuxdeploy with -e makes it copy libwebkit2gtk and friends, while the
WebKitWebProcess / WebKitNetworkProcess / WebKitGPUProcess helper executables
stay on the target system -- the resulting ABI mix crashes the WebProcess and
the agent window renders blank.

Fix: do not pass -e <AppDir>/usr/bin/PXView-Agent to linuxdeploy. The Agent is
installed into usr/bin already and will resolve webkit2gtk-4.1 from the target
system, which is what Tauri requires.
EOF
    exit 1
fi

# The Agent still has to be there and runnable -- it is shipped as a plain file,
# not via linuxdeploy.
agent="$APPDIR/usr/bin/PXView-Agent"
if [ -f "$agent" ]; then
    if [ ! -x "$agent" ]; then
        echo "ERROR: $agent is not executable" >&2
        exit 1
    fi
    echo "  [OK] WebKitGTK stack not bundled; PXView-Agent shipped as-is"
else
    echo "  [OK] WebKitGTK stack not bundled (no PXView-Agent in this AppDir)"
fi
