#!/bin/bash
# =============================================================================
# build_macos.sh
# PXView macOS 本机构建脚本 (ARM64 / x86_64 自动检测)
#
# 用法:
#   ./build_macos.sh              # 完整构建 (依赖安装 + 编译 + 打包 DMG)
#   ./build_macos.sh --no-deps    # 跳过依赖安装 (已安装好依赖时使用)
#   ./build_macos.sh --no-dmg     # 只编译安装,不打包 DMG
#   ./build_macos.sh --no-firmware # 跳过 fx2lafw 固件编译
#   ./build_macos.sh --no-webui   # 跳过 Web UI 构建
#
# 说明:
#   - 本脚本为原生编译,检测当前机器架构 (ARM64 或 x86_64)
#   - 如需 Universal Binary,请分别构建两个架构后手动用 lipo 合并
#   - DMG 签名使用 ad-hoc 签名 (codesign -),如需正式签名请修改 CODESIGN_IDENTITY
# =============================================================================
set -e

# ── 配置 ──────────────────────────────────────────────────────────────────
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

VERSION="${PXVIEW_VERSION:-1.5.4}"
INSTALL_PREFIX="$SCRIPT_DIR/install.dir"
CODESIGN_IDENTITY="-"  # ad-hoc 签名;正式分发请改为你的 Developer ID
OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"

# 检测当前架构
ARCH=$(uname -m)
if [ "$ARCH" = "arm64" ]; then
    ARCH_NAME="ARM64"
    CROSS_COMPILE=false
elif [ "$ARCH" = "x86_64" ]; then
    ARCH_NAME="x86_64"
    CROSS_COMPILE=false  # 在 Intel Mac 上原生编译
else
    echo "错误: 不支持的架构 $ARCH"
    exit 1
fi

# 命令行参数解析
INSTALL_DEPS=true
BUILD_DMG=true
BUILD_FIRMWARE=true
BUILD_WEBUI=true

for arg in "$@"; do
    case "$arg" in
        --no-deps)       INSTALL_DEPS=false ;;
        --no-dmg)        BUILD_DMG=false ;;
        --no-firmware)   BUILD_FIRMWARE=false ;;
        --no-webui)      BUILD_WEBUI=false ;;
        --help|-h)
            echo "用法: ./build_macos.sh [选项]"
            echo "  --no-deps        跳过依赖安装"
            echo "  --no-dmg         不打包 DMG"
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
echo " PXView macOS 本机构建"
echo " 架构:      $ARCH_NAME"
echo " 交叉编译:  $CROSS_COMPILE"
echo " 版本:      $VERSION"
echo " 签名:      $CODESIGN_IDENTITY (ad-hoc)"
echo " 部署目标:  macOS $OSX_DEPLOYMENT_TARGET"
echo "================================================"
echo ""

# ── 步骤 1: 检查 Homebrew ──────────────────────────────────────────────────
echo " [1/7] 检查 Homebrew..."
if [ "$ARCH" = "arm64" ]; then
    BREW_PREFIX="/opt/homebrew"
else
    BREW_PREFIX="/usr/local"
fi

if [ ! -f "$BREW_PREFIX/bin/brew" ]; then
    echo "   Homebrew 未安装,正在安装..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi
eval "$("$BREW_PREFIX/bin/brew" shellenv)"
echo "   [OK] Homebrew: $(brew --version | head -1)"
echo ""

# ── 步骤 2: 安装构建依赖 ──────────────────────────────────────────────────
if [ "$INSTALL_DEPS" = true ]; then
    echo " [2/7] 安装构建依赖..."
    brew update
    brew install cmake ninja gettext glib libusb zlib boost fftw python3 qt pkg-config libzip nettle libftdi sdcc
    echo "   [OK] 依赖安装完成"
else
    echo " [2/7] 跳过依赖安装 (--no-deps)"
fi
echo ""

# ── 步骤 3: 设置 Node.js (Web UI) ─────────────────────────────────────────
if [ "$BUILD_WEBUI" = true ]; then
    echo " [3/7] 检查 Node.js..."
    if ! command -v node >/dev/null 2>&1; then
        echo "   Node.js 未安装,正在安装..."
        brew install node
    fi
    echo "   Node.js: $(node --version)"
    echo "   npm:     $(npm --version)"
else
    echo " [3/7] 跳过 Node.js 检查 (--no-webui)"
fi
echo ""

