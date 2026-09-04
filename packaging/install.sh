#!/usr/bin/env bash
#==============================================================================
#= PXView Linux installer -- must run as root.
#=
#= Installs the bundled tree to $PXVIEW_PREFIX (default /opt/PXView) and does
#= the system integration an AppImage fundamentally cannot do:
#=
#=   * udev rules -> /etc/udev/rules.d   (without them the USB device is
#=     invisible to non-root users; the AppImage shipped these as a separate
#=     zip the user had to find and install by hand)
#=   * icons      -> /usr/share/icons/hicolor
#=   * two .desktop entries (classic PXView + PXView Agent) -- the AppImage has
#=     only one entry point, which is why it needed the pxview-launcher hack
#=   * wrappers   -> /usr/local/bin/pxview, /usr/local/bin/pxview-agent
#=   * $PREFIX/uninstall.sh
#=
#= Upgrade model (industrial-installer style; on-disk contract documented in
#= packaging/INSTALL_MANIFEST_SPEC.md):
#=
#=   * Ownership marker + version stamp : $PREFIX/.pxview/install.json
#=   * File manifest                    : $PREFIX/.pxview/manifest.txt
#=     ([tree] relative paths, [external]/[shortcuts] absolute paths)
#=   * Upgrade = staged swap: the old tree is renamed aside (atomic on one
#=     filesystem), the new tree is installed and self-checked, and only then
#=     the old tree is deleted. Any failure after the swap rolls back to the
#=     untouched old tree, so the system is never left without a PXView.
#=   * Uninstall = manifest-driven: deletes exactly the files the installer
#=     created, with glob fallbacks for pre-manifest installs.
#=
#= Environment switches:
#=   PXVIEW_PREFIX=/opt/PXView    install location
#=   PXVIEW_OVERWRITE=yes         plain in-place overwrite, no staging
#=                                (still prunes our own orphans via the old
#=                                manifest; keeps foreign extra files)
#=   PXVIEW_KEEP_OLD=yes          keep the staged old tree as .old.<ts> backup
#=   PXVIEW_FORCE=yes             take over a directory without our marker
#=   PXVIEW_KILL_RUNNING=yes      kill running instances instead of refusing
#=
#= Layout after install (all paths are resolved from applicationDirPath(), so
#= the prefix is relocatable -- see CMakeLists.txt INSTALL_RPATH notes):
#=
#=   /opt/PXView/bin/PXView             RUNPATH $ORIGIN/../lib, no LD_LIBRARY_PATH
#=   /opt/PXView/bin/PXView-Agent       uses the system webkit2gtk-4.1
#=   /opt/PXView/lib/                   Qt + libsigrok + bundled Python stdlib
#=   /opt/PXView/share/PXView/          res, lang, demo, doc
#=   /opt/PXView/share/libsigrokdecode/
#=   /opt/PXView/share/sigrok-firmware/
#==============================================================================
set -Eeuo pipefail

VERSION="@PXVIEW_VERSION@"
PREFIX="${PXVIEW_PREFIX:-/opt/PXView}"
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${0}")")" && pwd)"
PAYLOAD="$SCRIPT_DIR/data"

BIN_DIR=/usr/local/bin
APPS_DIR=/usr/local/share/applications
ICON_ROOT=/usr/share/icons/hicolor
UDEV_DIR=/etc/udev/rules.d
META_DIR="$PREFIX/.pxview"

step() { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '    [警告] %s\n' "$*" >&2; }
die()  { printf '\n错误: %s\n' "$*" >&2; exit 1; }

#------------------------------------------------------------------------------
# Bookkeeping: every file installed outside $PREFIX is tracked while the
# installer runs, so the manifest (and therefore the uninstaller) knows
# exactly what to remove. [tree] is walked from the installed payload.
#------------------------------------------------------------------------------
EXTERNALS=()    # absolute paths: udev rules, wrappers, .desktop entries, icons
SHORTCUTS=()    # absolute paths: per-user Desktop .desktop launchers
OLD_DIR=""      # staged old tree during a staged-swap upgrade ("" = none)
OLD_VERSION=""  # version string detected for the previous install ("" = fresh)
OLD_MANIFEST="" # old manifest path (staged-swap upgrades only)

track_external() { EXTERNALS+=("$1"); }
track_shortcut() { SHORTCUTS+=("$1"); }

