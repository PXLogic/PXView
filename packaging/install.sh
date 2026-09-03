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
set -euo pipefail

VERSION="@PXVIEW_VERSION@"
PREFIX="${PXVIEW_PREFIX:-/opt/PXView}"
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${0}")")" && pwd)"
PAYLOAD="$SCRIPT_DIR/data"

BIN_DIR=/usr/local/bin
APPS_DIR=/usr/local/share/applications
ICON_ROOT=/usr/share/icons/hicolor
UDEV_DIR=/etc/udev/rules.d

step() { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '    [警告] %s\n' "$*" >&2; }
die()  { printf '\n错误: %s\n' "$*" >&2; exit 1; }

printf 'PXView %s 安装程序\n' "$VERSION"

#------------------------------------------------------------------------------
# 1. Sanity checks
#------------------------------------------------------------------------------
[ "$(id -u)" -eq 0 ] || die "需要 root 权限，请用 sudo 运行本安装程序。"

[ -d "$PAYLOAD" ] || die "找不到安装数据目录: $PAYLOAD (安装包损坏?)"
[ -x "$PAYLOAD/bin/PXView" ] || [ -f "$PAYLOAD/bin/PXView" ] \
    || die "$PAYLOAD/bin/PXView 不存在，安装包损坏。"

# The Tauri Agent needs webkit2gtk-4.1 from the system; it is deliberately not
# bundled (see packaging/check-webkit-stack.sh).
if [ -f "$PAYLOAD/bin/PXView-Agent" ] && \
   ! ldconfig -p 2>/dev/null | grep -q 'libwebkit2gtk-4.1'; then
    warn "未检测到 libwebkit2gtk-4.1，PXView Agent 将无法启动。"
    warn "Ubuntu/Debian: sudo apt install libwebkit2gtk-4.1-0 libgtk-3-0 librsvg2-2 libayatana-appindicator3-1"
    warn "Fedora:        sudo dnf install webkit2gtk4.1 gtk3 librsvg2 libappindicator-gtk3"
fi

#------------------------------------------------------------------------------
# 2. Install the tree
#------------------------------------------------------------------------------
step "安装到 $PREFIX"

if [ -e "$PREFIX" ]; then
    backup="$PREFIX.old.$(date +%Y%m%d%H%M%S)"
    warn "$PREFIX 已存在，备份为 $backup"
    mv "$PREFIX" "$backup"
fi

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
        info "$(basename "$rule") -> $UDEV_DIR/"
    done
else
    warn "安装包中没有 udev 规则，跳过"
fi

# libsigrok's own rules ship inside the tree as well
for extra in "$PREFIX"/share/libsigrokdecode/contrib/*.rules; do
    [ -f "$extra" ] || continue
    cp -f "$extra" "$UDEV_DIR/" && info "$(basename "$extra") -> $UDEV_DIR/"
done

if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger || true
    info "已重新加载 udev 规则"
fi

if ! getent group plugdev >/dev/null 2>&1; then
    groupadd plugdev && info "已创建 plugdev 组"
fi

#------------------------------------------------------------------------------
# 4. Icons
#------------------------------------------------------------------------------
step "安装图标"
for size in 16x16 32x32 48x48 64x64 128x128 256x256 scalable; do
    src="$PREFIX/share/icons/hicolor/$size/apps"
    [ -d "$src" ] || continue
    mkdir -p "$ICON_ROOT/$size/apps"
    cp -f "$src"/* "$ICON_ROOT/$size/apps/" 2>/dev/null || true
done
if [ -d "$PREFIX/share/pixmaps" ]; then
    mkdir -p /usr/share/pixmaps
    cp -f "$PREFIX"/share/pixmaps/* /usr/share/pixmaps/ 2>/dev/null || true
fi
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q -t -f "$ICON_ROOT" || true
info "图标已安装"

#------------------------------------------------------------------------------
# 5. Launcher wrappers -- no LD_LIBRARY_PATH, the binaries use $ORIGIN RUNPATH
#------------------------------------------------------------------------------
step "创建命令行入口"
mkdir -p "$BIN_DIR"

cat > "$BIN_DIR/pxview" <<EOF
#!/bin/sh
exec "$PREFIX/bin/PXView" "\$@"
EOF
chmod 755 "$BIN_DIR/pxview"
info "$BIN_DIR/pxview"

if [ -f "$PREFIX/bin/PXView-Agent" ]; then
    cat > "$BIN_DIR/pxview-agent" <<EOF
#!/bin/sh
exec "$PREFIX/bin/PXView-Agent" "\$@"
EOF
    chmod 755 "$BIN_DIR/pxview-agent"
    info "$BIN_DIR/pxview-agent"
fi

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
    info "$APPS_DIR/pxview-agent.desktop"
fi

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

    for name in pxview pxview-agent; do
        [ -f "$APPS_DIR/$name.desktop" ] || continue
        cp -f "$APPS_DIR/$name.desktop" "$desktop/"
        chmod 755 "$desktop/$name.desktop"
        # GNOME refuses to run .desktop files that are not marked trusted.
        sudo -u "$user" gio set "$desktop/$name.desktop" \
             metadata::trusted true 2>/dev/null || true
        chown "$user" "$desktop/$name.desktop" 2>/dev/null || true
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
# 8. Uninstaller
#------------------------------------------------------------------------------
step "生成卸载脚本"
cat > "$PREFIX/uninstall.sh" <<EOF
#!/usr/bin/env bash
# PXView $VERSION uninstaller -- run as root.
set -u
[ "\$(id -u)" -eq 0 ] || { echo "需要 root 权限，请用 sudo 运行。" >&2; exit 1; }

rm -rf "$PREFIX"
rm -f "$BIN_DIR/pxview" "$BIN_DIR/pxview-agent"
rm -f "$APPS_DIR/pxview.desktop" "$APPS_DIR/pxview-agent.desktop"
rm -f "$UDEV_DIR/60-px.rules" "$UDEV_DIR/60-libsigrok.rules" \\
      "$UDEV_DIR/61-libsigrok-plugdev.rules" "$UDEV_DIR/61-libsigrok-uaccess.rules"
rm -f /usr/share/pixmaps/pxview.png /usr/share/pixmaps/pxview.svg
for s in 16x16 32x32 48x48 64x64 128x128 256x256 scalable; do
    rm -f "$ICON_ROOT/\$s/apps/pxview.png" "$ICON_ROOT/\$s/apps/pxview.svg"
done
command -v udevadm >/dev/null 2>&1 && { udevadm control --reload-rules; udevadm trigger; } || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t -f "$ICON_ROOT" || true
echo "PXView $VERSION 已从 $PREFIX 卸载。"
EOF
chmod 755 "$PREFIX/uninstall.sh"
info "$PREFIX/uninstall.sh"

#------------------------------------------------------------------------------
# 9. Summary
#------------------------------------------------------------------------------
step "安装完成"
info "版本:       $VERSION"
info "安装目录:   $PREFIX"
info "启动:       pxview          (或在应用菜单中选择 PXView)"
[ -f "$PREFIX/bin/PXView-Agent" ] && \
    info "            pxview-agent    (或在应用菜单中选择 PXView Agent)"
info "卸载:       sudo $PREFIX/uninstall.sh"
echo
if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ]; then
    info "注意: 请把自己加入 plugdev 组并重新登录，否则普通用户无权访问 USB 设备:"
    info "      sudo usermod -aG plugdev $SUDO_USER"
fi
echo