# ── 步骤 4: 构建 fx2lafw 固件 (可选) ──────────────────────────────────────
if [ "$BUILD_FIRMWARE" = true ]; then
    echo " [4/7] 构建 fx2lafw 固件..."
    if command -v sdcc >/dev/null 2>&1; then
        bash build_fx2lafw.sh || echo "   [警告] fx2lafw 固件编译失败 (不影响主构建)"
    else
        echo "   [跳过] sdcc 未安装"
        echo "   安装: brew install sdcc"
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

export LDFLAGS="-L$(brew --prefix gettext)/lib"
export CPPFLAGS="-I$(brew --prefix gettext)/include"
export PKG_CONFIG_PATH="$(brew --prefix gettext)/lib/pkgconfig:$PKG_CONFIG_PATH"

# 架构标记 (本机构建不需要交叉编译,但如果在 ARM64 上指定 x86_64 则需要)
OSX_ARCH_FLAG=""
if [ "$CROSS_COMPILE" = true ]; then
    OSX_ARCH_FLAG="-DCMAKE_OSX_ARCHITECTURES=x86_64"
    echo "   配置: x86_64 交叉编译 (Rosetta 2)"
else
    echo "   配置: $ARCH_NAME 原生编译"
fi

cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
    $OSX_ARCH_FLAG \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$OSX_DEPLOYMENT_TARGET" \
    -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"

cd "$SCRIPT_DIR"
echo "   [OK] CMake 配置完成"
echo ""

# ── 步骤 6: 编译和安装 ────────────────────────────────────────────────────
echo " [6/7] 编译 PXView..."
cd build

if [ "$BUILD_WEBUI" = true ]; then
    echo "   构建 Web UI..."
    ninja install-webui || echo "   [警告] Web UI 构建跳过"
fi

echo "   编译主程序..."
ninja
echo "   安装..."
ninja install
cd "$SCRIPT_DIR"
echo "   [OK] 编译安装完成"
echo ""

