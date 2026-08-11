#!/bin/bash
# =============================================================================
# build_windows.sh
# PXView Windows 本机构建脚本 (x86_64, MSYS2/UCRT64)
#
# 基于 .github/workflows/build.yml Windows 作业,保持本地与 CI 一致
#
# 前置条件:
#   1. 安装 MSYS2: https://www.msys2.org/
#      或: choco install msys2
#   2. 在 MSYS2 UCRT64 终端中运行此脚本
#
# 用法:
#   ./build_windows.sh              # 完整构建 (依赖安装 + 编译 + 打包)
#   ./build_windows.sh --no-deps    # 跳过依赖安装 (已安装好依赖时使用)
#   ./build_windows.sh --no-package # 只编译安装,不打包
#   ./build_windows.sh --no-firmware # 跳过 fx2lafw 固件编译
#   ./build_windows.sh --no-webui   # 跳过 Web UI 构建
#   ./build_windows.sh --no-nsis    # 跳过 NSIS 安装程序创建
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
BUILD_NSIS=true

for arg in "$@"; do
    case "$arg" in
        --no-deps)       INSTALL_DEPS=false ;;
        --no-package)   BUILD_PACKAGE=false; BUILD_NSIS=false ;;
        --no-firmware)  BUILD_FIRMWARE=false ;;
        --no-webui)     BUILD_WEBUI=false ;;
        --no-nsis)      BUILD_NSIS=false ;;
        --help|-h)
            echo "用法: ./build_windows.sh [选项]"
            echo "  --no-deps        跳过依赖安装"
            echo "  --no-package     不打包 (隐含 --no-nsis)"
            echo "  --no-firmware    跳过 fx2lafw 固件编译"
            echo "  --no-webui       跳过 Web UI 构建"
            echo "  --no-nsis        跳过 NSIS 安装程序创建"
            echo "  --help           显示此帮助"
            exit 0
            ;;
        *)
            echo "未知选项: $arg (使用 --help 查看帮助)"
            exit 1
            ;;
    esac
done

TOTAL_STEPS=7
echo "================================================"
echo " PXView Windows 本机构建 (UCRT64)"
echo " MSYS2:    ${MSYSTEM:-unknown}"
echo " 版本:     $VERSION"
echo "================================================"
echo ""

# ── 检查 MSYS2 环境 ──────────────────────────────────────────────────────
if [ -z "$MSYSTEM" ]; then
    echo " [错误] 此脚本必须在 MSYS2 UCRT64 终端中运行!"
    echo "   请安装 MSYS2 (https://www.msys2.org/)"
    echo "   然后打开 'MSYS2 UCRT64' 终端运行此脚本"
    exit 1
fi

if [ "$MSYSTEM" != "UCRT64" ]; then
    echo " [警告] 当前环境为 $MSYSTEM,建议使用 UCRT64"
    echo "   请在 'MSYS2 UCRT64' 终端中运行此脚本"
    echo ""
    read -p " 是否继续? (y/N): " CONTINUE
    [ "$CONTINUE" = "y" ] || exit 1
fi
echo ""

# ── 步骤 1: 安装构建依赖 ──────────────────────────────────────────────────
# 包列表与 .github/workflows/build.yml Windows 作业一致
# 注意: 使用 pkgconf 而非 pkg-config,避免与 cmake 依赖冲突
if [ "$INSTALL_DEPS" = true ]; then
    echo " [1/${TOTAL_STEPS}] 安装构建依赖..."
    pacman -S --needed --noconfirm \
        mingw-w64-ucrt-x86_64-toolchain \
        mingw-w64-ucrt-x86_64-cmake \
        mingw-w64-ucrt-x86_64-ninja \
        mingw-w64-ucrt-x86_64-pkgconf \
        mingw-w64-ucrt-x86_64-qt6-base \
        mingw-w64-ucrt-x86_64-qt6-svg \
        mingw-w64-ucrt-x86_64-qt6-websockets \
        mingw-w64-ucrt-x86_64-glib2 \
        mingw-w64-ucrt-x86_64-boost \
        mingw-w64-ucrt-x86_64-fftw \
        mingw-w64-ucrt-x86_64-zlib \
        mingw-w64-ucrt-x86_64-python \
        mingw-w64-ucrt-x86_64-libzip \
        mingw-w64-ucrt-x86_64-nettle \
        mingw-w64-ucrt-x86_64-libftdi \
        mingw-w64-ucrt-x86_64-libusb \
        mingw-w64-ucrt-x86_64-lcms2 \
        zip unzip curl

    # sdcc: 编译 fx2lafw 固件 (可选,CI 中不安装)
    if [ "$BUILD_FIRMWARE" = true ]; then
        echo "   安装 sdcc (fx2lafw 固件编译器)..."
        pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-sdcc 2>/dev/null \
            || echo "   [跳过] sdcc 安装失败,fx2lafw 固件将跳过"
    fi

    echo "   [OK] 依赖安装完成"