# True when both paths live on the same filesystem, i.e. rename() -- and with
# it the atomic staged swap -- is possible. Unprobeable (stat missing) counts
# as "not same" and takes the conservative path.
same_fs() {
    local a b
    a="$(stat -c %d -- "$1" 2>/dev/null)" || return 1
    b="$(stat -c %d -- "$2" 2>/dev/null)" || return 1
    [ -n "$a" ] && [ "$a" = "$b" ]
}

# Version recorded in a .pxview/install.json ("" when absent/unparseable).
marker_version() {
    [ -f "$1" ] || return 0
    sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -1
}

# Glob-based cleanup of integration files from pre-manifest installs.
# Deliberately does NOT touch per-user Desktop shortcuts: they keep pointing
# into $PREFIX, which the upgrade refreshes in place; only a real uninstall
# removes them.
legacy_cleanup_externals() {
    local d s
    rm -f -- "$BIN_DIR/pxview" "$BIN_DIR/pxview-agent" "$BIN_DIR/pxview-uninstall"
    rm -f -- "$APPS_DIR"/pxview*.desktop
    d=/usr/lib/udev/rules.d
    [ -d "$d" ] || { d=/lib/udev/rules.d; [ -d "$d" ] || d=/etc/udev/rules.d; }
    rm -f -- "$d"/60-px.rules "$d"/pxview*.rules \
             "$d"/60-libsigrok*.rules "$d"/61-libsigrok*.rules
    rm -f -- /usr/share/pixmaps/pxview.png /usr/share/pixmaps/pxview.svg
    for s in 16x16 32x32 48x48 64x64 128x128 256x256 scalable; do
        rm -f -- "$ICON_ROOT/$s/apps/pxview.png" "$ICON_ROOT/$s/apps/pxview.svg"
    done
}

# ERR-trap rollback: armed only once the staged swap has happened. Undoes a
# half-finished install by dropping the partial new tree and moving the staged
# old tree back, leaving the system exactly as it was before the upgrade.
rollback() {
    local rc=$?
    trap - ERR
    set +e
    if [ -n "$OLD_DIR" ] && [ -d "$OLD_DIR" ]; then
        warn "安装失败 (exit $rc)，正在回滚到升级前状态..."
        rm -rf -- "$PREFIX"
        if mv -- "$OLD_DIR" "$PREFIX"; then
            warn "已回滚：旧版本保留在 $PREFIX，系统与安装前一致。"
        else
            warn "回滚失败！旧版本树仍在 $OLD_DIR，请手动恢复。"
        fi
    fi
    exit "$rc"
}

printf 'PXView %s 安装程序\n' "$VERSION"

#------------------------------------------------------------------------------
# 1. Sanity checks
#------------------------------------------------------------------------------
[ "$(id -u)" -eq 0 ] || die "需要 root 权限，请用 sudo 运行本安装程序。"

[ -d "$PAYLOAD" ] || die "找不到安装数据目录: $PAYLOAD (安装包损坏?)"
[ -x "$PAYLOAD/bin/PXView" ] || [ -f "$PAYLOAD/bin/PXView" ] \
    || die "$PAYLOAD/bin/PXView 不存在，安装包损坏。"

# The Tauri Agent needs webkit2gtk-4.1 from the target system; it is
# deliberately never bundled (see packaging/check-webkit-stack.sh). Since the
# installer runs as root anyway, try to pull the runtime in rather than leaving
# the user with an Agent that silently renders a blank window.
have_webkit() {
    # Prefer the ldconfig cache (fast, and matches the loader's view). When the
    # cache is stale -- e.g. a library was installed but ldconfig was never
    # re-run -- fall back to probing the actual .so files so we don't wrongly
    # report "missing" (and then try to reinstall) system-wide WebKit.
    ldconfig -p 2>/dev/null | grep -q 'libwebkit2gtk-4.1' && return 0
    local d
    for d in /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
        [ -d "$d" ] || continue
        [ -n "$(find "$d" -maxdepth 1 -name 'libwebkit2gtk-4.1.so*' -print -quit 2>/dev/null)" ] && return 0
    done
    return 1
}