# ── 步骤 7: 打包 DMG (可选) ───────────────────────────────────────────────
if [ "$BUILD_DMG" = true ]; then
    echo " [7/7] 打包 DMG..."
    MACDEPLOYQT="$(brew --prefix qt)/bin/macdeployqt"
    BUNDLE="$INSTALL_PREFIX/PXView.app"

    # ── Python framework 路径修正 ──
    # Homebrew python@3.x 把 framework 放在 <prefix>/Frameworks/Python.framework,
    # 但 macdeployqt 期望在 <prefix>/lib/Python.framework
    PY_PREFIX="$(brew --prefix python3 2>/dev/null \
                 || brew --prefix python@3.14 2>/dev/null \
                 || brew --prefix python@3.13 2>/dev/null \
                 || brew --prefix python@3.12 2>/dev/null)"
    if [ -n "$PY_PREFIX" ]; then
        mkdir -p "$PY_PREFIX/lib"
        if [ ! -e "$PY_PREFIX/lib/Python.framework" ] && [ -e "$PY_PREFIX/Frameworks/Python.framework" ]; then
            ln -s "$PY_PREFIX/Frameworks/Python.framework" "$PY_PREFIX/lib/Python.framework"
        fi
    fi

    # ── macdeployqt: 依赖收集 ──
    echo "   macdeployqt 依赖收集..."
    "$MACDEPLOYQT" "$BUNDLE" \
        -always-overwrite -verbose=1 \
        || true

    # ── 预先从 Homebrew 复制 macdeployqt 缺失的传递依赖 ──
    BREW_LIB="$(brew --prefix)/lib"
    mkdir -p "$BUNDLE/Contents/Frameworks"
    for dep in libbrotlicommon.1.dylib libsharpyuv.0.dylib libwebp.7.dylib; do
        real_path="$(readlink -f "$BREW_LIB/$dep" 2>/dev/null || echo "$BREW_LIB/$dep")"
        if [ -f "$real_path" ]; then
            real_name="$(basename "$real_path")"
            cp -f "$real_path" "$BUNDLE/Contents/Frameworks/$real_name"
            if [ "$real_name" != "$dep" ]; then
                rm -f "$BUNDLE/Contents/Frameworks/$dep"
                ln -s "$real_name" "$BUNDLE/Contents/Frameworks/$dep"
            fi
            install_name_tool -id "@rpath/$dep" "$BUNDLE/Contents/Frameworks/$real_name"
        fi
    done

    # ── 复制 Python 标准库到 bundled Python.framework ──
    echo "   复制 Python 标准库..."
    BUNDLED_FW="$BUNDLE/Contents/Frameworks/Python.framework"
    if [ -d "$BUNDLED_FW" ]; then
        for ver_dir in "$BUNDLED_FW"/Versions/3.*; do
            [ -d "$ver_dir" ] || continue
            py_ver=$(basename "$ver_dir")
            STDLIB_SRC=""
            for fw_base in \
                "$PY_PREFIX/Frameworks/Python.framework" \
                "$PY_PREFIX/lib/Python.framework" \
                "$(brew --prefix)/Frameworks/Python.framework"; do
                if [ -d "$fw_base/Versions/$py_ver/lib/python$py_ver/encodings" ]; then
                    STDLIB_SRC="$fw_base/Versions/$py_ver/lib/python$py_ver"
                    break
                fi
            done
            if [ -z "$STDLIB_SRC" ]; then
                for candidate in "$(brew --prefix)/lib/python$py_ver" "$PY_PREFIX/lib/python$py_ver"; do
                    if [ -d "$candidate/encodings" ]; then
                        STDLIB_SRC="$candidate"
                        break
                    fi
                done
            fi
            if [ -n "$STDLIB_SRC" ]; then
                mkdir -p "$ver_dir/lib"
                cp -R "$STDLIB_SRC" "$ver_dir/lib/"
                echo "   已复制 Python stdlib (python$py_ver)"
                if [ -d "$ver_dir/lib/python$py_ver/encodings" ]; then
                    echo "   [OK] encodings 模块验证通过"
                else
                    echo "   [警告] encodings 模块缺失"
                fi
            else
                echo "   [警告] 未找到 Python stdlib for $py_ver"
            fi
        done
        # 删除断链
        find "$BUNDLED_FW" -type l ! -exec test -e {} \; -delete 2>/dev/null || true
        # 创建 Current 符号链接
        if [ ! -e "$BUNDLED_FW/Versions/Current" ]; then
            latest_ver=$(ls -d "$BUNDLED_FW"/Versions/3.* 2>/dev/null | sort -V | tail -1)
            if [ -n "$latest_ver" ]; then
                ln -s "$(basename "$latest_ver")" "$BUNDLED_FW/Versions/Current"
            fi
        fi
    else
        echo "   [警告] 未找到 Python.framework"
    fi

    # ── 清除隔离标记 ──
    xattr -cr "$BUNDLE" 2>/dev/null || true

    # ── 递归签名 ──
    echo "   codesign 递归签名..."
    codesign --force --deep --sign "$CODESIGN_IDENTITY" "$BUNDLE"

    # ── 验证签名 ──
    if ! codesign --verify --deep --strict "$BUNDLE" 2>&1; then
        echo "   [错误] codesign 验证失败"
        exit 1
    fi
    echo "   [OK] 签名验证通过"

    # ── 生成 DMG ──
    echo "   hdiutil 生成 DMG..."
    DMG_NAME="PXView-macOS-$ARCH_NAME-$VERSION.dmg"
    hdiutil create -volname PXView -srcfolder "$BUNDLE" \
        -ov -format UDZO "$INSTALL_PREFIX/$DMG_NAME"
    echo "   [OK] DMG 生成: $DMG_NAME"
    ls -la "$INSTALL_PREFIX"/*.dmg
else
    echo " [7/7] 跳过 DMG 打包 (--no-dmg)"
fi

echo ""
echo "================================================"
echo " 构建完成!"
echo "================================================"
if [ "$BUILD_DMG" = true ]; then
    echo " DMG:  $SCRIPT_DIR/install.dir/PXView-macOS-$ARCH_NAME-$VERSION.dmg"
else
    echo " 安装目录: $SCRIPT_DIR/install.dir/"
    echo " App:      $SCRIPT_DIR/install.dir/PXView.app"
fi
echo ""
echo " 运行:"
echo "   open install.dir/PXView.app"
echo " 或安装 DMG:"
echo "   open install.dir/PXView-macOS-$ARCH_NAME-$VERSION.dmg"
echo ""
echo " 提示: 如需 Universal Binary (ARM64 + x86_64),"
echo "   请在 Intel Mac 和 Apple Silicon Mac 上分别构建,"
echo "   然后用 lipo 合并:"
echo "   lipo -create arm64/PXView.app/Contents/MacOS/PXView \\"
echo "              x86_64/PXView.app/Contents/MacOS/PXView \\"
echo "              -output universal/PXView.app/Contents/MacOS/PXView"
echo "================================================"