else
    echo " [1/${TOTAL_STEPS}] 跳过依赖安装 (--no-deps)"
fi
echo ""

# ── 步骤 2: 构建 fx2lafw 固件 (可选) ──────────────────────────────────────
if [ "$BUILD_FIRMWARE" = true ]; then
    echo " [2/${TOTAL_STEPS}] 构建 fx2lafw 固件..."
    bash build_fx2lafw.sh || echo "   [警告] fx2lafw 固件编译失败 (不影响主构建)"
else
    echo " [2/${TOTAL_STEPS}] 跳过 fx2lafw 固件 (--no-firmware)"
fi
echo ""

# ── 步骤 3: 配置 CMake ────────────────────────────────────────────────────
echo " [3/${TOTAL_STEPS}] 配置 CMake..."
mkdir -p build
cd build
cmake .. -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
cd "$SCRIPT_DIR"
echo "   [OK] CMake 配置完成"
echo ""

# ── 步骤 4: 构建 Web UI (可选) ─────────────────────────────────────────────
if [ "$BUILD_WEBUI" = true ]; then
    echo " [4/${TOTAL_STEPS}] 构建 Web UI..."
    if command -v npm >/dev/null 2>&1 || command -v node >/dev/null 2>&1; then
        cd build
        ninja install-webui || echo "   [警告] Web UI 构建跳过"
        cd "$SCRIPT_DIR"
    else
        echo "   [跳过] npm/node 未安装"
        echo "   安装: pacman -S mingw-w64-ucrt-x86_64-nodejs"
    fi
else
    echo " [4/${TOTAL_STEPS}] 跳过 Web UI (--no-webui)"
fi
echo ""

# ── 步骤 5: 编译 PXView ───────────────────────────────────────────────────
echo " [5/${TOTAL_STEPS}] 编译 PXView..."
cd build
ninja -j $(nproc)
cd "$SCRIPT_DIR"
echo "   [OK] 编译完成"
echo ""

# ── 步骤 6: 安装 ──────────────────────────────────────────────────────────
echo " [6/${TOTAL_STEPS}] 安装..."
cd build
ninja install
cd "$SCRIPT_DIR"
echo "   [OK] 安装完成"
echo ""

# ── 步骤 7: 打包 ─────────────────────────────────────────────────────────
if [ "$BUILD_PACKAGE" = true ]; then
    echo " [7/${TOTAL_STEPS}] 打包..."

    # Python runtime is bundled entirely from MSYS2 (see window/package.sh):
    #   - python3XX.dll / libffi-8.dll via copy-deps.sh (ldd of PXView.exe)
    #   - .pyd extension modules from /ucrt64/lib/pythonX.Y/lib-dynload
    #   - stdlib from /ucrt64/lib/pythonX.Y
    # We intentionally do NOT pull python.org embeddable zip, because its
    # libffi build is incompatible with MinGW's _ctypes.pyd (breaks ctypes).

    # 打包 (收集 DLL 和资源)
    echo "   运行 window/package.sh..."
    chmod +x window/copy-deps.sh
    bash window/package.sh

    if [ -f "$SCRIPT_DIR/package/PXView.exe" ]; then
        echo "   [OK] 打包完成"
    else
        echo "   [警告] package/PXView.exe 未找到"
        echo "   手动运行: bash window/package.sh"
    fi

    # NSIS 安装程序 (可选)
    if [ "$BUILD_NSIS" = true ]; then
        echo ""
        echo "   创建 NSIS 安装程序..."
        if command -v makensis >/dev/null 2>&1; then
            makensis "/DPRODUCT_VERSION=$VERSION" window_nisi.nsi
            INSTALLER="PXView-Windows-x86_64-Setup-$VERSION.exe"
            if [ -f "$INSTALLER" ]; then
                echo "   [OK] NSIS 安装程序: $INSTALLER"
            else
                echo "   [警告] NSIS 安装程序未生成"
            fi
        else
            echo "   [跳过] makensis 未安装"
            echo "   安装 NSIS: https://nsis.sourceforge.io/"
        fi
    fi
else
    echo " [7/${TOTAL_STEPS}] 跳过打包 (--no-package)"
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
echo " 提示: 如需创建 NSIS 安装程序,请安装 NSIS 后重新运行"
echo "================================================"