install_webkit_runtime() {
    # Keep a diagnostics log so a silent failure isn't a black box. On success
    # the temp log is removed; on failure the tail of it is shown to the user.
    WEBKIT_LOG="${TMPDIR:-/tmp}/pxview-webkit-install.log"
    rm -f "$WEBKIT_LOG"
    if command -v apt-get >/dev/null 2>&1; then
        local deb="libwebkit2gtk-4.1-0 libgtk-3-0 librsvg2-2 libayatana-appindicator3-1"
        if apt-get install -y -qq --no-install-recommends $deb >>"$WEBKIT_LOG" 2>&1; then
            return 0
        fi
        # Package index may be stale or absent on a fresh box.
        apt-get update -qq >>"$WEBKIT_LOG" 2>&1 || true
        apt-get install -y -qq --no-install-recommends $deb >>"$WEBKIT_LOG" 2>&1
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y -q webkit2gtk4.1 gtk3 librsvg2 libappindicator-gtk3 >>"$WEBKIT_LOG" 2>&1
    elif command -v pacman >/dev/null 2>&1; then
        pacman -S --noconfirm --needed webkit2gtk-4.1 gtk3 librsvg libappindicator-gtk3 >>"$WEBKIT_LOG" 2>&1
    else
        return 1
    fi
    # Rebuild the loader cache so system WebKit becomes visible to ldconfig -p.
    command -v ldconfig >/dev/null 2>&1 && ldconfig >/dev/null 2>&1
    return 0
}

if [ -f "$PAYLOAD/bin/PXView-Agent" ] && ! have_webkit; then
    warn "未检测到 libwebkit2gtk-4.1，PXView Agent 需要它才能启动。"
    info "尝试自动安装 WebKitGTK 运行时..."
    # Failure here is not fatal: PXView itself does not need WebKit.
    if ! install_webkit_runtime; then
        :
    fi
    if have_webkit; then
        info "WebKitGTK 运行时已就绪"
    else
        log="${WEBKIT_LOG:-}"
        if [ -n "$log" ] && [ -s "$log" ]; then
            warn "安装过程输出（末尾 12 行）:"
            tail -n 12 "$log" | sed 's/^/    /' >&2
        fi
        warn "自动安装失败，请手动安装（PXView 主程序不受影响，仅 Agent 无法启动）:"
        warn "  Ubuntu/Debian: sudo apt install libwebkit2gtk-4.1-0 libgtk-3-0 librsvg2-2 libayatana-appindicator3-1"
        warn "  Fedora:        sudo dnf install webkit2gtk4.1 gtk3 librsvg2 libappindicator-gtk3"
        warn "  Arch:          sudo pacman -S webkit2gtk-4.1 gtk3 librsvg libappindicator-gtk3"
    fi
fi

#------------------------------------------------------------------------------
# 2. Upgrade handling -- detect the previous install, stage it, purge its
#    integration files. Everything after the staged swap is rollback-protected
#    by the ERR trap; everything before it is read-only inspection.
#------------------------------------------------------------------------------
step "检查已安装版本"

if compgen -G "$PREFIX.old.*" >/dev/null 2>&1; then
    warn "检测到此前中断升级遗留的暂存目录 ($PREFIX.old.*)，卸载时会一并清除"
fi

if [ ! -e "$PREFIX" ]; then
    info "全新安装: $PREFIX"
else
    #---- Ownership check: never touch a directory PXView does not manage ----
    if [ ! -f "$PREFIX/bin/PXView" ] && [ ! -f "$META_DIR/install.json" ]; then
        if [ "${PXVIEW_FORCE:-no}" != "yes" ] && [ "${PXVIEW_OVERWRITE:-no}" != "yes" ]; then
            die "$PREFIX 已存在，但找不到 PXView 的所有权标记 (bin/PXView 或 .pxview/install.json)。
为避免误删无关目录，已中止。若确认可接管该目录，请改用:
    PXVIEW_FORCE=yes        按升级流程接管 (暂存旧目录, 成功后删除)
    PXVIEW_OVERWRITE=yes    直接覆盖 (不清理旧系统集成文件)"
        fi
    fi

    #---- Previous version (marker JSON, else the legacy uninstall banner) ----
    OLD_VERSION="$(marker_version "$META_DIR/install.json")"
    if [ -z "$OLD_VERSION" ] && [ -f "$PREFIX/uninstall.sh" ]; then
        OLD_VERSION="$(sed -n 's/^# PXView \(.*\) uninstaller.*/\1/p' "$PREFIX/uninstall.sh" | head -1)"
    fi

    #---- Running instances: replacing files under a live capture loses data --
    if command -v pgrep >/dev/null 2>&1; then
        if pgrep -x PXView >/dev/null 2>&1 || pgrep -x PXView-Agent >/dev/null 2>&1; then
            if [ "${PXVIEW_KILL_RUNNING:-no}" = "yes" ]; then
                warn "检测到正在运行的 PXView/PXView-Agent，按 PXVIEW_KILL_RUNNING=yes 终止"
                pkill -x PXView-Agent 2>/dev/null || true
                pkill -x PXView 2>/dev/null || true
                sleep 2
                pkill -9 -x PXView-Agent 2>/dev/null || true
                pkill -9 -x PXView 2>/dev/null || true
            else
                die "检测到 PXView 正在运行。升级会替换其文件并中断正在进行的采集。
