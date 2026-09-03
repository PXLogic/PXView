#!/bin/bash
# =============================================================================
# build_linux.sh
# PXView Linux 本机构建脚本 (x86_64)
#
# 用法:
#   ./build_linux.sh              # 完整构建 (依赖安装 + 编译 + 打包 .sh 安装器)
#   ./build_linux.sh --no-deps    # 跳过依赖安装 (已安装好依赖时使用)
#   ./build_linux.sh --no-package # 只编译安装,不生成安装器
#   ./build_linux.sh --no-firmware # 跳过 fx2lafw 固件编译
#   ./build_linux.sh --no-webui   # 跳过 Web UI 构建
#   ./build_linux.sh --system-qt  # 使用系统 Qt 而非 aqtinstall
# =============================================================================
set -e

# ── 配置 ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

QT_VERSION="6.11.0"
QT_PREFIX="$SCRIPT_DIR/Qt"
VERSION="${PXVIEW_VERSION:-1.5.4}"
INSTALL_PREFIX="$SCRIPT_DIR/install.dir/usr"
NPROC=$(nproc)

# 命令行参数解析
INSTALL_DEPS=true
BUILD_PACKAGE=true
BUILD_FIRMWARE=true
BUILD_WEBUI=true
USE_SYSTEM_QT=false

for arg in "$@"; do
    case "$arg" in
        --no-deps)       INSTALL_DEPS=false ;;
        --no-package)    BUILD_PACKAGE=false ;;
        --no-appimage)   BUILD_PACKAGE=false ;;   # 兼容旧调用: 等价于 --no-package
        --no-firmware)   BUILD_FIRMWARE=false ;;
        --no-webui)      BUILD_WEBUI=false ;;
        --system-qt)     USE_SYSTEM_QT=true ;;
        --help|-h)
            echo "用法: ./build_linux.sh [选项]"
            echo "  --no-deps        跳过依赖安装"
            echo "  --no-package     不生成 .sh 安装器"
            echo "  --no-firmware    跳过 fx2lafw 固件编译"
            echo "  --no-webui       跳过 Web UI 构建"
            echo "  --system-qt      使用系统 Qt (需自行安装 qt6-base-dev)"
            echo "  --help           显示此帮助"
            exit 0
            ;;
        *)
            echo "未知选项: $arg (使用 --help 查看帮助)"
            exit 1
            ;;
    esac
done

echo "================================================" 
echo " PXView Linux 本机构建"
echo " 架构:   $(uname -m)"
echo " CPU核心: $NPROC"
echo " 版本:   $VERSION"
echo "================================================"
echo ""

# ── 步骤 1: 安装构建依赖 ──────────────────────────────────────────────────
if [ "$INSTALL_DEPS" = true ]; then
    echo " [1/7] 安装构建依赖..."
    sudo apt-get update
    sudo apt-get install -y \
        git gcc g++ make cmake ninja-build pkg-config \
        libglib2.0-dev zlib1g-dev libusb-1.0-0-dev libudev-dev \
        libboost-dev libfftw3-dev libssl-dev \
        python3-dev python3-pip \
        libgl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-0 libvulkan-dev \
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
        libxcb-render0 libxcb-render-util0 libxcb-randr0 libxcb-shape0 \
        libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xinerama0 \
        libxcb-xinput0 libxcb-xkb1 libxcb-util1 \
        libwayland-dev libwayland-egl1 libwayland-client0 libwayland-cursor0 \
        libzip-dev nettle-dev libftdi1-dev \
        sdcc wget zip
    echo "   [OK] 依赖安装完成"
else
    echo " [1/7] 跳过依赖安装 (--no-deps)"
fi
echo ""

# ── 步骤 2: 安装 Qt ───────────────────────────────────────────────────────
echo " [2/7] 准备 Qt..."
if [ "$USE_SYSTEM_QT" = true ]; then
    echo "   使用系统 Qt"
    QT_CMAKE_PREFIX_PATH=""
else
    if [ ! -d "$QT_PREFIX/$QT_VERSION/gcc_64" ]; then
        echo "   通过 aqtinstall 安装 Qt $QT_VERSION..."
        pip3 install aqtinstall
        aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 \
            -m qtwebsockets --outputdir "$QT_PREFIX"
        echo "   [OK] Qt 安装到 $QT_PREFIX"
    else
        echo "   [OK] Qt 已存在于 $QT_PREFIX, 跳过安装"
    fi
    QT_CMAKE_PREFIX_PATH="$QT_PREFIX/$QT_VERSION/gcc_64"
fi
echo ""

# ── 步骤 3: 构建 qtwayland 平台插件 (仅 aqtinstall 模式) ──────────────────
if [ "$USE_SYSTEM_QT" = false ]; then
    echo " [3/7] 检查 qtwayland 平台插件..."
    QT_PLUGINS_DIR="$QT_PREFIX/$QT_VERSION/gcc_64/plugins/platforms"
    if [ -f "$QT_PLUGINS_DIR/libqwayland-qt.so" ] || [ -f "$QT_PLUGINS_DIR/libqwayland-generic.so" ]; then
        echo "   [OK] qtwayland 插件已存在,跳过编译"
    else
        echo "   构建 qtwayland (让安装包在 xcb 和 wayland 会话下都能启动)..."
        sudo apt-get install -y wayland-protocols libwayland-dev
        git clone --branch "v$QT_VERSION" --depth 1 https://github.com/qt/qtwayland.git
        cd qtwayland
        cmake -B build -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_PREFIX_PATH="$QT_PREFIX/$QT_VERSION/gcc_64" \
            -DCMAKE_INSTALL_PREFIX="$QT_PREFIX/$QT_VERSION/gcc_64" \
            -DQT_FEATURE_wayland_server=OFF \
            -DQT_FEATURE_wayland_client=ON
        cmake --build build --parallel "$NPROC"
        cmake --install build
        cd "$SCRIPT_DIR"
        echo "   [OK] qtwayland 编译完成"
    fi
