#!/bin/bash
# =============================================================================
# build_linux.sh
# PXView Linux 本机构建脚本 (x86_64)
#
# 用法:
#   ./build_linux.sh              # 完整构建 (依赖安装 + 编译 + 打包 AppImage)
#   ./build_linux.sh --no-deps    # 跳过依赖安装 (已安装好依赖时使用)
#   ./build_linux.sh --no-appimage # 只编译安装,不打包 AppImage
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
LINUXDEPLOY_VERSION="continuous"
VERSION="${PXVIEW_VERSION:-1.5.4}"
INSTALL_PREFIX="$SCRIPT_DIR/install.dir/usr"
NPROC=$(nproc)

# 命令行参数解析
INSTALL_DEPS=true
BUILD_APPIMAGE=true
BUILD_FIRMWARE=true
BUILD_WEBUI=true
USE_SYSTEM_QT=false

for arg in "$@"; do
    case "$arg" in
        --no-deps)       INSTALL_DEPS=false ;;
        --no-appimage)  BUILD_APPIMAGE=false ;;
        --no-firmware)  BUILD_FIRMWARE=false ;;
        --no-webui)     BUILD_WEBUI=false ;;
        --system-qt)    USE_SYSTEM_QT=true ;;
        --help|-h)
            echo "用法: ./build_linux.sh [选项]"
            echo "  --no-deps        跳过依赖安装"
            echo "  --no-appimage    不打包 AppImage"
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
        echo "   构建 qtwayland (让 AppImage 同时支持 xcb 和 wayland)..."
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

# ── 步骤 7: 打包 AppImage (可选) ──────────────────────────────────────────
if [ "$BUILD_APPIMAGE" = true ]; then
    echo " [7/7] 打包 AppImage..."

    # 打包 Python 标准库到 AppDir
    echo "   打包 Python 标准库..."
    PY_VERSION=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
    echo "   Python 版本: $PY_VERSION"
    mkdir -p "install.dir/usr/lib/python$PY_VERSION"
    cp -a "/usr/lib/python$PY_VERSION"/* "install.dir/usr/lib/python$PY_VERSION/" 2>/dev/null || true
    mkdir -p "install.dir/usr/lib/python$PY_VERSION/lib-dynload"
    cp -a "/usr/lib/python$PY_VERSION/lib-dynload"/* "install.dir/usr/lib/python$PY_VERSION/lib-dynload/" 2>/dev/null || true
    rm -rf "install.dir/usr/lib/python$PY_VERSION/test"
    rm -rf "install.dir/usr/lib/python$PY_VERSION/__pycache__"
    find "install.dir/usr/lib/python$PY_VERSION" -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

    # 下载 linuxdeploy
    echo "   下载 linuxdeploy..."
    if [ ! -f linuxdeploy-x86_64.AppImage ]; then
        wget -c "https://github.com/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_VERSION/linuxdeploy-x86_64.AppImage"
    fi
    if [ ! -f linuxdeploy-plugin-qt-x86_64.AppImage ]; then
        wget -c "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_VERSION/linuxdeploy-plugin-qt-x86_64.AppImage"
    fi
    chmod +x linuxdeploy*.AppImage

    export QMAKE="$QT_PREFIX/$QT_VERSION/gcc_64/bin/qmake"
    export LD_LIBRARY_PATH="$QT_PREFIX/$QT_VERSION/gcc_64/lib:$LD_LIBRARY_PATH"
    export OUTPUT="PXView-Linux-x86_64-AppImage-$VERSION.AppImage"

    ./linuxdeploy-x86_64.AppImage \
        --appdir install.dir \
        -e install.dir/usr/bin/PXView \
        -e install.dir/usr/bin/PXView-Agent \
        -d install.dir/usr/share/applications/pxview.desktop \
        --plugin qt \
        --output appimage

    echo "   [OK] AppImage 生成: $OUTPUT"
    chmod +x "$OUTPUT"
    ls -la "$OUTPUT"

    # 打包 udev 规则
    echo "   打包 udev 规则..."
    mkdir -p udev-rules
    cp install.dir/usr/lib/udev/rules.d/60-px.rules udev-rules/ 2>/dev/null || true
    cp libsigrok/contrib/60-libsigrok.rules udev-rules/ 2>/dev/null || true
    cp libsigrok/contrib/61-libsigrok-plugdev.rules udev-rules/ 2>/dev/null || true
    cp libsigrok/contrib/61-libsigrok-uaccess.rules udev-rules/ 2>/dev/null || true

    cat > udev-rules/install-udev-rules.sh << 'UDEVEOF'
#!/bin/bash
set -e
RULES_FILES="60-px.rules 60-libsigrok.rules 61-libsigrok-plugdev.rules 61-libsigrok-uaccess.rules"
if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行: sudo $0"
    exit 1
fi
if [ -d /usr/lib/udev/rules.d ]; then
    DEST=/usr/lib/udev/rules.d
elif [ -d /lib/udev/rules.d ]; then
    DEST=/lib/udev/rules.d
elif [ -d /etc/udev/rules.d ]; then
    DEST=/etc/udev/rules.d
else
    echo "错误: 未找到 udev 规则目录"
    exit 1
fi
echo "安装 udev 规则到 $DEST/"
for f in $RULES_FILES; do
    [ -f "$f" ] || continue
    cp "$f" "$DEST/$f"
    echo "  -> 已安装 $f"
done
udevadm control --reload-rules
udevadm trigger
echo "完成。PXView 设备现在无需 sudo 即可访问。"
echo "注意: 要使 'plugdev' 组规则生效,请将自己加入该组:"
echo "  sudo usermod -aG plugdev \$USER"
echo "然后重新登录 (或运行 'newgrp plugdev')。"
UDEVEOF
    chmod +x udev-rules/install-udev-rules.sh
    (cd udev-rules && zip -r "../PXView-Linux-udev-rules-$VERSION.zip" .)
    echo "   [OK] udev 规则打包: PXView-Linux-udev-rules-$VERSION.zip"
else
    echo " [7/7] 跳过 AppImage 打包 (--no-appimage)"
fi

echo ""
echo "================================================" 
echo " 构建完成!"
echo "================================================" 
if [ "$BUILD_APPIMAGE" = true ]; then
    echo " AppImage:  $SCRIPT_DIR/PXView-Linux-x86_64-AppImage-$VERSION.AppImage"
    echo " udev规则:  $SCRIPT_DIR/PXView-Linux-udev-rules-$VERSION.zip"
else
    echo " 安装目录:  $SCRIPT_DIR/install.dir/"
    echo " 可执行文件: $SCRIPT_DIR/install.dir/usr/bin/PXView"
fi
echo ""
echo " 运行:"
echo "   ./PXView-Linux-x86_64-AppImage-$VERSION.AppImage"
echo " 或:"
echo "   ./install.dir/usr/bin/PXView"
echo ""
echo " 安装 udev 规则 (USB 设备访问权限):"
echo "   sudo ./udev-rules/install-udev-rules.sh"
echo "================================================" 