请先关闭 PXView / PXView-Agent 后重试；
或以 PXVIEW_KILL_RUNNING=yes 运行，由安装器自动结束进程。"
            fi
        fi
    fi

    if [ "${PXVIEW_OVERWRITE:-no}" = "yes" ]; then
        #---- Plain overwrite: no staging; prune only OUR orphaned tree files --
        warn "PXVIEW_OVERWRITE=yes: 直接覆盖安装 (不暂存旧版本)"
        if [ -f "$META_DIR/manifest.txt" ]; then
            while IFS= read -r rel; do
                [ -n "$rel" ] || continue
                if [ ! -e "$PAYLOAD/$rel" ]; then
                    rm -rf -- "$PREFIX/$rel"
                fi
            done < <(awk '/^\[tree\]/{s=1;next} /^\[/{s=0} s' "$META_DIR/manifest.txt")
            info "已按旧安装清单清理本版本不再包含的文件"
        fi
    elif same_fs "$PREFIX" "$(dirname -- "$PREFIX")"; then
        #---- Staged swap: atomic rename, then rollback-protected install -----
        OLD_DIR="$PREFIX.old.$(date +%Y%m%d%H%M%S)"
        [ -e "$OLD_DIR" ] && OLD_DIR="${OLD_DIR}.$$"
        trap rollback ERR
        mv -- "$PREFIX" "$OLD_DIR"
        [ -f "$OLD_DIR/.pxview/manifest.txt" ] && OLD_MANIFEST="$OLD_DIR/.pxview/manifest.txt"
        info "旧版本已暂存: $OLD_DIR"
        info "  安装成功后自动删除; 任一步失败将自动回滚到升级前状态"
    else
        #---- Cross-device mount point: rename() impossible --------------------
        warn "$PREFIX 位于独立文件系统，无法原子暂存，回退为先卸载后安装 (此路径无回滚保护)"
        if [ -f "$PREFIX/uninstall.sh" ]; then
            "$PREFIX/uninstall.sh" || warn "旧版卸载脚本执行失败，继续覆盖安装"
        fi
    fi

    #---- Purge the previous install's external integration files ------------
    if [ -n "$OLD_DIR" ]; then
        if [ -n "$OLD_MANIFEST" ] && [ -f "$OLD_MANIFEST" ]; then
            while IFS= read -r f; do
                rm -f -- "$f"
            done < <(grep -E '^/' "$OLD_MANIFEST" || true)
            info "已按旧安装清单精确清理旧版系统集成文件"
        else
            legacy_cleanup_externals
            info "旧版本无安装清单，已按通配规则清理旧版系统集成文件"
        fi
    fi

    if [ -n "$OLD_VERSION" ]; then
        info "检测到旧版本: $OLD_VERSION -> $VERSION"
    fi
fi

#------------------------------------------------------------------------------
# 3. Install the tree
#------------------------------------------------------------------------------
step "安装到 $PREFIX"

mkdir -p "$PREFIX"
cp -a "$PAYLOAD/." "$PREFIX/"

chmod 755 "$PREFIX/bin/PXView"
[ -f "$PREFIX/bin/PXView-Agent" ] && chmod 755 "$PREFIX/bin/PXView-Agent"
[ -f "$PREFIX/bin/pxview-launcher" ] && chmod 755 "$PREFIX/bin/pxview-launcher"
find "$PREFIX/bin" -type f -name '*.so' -exec chmod 755 {} + 2>/dev/null || true

info "文件已安装"

#------------------------------------------------------------------------------
# 3. udev rules -- this is the part the AppImage could never do
#------------------------------------------------------------------------------
step "安装 udev 规则 (USB 设备访问权限)"