else
    echo " [3/7] 跳过 qtwayland (使用系统 Qt)"
fi
echo ""

# ── 步骤 4: 构建 fx2lafw 固件 (可选) ──────────────────────────────────────
if [ "$BUILD_FIRMWARE" = true ]; then
    echo " [4/7] 构建 fx2lafw 固件..."
    if command -v sdcc >/dev/null 2>&1; then
        bash build_fx2lafw.sh || echo "   [警告] fx2lafw 固件编译失败 (不影响主构建)"
    else
        echo "   [跳过] sdcc 未安装,固件编译需要 sdcc"
        echo "   安装: sudo apt install sdcc"
    fi
else
    echo " [4/7] 跳过 fx2lafw 固件 (--no-firmware)"
fi
echo ""

# ── 步骤 5: 配置 CMake ────────────────────────────────────────────────────
echo " [5/7] 配置 CMake..."
rm -rf build install.dir
mkdir -p build
cd build

CMAKE_ARGS="-G Ninja -DCMAKE_BUILD_TYPE=Release"
CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=$INSTALL_PREFIX"

if [ "$USE_SYSTEM_QT" = false ]; then
    CMAKE_ARGS="$CMAKE_ARGS -DCMAKE_PREFIX_PATH=$QT_CMAKE_PREFIX_PATH"
fi

cmake .. $CMAKE_ARGS
cd "$SCRIPT_DIR"
echo "   [OK] CMake 配置完成"
echo ""

# ── 步骤 6: 构建和安装 ────────────────────────────────────────────────────
echo " [6/7] 编译 PXView..."
cd build

if [ "$BUILD_WEBUI" = true ]; then
    echo "   构建 Web UI..."
    # Web UI 需要 Node.js
    if command -v npm >/dev/null 2>&1; then
        ninja install-webui || echo "   [警告] Web UI 构建跳过"
    else
        echo "   [跳过] npm 未安装,Web UI 构建需要 Node.js"
        echo "   安装: sudo apt install nodejs npm"
    fi
fi

echo "   编译主程序 (ninja -j $NPROC)..."
ninja -j "$NPROC"
echo "   安装..."
ninja install
cd "$SCRIPT_DIR"
echo "   [OK] 编译安装完成"
echo ""

# ── 步骤 7: 打包运行时库并生成安装器 (可选) ──────────────────────────────
#
# 这里刻意不再使用 linuxdeploy / AppImage。linuxdeploy 会顺着 -d 给的 .desktop
# 文件找到同目录的 usr/bin/PXView-Agent(Tauri 二进制),把整个 WebKitGTK 栈
# (libwebkit2gtk-4.1 + libjavascriptcoregtk + libsoup + gtk3 + gstreamer 全家,
# 约 95 MB)复制进 usr/lib;而 WebKit 派生的 helper(WebKitWebProcess /
# WebKitNetworkProcess / WebKitGPUProcess + injected-bundle/)是可执行文件,
# 永远不会被复制。结果就是"打包的库 + 系统的 helper"ABI 混搭,Agent 窗口全白。
#
# packaging/bundle-runtime-libs.sh 用白名单取代它:只打包桌面栈不拥有的依赖
# (Qt、libsigrok、libpython、boost/fftw/usb/zip/nettle...),GLib/GTK/GStreamer/
# X11/Wayland 一律留给目标系统。
# ---------------------------------------------------------------------------
if [ "$BUILD_PACKAGE" = true ]; then
    echo " [7/7] 打包运行时库并生成安装器..."

    # 用 aqtinstall 装的 Qt 时必须显式指过去;用系统 Qt 时让脚本自己用 qmake 探测。
    if [ "$USE_SYSTEM_QT" = false ]; then
        export QT_DIR="$QT_PREFIX/$QT_VERSION/gcc_64"
    fi

    # 装填 install.dir/usr:Qt 运行时与插件、GCC 运行时、Python 标准库、
    # 白名单内的系统依赖,并写入 bin/qt.conf (让可重定位的安装树找到 Qt 插件)。
    # 脚本末尾会自动跑 packaging/check-webkit-stack.sh 自检。
    bash packaging/bundle-runtime-libs.sh install.dir

    echo "   生成自解压安装器..."
    PXVIEW_APPDIR="$SCRIPT_DIR/install.dir/usr" PXVIEW_VERSION="$VERSION" \
        bash packaging/make-installer.sh

    echo "   [OK] 安装器生成完成"
    ls -la "PXView-Linux-x86_64-Installer-$VERSION.sh"
else
    echo " [7/7] 跳过打包 (--no-package)"
fi

echo ""
echo "================================================"
echo " 构建完成!"
echo "================================================"
if [ "$BUILD_PACKAGE" = true ]; then
    echo " 安装器:    $SCRIPT_DIR/PXView-Linux-x86_64-Installer-$VERSION.sh"
    echo " 安装:      sudo $SCRIPT_DIR/PXView-Linux-x86_64-Installer-$VERSION.sh"
    echo " 指定目录:  sudo PXVIEW_PREFIX=/opt/PXView $SCRIPT_DIR/PXView-Linux-x86_64-Installer-$VERSION.sh"
fi
echo " 安装树:    $SCRIPT_DIR/install.dir/usr/"
echo " 直接运行:  $SCRIPT_DIR/install.dir/usr/bin/PXView"
echo ""
echo " udev 规则、图标、桌面项与卸载脚本均由安装器自动处理 (需要 root)。"
echo " 卸载:      sudo /opt/PXView/uninstall.sh"
echo "================================================"
