#!/bin/bash
# =============================================================================
# build_windows.sh
# PXView Windows 本机构建脚本 (x86_64, MSYS2/MinGW64)
#
# 前置条件:
#   1. 安装 MSYS2: https://www.msys2.org/
#      或: choco install msys2
#   2. 在 MSYS2 MinGW64 终端中运行此脚本
#
# 用法:
#   ./build_windows.sh              # 完整构建 (依赖安装 + 编译 + 打包)
#   ./build_windows.sh --no-deps    # 跳过依赖安装 (已安装好依赖时使用)
#   ./build_windows.sh --no-package # 只编译安装,不打包
#   ./build_windows.sh --no-firmware # 跳过 fx2lafw 固件编译
#   ./build_windows.sh --no-webui   # 跳过 Web UI 构建
#
# 参考: windows.md
# =============================================================================
set -e

# ── 配置 ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

VERSION="${PXVIEW_VERSION:-1.5.4}"
INSTALL_PREFIX="$SCRIPT_DIR/install.dir"

# 命令行参数解析
INSTALL_DEPS=true
BUILD_PACKAGE=true
BUILD_FIRMWARE=true
BUILD_WEBUI=true

for arg in "$@"; do
    case "$arg" in
        --no-deps)       INSTALL_DEPS=false ;;
        --no-package)   BUILD_PACKAGE=false ;;
        --no-firmware)  BUILD_FIRMWARE=false ;;
        --no-webui)     BUILD_WEBUI=false ;;
        --help|-h)
            echo "用法: ./build_windows.sh [选项]"
            echo "  --no-deps        跳过依赖安装"
            echo "  --no-package     不打包"
            echo "  --no-firmware    跳过 fx2lafw 固件编译"
            echo "  --no-webui       跳过 Web UI 构建"
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
echo " PXView Windows 本机构建 (MinGW64)"
echo " MSYS2:    ${MSYSTEM:-unknown}"
echo " 版本:     $VERSION"
echo "================================================"
echo ""

# ── 检查 MSYS2 环境 ──────────────────────────────────────────────────────
if [ -z "$MSYSTEM" ]; then
    echo " [错误] 此脚本必须在 MSYS2 MinGW64 终端中运行!"
    echo "   请安装 MSYS2 (https://www.msys2.org/)"
    echo "   然后打开 'MSYS2 MinGW64' 终端运行此脚本"
    exit 1
fi

if [ "$MSYSTEM" != "MINGW64" ]; then
    echo " [警告] 当前环境为 $MSYSTEM,建议使用 MINGW64"
    echo "   请在 'MSYS2 MinGW64' 终端中运行此脚本"
    echo ""
    read -p " 是否继续? (y/N): " CONTINUE
    [ "$CONTINUE" = "y" ] || exit 1
fi
echo ""

# ── 步骤 1: 安装构建依赖 ──────────────────────────────────────────────────
if [ "$INSTALL_DEPS" = true ]; then
    echo " [1/6] 安装构建依赖..."
    pacman -S --needed --noconfirm \
        mingw-w64-x86_64-pkg-config \
        mingw-w64-x86_64-libusb \
        mingw-w64-x86_64-toolchain \
        mingw-w64-x86_64-boost \
        mingw-w64-x86_64-python \
        mingw-w64-x86_64-cmake \
        mingw-w64-x86_64-qt6-base \
        mingw-w64-x86_64-qt6-svg \
        mingw-w64-x86_64-qt6-websockets \
        mingw-w64-x86_64-glib2 \
        mingw-w64-x86_64-fftw \
        mingw-w64-x86_64-zlib \
        mingw-w64-x86_64-nlohmann-json \
        mingw-w64-x86_64-libzip \
        liblzma liblzma-devel \
        mingw-w64-x86_64-lcms2 \
        curl unzip

    # 可选依赖 (非 PXLogic 硬件必需)
    echo "   安装可选依赖 (FTDI/rdtech-tc 驱动)..."
    pacman -S --needed --noconfirm \
        mingw-w64-x86_64-libftdi \
        mingw-w64-x86_64-nettle \
        2>/dev/null || echo "   [跳过] 可选依赖安装失败 (不影响 PXLogic 硬件)"

    # sdcc: 编译 fx2lafw 固件
    if [ "$BUILD_FIRMWARE" = true ]; then
        echo "   安装 sdcc (fx2lafw 固件编译器)..."
        pacman -S --needed --noconfirm mingw-w64-x86_64-sdcc 2>/dev/null \
            || echo "   [警告] sdcc 安装失败,fx2lafw 固件将跳过"
    fi

    echo "   [OK] 依赖安装完成"