if [ -d "$PREFIX/lib/udev/rules.d" ]; then
    # Prefer the distro's own directory when it exists, fall back to /etc.
    if [ -d /usr/lib/udev/rules.d ]; then
        UDEV_DIR=/usr/lib/udev/rules.d
    elif [ -d /lib/udev/rules.d ]; then
        UDEV_DIR=/lib/udev/rules.d
    fi
    mkdir -p "$UDEV_DIR"
    for rule in "$PREFIX"/lib/udev/rules.d/*.rules; do
        [ -f "$rule" ] || continue
        cp -f "$rule" "$UDEV_DIR/"
        track_external "$UDEV_DIR/$(basename "$rule")"
        info "$(basename "$rule") -> $UDEV_DIR/"
    done
else
    warn "安装包中没有 udev 规则，跳过"
fi

# libsigrok's own rules ship inside the tree as well
for extra in "$PREFIX"/share/libsigrokdecode/contrib/*.rules; do
    [ -f "$extra" ] || continue
    cp -f "$extra" "$UDEV_DIR/" && {
        track_external "$UDEV_DIR/$(basename "$extra")"
        info "$(basename "$extra") -> $UDEV_DIR/"
    }
done

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger || true
    info "已重新加载 udev 规则"
fi

if ! getent group plugdev >/dev/null 2>&1; then
    groupadd plugdev && info "已创建 plugdev 组"
fi

# We already hold root here: add the invoking user to plugdev so they can use
# the USB device right after a re-login, instead of printing a "please run this
# yourself" hint. Membership takes effect on the user's next login, so we still
# tell them to log out/in. SUDO_USER is unset when run via su/root directly.
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
    if getent passwd "$SUDO_USER" >/dev/null 2>&1; then
        if usermod -aG plugdev "$SUDO_USER" 2>/dev/null; then
            info "已将 $SUDO_USER 加入 plugdev 组 (重新登录后生效)"
        else
            warn "无法自动将 $SUDO_USER 加入 plugdev 组，请手动执行: usermod -aG plugdev $SUDO_USER"
        fi
    fi
fi

#------------------------------------------------------------------------------
# 4. Icons
#------------------------------------------------------------------------------
step "安装图标"
for size in 16x16 32x32 48x48 64x64 128x128 256x256 scalable; do
    src="$PREFIX/share/icons/hicolor/$size/apps"
    [ -d "$src" ] || continue
    mkdir -p "$ICON_ROOT/$size/apps"
    for f in "$src"/*; do
        [ -f "$f" ] || continue
        dest="$ICON_ROOT/$size/apps/$(basename "$f")"
        cp -f "$f" "$dest" 2>/dev/null || true
        track_external "$dest"
    done
done
if [ -d "$PREFIX/share/pixmaps" ]; then
    mkdir -p /usr/share/pixmaps
    for f in "$PREFIX"/share/pixmaps/*; do
        [ -f "$f" ] || continue
        dest="/usr/share/pixmaps/$(basename "$f")"
        cp -f "$f" "$dest" 2>/dev/null || true
        track_external "$dest"
    done
fi
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q -t -f "$ICON_ROOT" || true
info "图标已安装"

#------------------------------------------------------------------------------
# 5. Launcher wrappers -- no LD_LIBRARY_PATH, the binaries use $ORIGIN RUNPATH
#------------------------------------------------------------------------------
#
# Deliberately no QT_PLUGIN_PATH / LD_LIBRARY_PATH here:
#
#   * Libraries  -- PXView carries INSTALL_RPATH $ORIGIN/../lib, so the bundled
#                   Qt, libsigrok and Python are found without any environment.
#   * Qt plugins -- RUNPATH never applies to plugins. Qt would otherwise look in
#                   the aqtinstall paths compiled into it and fail with
#                   "could not load the Qt platform plugin xcb". bin/qt.conf
#                   (written at package time) fixes that, and it works even for
#                   the .desktop entries below, which exec the binary directly
#                   and would bypass any wrapper we set variables in.
#   * The Agent  -- Tauri/WebKitGTK must resolve everything from the system.
#                   Exporting LD_LIBRARY_PATH=$PREFIX/lib here would also be
#                   inherited by the Agent (it sits in the same bin/), which is
#                   exactly the ABI mix check-webkit-stack.sh exists to prevent.
step "创建命令行入口"
mkdir -p "$BIN_DIR"

cat > "$BIN_DIR/pxview" <<EOF
#!/bin/sh
exec "$PREFIX/bin/PXView" "\$@"
EOF
chmod 755 "$BIN_DIR/pxview"
track_external "$BIN_DIR/pxview"
info "$BIN_DIR/pxview"

if [ -f "$PREFIX/bin/PXView-Agent" ]; then
    cat > "$BIN_DIR/pxview-agent" <<EOF
#!/bin/sh
exec "$PREFIX/bin/PXView-Agent" "\$@"
EOF
    chmod 755 "$BIN_DIR/pxview-agent"
    track_external "$BIN_DIR/pxview-agent"
    info "$BIN_DIR/pxview-agent"
fi

# Uninstall wrapper: lifts to root with a graphical prompt (pkexec) when
# available, otherwise falls back to sudo. This is what the "卸载 PXView"
# desktop/menu entry calls.
#
# NB: pkexec must NOT run with `exec` in the success path -- if it fails (e.g.
# no polkit authentication agent is running on the desktop), we must fall back
# to sudo instead of dying silently. The caller is a `Terminal=false` .desktop
# entry, so without this a failure looks like "nothing happened" to the user.
cat > "$BIN_DIR/pxview-uninstall" <<EOF
#!/bin/sh
# Uninstall PXView, prompting for privilege elevation when needed.
if command -v pkexec >/dev/null 2>&1; then
    pkexec "$PREFIX/uninstall.sh" && exit 0
fi
if command -v sudo >/dev/null 2>&1; then
    exec sudo "$PREFIX/uninstall.sh"
fi
echo "无法提权，请手动用 root 运行: $PREFIX/uninstall.sh" >&2
exit 1
EOF
chmod 755 "$BIN_DIR/pxview-uninstall"
track_external "$BIN_DIR/pxview-uninstall"
info "$BIN_DIR/pxview-uninstall"

#------------------------------------------------------------------------------
# 6. Desktop entries -- two real entries, no more single-AppRun juggling
#------------------------------------------------------------------------------
step "创建桌面菜单项"
mkdir -p "$APPS_DIR"

cat > "$APPS_DIR/pxview.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=PXView
GenericName=Logic Analyzer and Oscilloscope
Comment=GUI Program for PXLogic USB-based Instruments
Exec=$PREFIX/bin/PXView
Icon=pxview
Terminal=false
Categories=Development;Electronics;Qt;
Keywords=logic;analyzer;oscilloscope;sigrok;pxlogic;
StartupWMClass=PXView
EOF
track_external "$APPS_DIR/pxview.desktop"
info "$APPS_DIR/pxview.desktop"

if [ -f "$PREFIX/bin/PXView-Agent" ]; then
    cat > "$APPS_DIR/pxview-agent.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=PXView Agent
GenericName=PXView AI Agent
Comment=PXView Agent -- starts a headless PXView and opens the agent UI
Exec=$PREFIX/bin/PXView-Agent
Icon=pxview
Terminal=false
Categories=Development;Electronics;Qt;
Keywords=logic;analyzer;agent;ai;
StartupWMClass=PXView Agent
EOF
    track_external "$APPS_DIR/pxview-agent.desktop"
    info "$APPS_DIR/pxview-agent.desktop"
fi

# Uninstall entry -- invokes the root-lifting wrapper (pkexec/sudo), so a
# password dialog appears at click time. Reuses the self-installed pxview
# icon (already copied into /usr/share/icons/hicolor) so it always resolves,
# and stays visually consistent with the main PXView menu entries.
cat > "$APPS_DIR/pxview-uninstall.desktop" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=卸载 PXView
GenericName=Uninstall PXView
Comment=从本系统移除 PXView (需要管理员权限)
Exec=pxview-uninstall
Icon=pxview
Terminal=false
Categories=System;Development;Settings;
Keywords=uninstall;remove;purge;pxview;
EOF
track_external "$APPS_DIR/pxview-uninstall.desktop"
info "$APPS_DIR/pxview-uninstall.desktop"

command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database "$APPS_DIR" || true

#------------------------------------------------------------------------------
# 7. Desktop shortcuts for the invoking user (best effort)
#------------------------------------------------------------------------------
install_shortcuts() {
    local user="$1" home desktop
    home="$(getent passwd "$user" | cut -d: -f6)"
    [ -n "$home" ] && [ -d "$home" ] || return 0

    desktop="$(sudo -u "$user" XDG_CONFIG_HOME="$home/.config" \
               xdg-user-dir DESKTOP 2>/dev/null || true)"
    [ -n "$desktop" ] && [ -d "$desktop" ] || desktop="$home/Desktop"
    [ -d "$desktop" ] || return 0

    # pxview / pxview-agent are the launchers; pxview-uninstall the removal
    # entry (both desktop shortcut and menu entry).
    for name in pxview pxview-agent pxview-uninstall; do
        [ -f "$APPS_DIR/$name.desktop" ] || continue
        cp -f "$APPS_DIR/$name.desktop" "$desktop/"
        chmod 755 "$desktop/$name.desktop"
        # GNOME refuses to run .desktop files that are not marked trusted.
        sudo -u "$user" gio set "$desktop/$name.desktop" \
             metadata::trusted true 2>/dev/null || true
        chown "$user" "$desktop/$name.desktop" 2>/dev/null || true
        if [ -f "$desktop/$name.desktop" ]; then
            track_shortcut "$desktop/$name.desktop"
        fi
        info "桌面快捷方式: $desktop/$name.desktop"
    done
}

step "创建桌面快捷方式"
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
    install_shortcuts "$SUDO_USER" || warn "桌面快捷方式创建失败，可手动从应用菜单拖出"
else
    warn "未检测到普通用户 (SUDO_USER 未设置)，跳过桌面快捷方式"
fi

#------------------------------------------------------------------------------
# 9. Uninstaller -- manifest-driven, with glob fallbacks
#------------------------------------------------------------------------------
step "生成卸载脚本"
cat > "$PREFIX/uninstall.sh" <<EOF
#!/usr/bin/env bash
# PXView $VERSION uninstaller -- manifest-driven, run as root.
# Deletes exactly what the installer created: the $PREFIX tree plus every
# absolute path recorded in the manifest, with glob fallbacks for
# pre-manifest installs.
set -u
[ "\$(id -u)" -eq 0 ] || { echo "需要 root 权限，请用 sudo 运行。" >&2; exit 1; }

PREFIX="$PREFIX"
MANIFEST="$META_DIR/manifest.txt"
BIN_DIR="$BIN_DIR"
APPS_DIR="$APPS_DIR"
ICON_ROOT="$ICON_ROOT"

# 1) Manifest-driven removal -- absolute paths only ([external] + [shortcuts]);
#    the [tree] section is covered by the rm -rf of \$PREFIX below.
if [ -f "\$MANIFEST" ]; then
    grep -E '^/' "\$MANIFEST" | while IFS= read -r f; do
        [ -n "\$f" ] && rm -f -- "\$f"
    done
fi

# 2) Glob fallback -- covers pre-manifest installs and any stragglers; the
#    same patterns the installer itself uses for its legacy upgrade path.
rm -f -- "\$BIN_DIR/pxview" "\$BIN_DIR/pxview-agent" "\$BIN_DIR/pxview-uninstall"
rm -f -- "\$APPS_DIR"/pxview*.desktop
UDEV_DIR=/usr/lib/udev/rules.d
[ -d "\$UDEV_DIR" ] || { UDEV_DIR=/lib/udev/rules.d; [ -d "\$UDEV_DIR" ] || UDEV_DIR=/etc/udev/rules.d; }
rm -f -- "\$UDEV_DIR"/60-px.rules "\$UDEV_DIR"/pxview*.rules \\
         "\$UDEV_DIR"/60-libsigrok*.rules "\$UDEV_DIR"/61-libsigrok*.rules
rm -f -- /usr/share/pixmaps/pxview.png /usr/share/pixmaps/pxview.svg
for s in 16x16 32x32 48x48 64x64 128x128 256x256 scalable; do
    rm -f -- "\$ICON_ROOT/\$s/apps/pxview.png" "\$ICON_ROOT/\$s/apps/pxview.svg"
done

# 3) Per-user Desktop shortcuts. Walk /etc/passwd so any user (not just the
#    one who installed) gets cleaned up. plugdev membership is deliberately
#    left untouched -- it is a generic system group the account may still need
#    for other devices.
while IFS=: read -r _ _ uid _ _ homedir _; do
    # Only real interactive users (uid >= 1000) with an existing home dir.
    [ "\$uid" -ge 1000 ] 2>/dev/null || continue
    [ -n "\$homedir" ] && [ -d "\$homedir" ] || continue
    desktop="\$homedir/Desktop"
    if command -v xdg-user-dir >/dev/null 2>&1; then
        d="\$(sudo -u "\$(basename "\$homedir")" xdg-user-dir DESKTOP 2>/dev/null || true)"
        [ -n "\$d" ] && [ -d "\$d" ] && desktop="\$d"
    fi
    [ -d "\$desktop" ] || continue
    rm -f -- "\$desktop"/pxview*.desktop
done < /etc/passwd

# 4) The tree itself (marker dir included) plus leftover staging trees from
#    any interrupted upgrade.
rm -rf -- "\$PREFIX"
rm -rf -- "\$PREFIX".old.* 2>/dev/null

# 5) Refresh system caches.
command -v udevadm >/dev/null 2>&1 && { udevadm control --reload-rules; udevadm trigger; } || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "\$APPS_DIR" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f "\$ICON_ROOT" || true
echo "PXView $VERSION 已从 \$PREFIX 卸载。"
EOF
chmod 755 "$PREFIX/uninstall.sh"
info "$PREFIX/uninstall.sh"

#------------------------------------------------------------------------------
# 10. Install manifest + version stamp -- the on-disk contract consumed by
#     uninstall.sh and by the next upgrade (see INSTALL_MANIFEST_SPEC.md)
#------------------------------------------------------------------------------
step "写入安装清单与版本标记"
mkdir -p "$META_DIR"
{
    echo "# PXView install manifest -- consumed by uninstall.sh and the next upgrade"
    echo "# version=$VERSION"
    echo "[tree]"
    (cd "$PREFIX" && find . -mindepth 1 -not -path './.pxview' -not -path './.pxview/*' \
        | sed 's|^\./||' | LC_ALL=C sort)
    echo "[external]"
    if [ "${#EXTERNALS[@]}" -gt 0 ]; then
        printf '%s\n' "${EXTERNALS[@]}" | LC_ALL=C sort -u
    fi
    echo "[shortcuts]"
    if [ "${#SHORTCUTS[@]}" -gt 0 ]; then
        printf '%s\n' "${SHORTCUTS[@]}" | LC_ALL=C sort -u
    fi
} > "$META_DIR/manifest.txt"

cat > "$META_DIR/install.json" <<EOF
{
  "name": "PXView",
  "version": "$VERSION",
  "installed": "$(date +%Y-%m-%dT%H:%M:%S%z)",
  "installer": "self-extracting",
  "prefix": "$PREFIX",
  "manifest": ".pxview/manifest.txt"
}
EOF
info "$META_DIR/manifest.txt"
info "$META_DIR/install.json"

#------------------------------------------------------------------------------
# 11. Self check -- the upgrade is only committed once this passes
#------------------------------------------------------------------------------
step "安装自检"
test -x "$PREFIX/bin/PXView"
if command -v ldd >/dev/null 2>&1; then
    if ldd "$PREFIX/bin/PXView" 2>/dev/null | grep -q 'not found'; then
        warn "PXView 存在未解析的库依赖:"
        ldd "$PREFIX/bin/PXView" 2>/dev/null | grep 'not found' >&2 || true
        false   # trip the ERR trap -> automatic rollback to the staged old tree
    fi
fi
info "自检通过"

#------------------------------------------------------------------------------
# 12. Commit -- new tree is complete and verified; failures no longer roll back
#------------------------------------------------------------------------------
trap - ERR

if [ -n "$OLD_DIR" ] && [ -d "$OLD_DIR" ]; then
    if [ "${PXVIEW_KEEP_OLD:-no}" = "yes" ]; then
        info "PXVIEW_KEEP_OLD=yes: 旧版本备份保留于 $OLD_DIR (确认无误后可手动删除)"
    else
        rm -rf -- "$OLD_DIR"
        info "旧版本已删除: $OLD_DIR"
    fi
    OLD_DIR=""
fi

#------------------------------------------------------------------------------
# 13. Summary
#------------------------------------------------------------------------------
step "安装完成"
info "版本:       $VERSION"
if [ -n "$OLD_VERSION" ]; then
    info "升级自:     $OLD_VERSION"
fi
info "安装目录:   $PREFIX"
info "启动:       pxview          (或在应用菜单中选择 PXView)"
[ -f "$PREFIX/bin/PXView-Agent" ] && \
    info "            pxview-agent    (或在应用菜单中选择 PXView Agent)"
info "卸载:       sudo $PREFIX/uninstall.sh   (或应用菜单中的 卸载 PXView)"
info "安装清单:   $META_DIR/manifest.txt"
echo
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
    if getent group plugdev >/dev/null 2>&1 && getent passwd "$SUDO_USER" >/dev/null 2>&1 \
       && id -nG "$SUDO_USER" 2>/dev/null | grep -qw plugdev; then
        info "已将 $SUDO_USER 加入 plugdev 组。请重新登录使 USB 设备权限生效。"
    else
        info "注意: 请手动把自己加入 plugdev 组并重新登录，否则普通用户无权访问 USB 设备:"
        info "      sudo usermod -aG plugdev $SUDO_USER"
    fi
fi
echo