else
    echo " [1/6] 跳过依赖安装 (--no-deps)"
fi
echo ""

# ── 步骤 2: 下载 Python embed ─────────────────────────────────────────────
echo " [2/6] 准备 Python embed..."
PYVER=$(python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}')")
PYMINOR=$(python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
echo "   MSYS2 Python 版本: $PYVER"

if [ ! -d "$SCRIPT_DIR/python" ] || [ ! -f "$SCRIPT_DIR/python/python3$PYMINOR.dll" ]; then
    echo "   下载 Python $PYVER embed..."
    curl -L "https://www.python.org/ftp/python/$PYVER/python-$PYVER-embed-amd64.zip" -o python-embed.zip
    rm -rf "$SCRIPT_DIR/python"
    mkdir -p "$SCRIPT_DIR/python"
    unzip -q python-embed.zip -d "$SCRIPT_DIR/python"
    echo "   [OK] Python embed 下载完成"
else
    echo "   [OK] Python embed 已存在,跳过下载"
fi
echo ""

# ── 步骤 3: 构建 fx2lafw 固件 (可选) ──────────────────────────────────────
if [ "$BUILD_FIRMWARE" = true ]; then
    echo " [3/6] 构建 fx2lafw 固件..."
    if command -v sdcc >/dev/null 2>&1; then
        bash build_fx2lafw.sh || echo "   [警告] fx2lafw 固件编译失败 (不影响主构建)"
    else
        echo "   [跳过] sdcc 未安装"
        echo "   安装: pacman -S mingw-w64-x86_64-sdcc"
    fi
else
    echo " [3/6] 跳过 fx2lafw 固件 (--no-firmware)"
fi
echo ""

# ── 步骤 4: 配置 CMake ────────────────────────────────────────────────────
echo " [4/6] 配置 CMake..."
rm -rf build install.dir
mkdir -p build
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=RELEASE \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -G Ninja
cd "$SCRIPT_DIR"
echo "   [OK] CMake 配置完成"
echo ""

# ── 步骤 5: 编译和安装 ────────────────────────────────────────────────────
echo " [5/6] 编译 PXView..."
cd build

if [ "$BUILD_WEBUI" = true ]; then
    echo "   构建 Web UI..."
    # Web UI 需要 Node.js (可从 MSYS2 安装或系统安装)
    if command -v npm >/dev/null 2>&1 || command -v node >/dev/null 2>&1; then
        ninja install-webui || echo "   [警告] Web UI 构建跳过"
    else
        echo "   [跳过] npm/node 未安装"
        echo "   安装: pacman -S mingw-w64-x86_64-nodejs"
    fi
fi

echo "   编译主程序..."
ninja
echo "   安装..."
ninja install
cd "$SCRIPT_DIR"
echo "   [OK] 编译安装完成"
echo ""

# ── 步骤 6: 打包 ──────────────────────────────────────────────────────────
if [ "$BUILD_PACKAGE" = true ]; then
    echo " [6/6] 打包..."

    # 使用项目自带的打包脚本
    echo "   运行 window/package.sh..."
    bash window/package.sh

    # 检查 package 目录
    if [ -f "$SCRIPT_DIR/package/PXView.exe" ]; then
        echo "   [OK] 打包完成"
        ls -la "$SCRIPT_DIR/package/"
    else
        echo "   [警告] package/PXView.exe 未找到"
        echo "   手动运行: bash window/package.sh"
    fi
else
    echo " [6/6] 跳过打包 (--no-package)"
fi

echo ""
echo "================================================"
echo " 构建完成!"
echo "================================================"
if [ "$BUILD_PACKAGE" = true ] && [ -f "$SCRIPT_DIR/package/PXView.exe" ]; then
    echo " 打包目录:  $SCRIPT_DIR/package/"
    echo " 可执行文件: $SCRIPT_DIR/package/PXView.exe"
    echo ""
    echo " 运行:"
    echo "   package/PXView.exe"
else
    echo " 安装目录:  $SCRIPT_DIR/install.dir/"
    echo " 可执行文件: $SCRIPT_DIR/install.dir/bin/PXView.exe"
    echo ""
    echo " 运行:"
    echo "   install.dir/bin/PXView.exe"
fi
echo ""
echo " 提示: 如需创建 NSIS 安装程序,请使用 window_nsi.nsi"
echo "================================================"
