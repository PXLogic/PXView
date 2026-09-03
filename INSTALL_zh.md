# 安装指南

## 系统要求

- git
- gcc (>= 9.0) 或 clang
- g++
- make
- cmake >= 3.16
- ninja-build
- Qt >= 6.11.0 (Core, Gui, Widgets, Svg, Concurrent, WebSockets)
- libglib >= 2.32.0
- zlib
- libusb-1.0 >= 1.0.16
- libboost >= 1.42
- libfftw3 >= 3.3
- libzip
- python >= 3.8
- pkg-config >= 0.22
- nlohmann-json >= 3.2.0（header-only，若未安装则自动从 GitHub 下载）
- sdcc >= 4.0（可选；用于编译 fx2lafw 固件，支持基于 Cypress FX2 芯片的逻辑分析仪）

### 可选依赖（非 PXLogic 硬件所需）

以下依赖**不是** PXLogic 硬件所必需的，仅在需要特定旧型号驱动时安装：

- **libftdi1** — FTDI 驱动（asix-sigma、chronovu-la、ftdi-la、ikalogic-scanaplus、pipistrello-ols）。未安装时仅打印警告并禁用这些驱动，libsigrok 其余部分正常编译。
- **nettle** — rdtech-tc 驱动（AES-256 固件解密）。未安装时仅禁用该驱动。

---

## Linux

### 步骤 1：安装依赖

#### Ubuntu / Debian（如 Ubuntu 22.04 / 24.04）：

```bash
sudo apt update
sudo apt install git gcc g++ make cmake ninja-build libglib2.0-dev zlib1g-dev \
  libusb-1.0-0-dev libboost-dev libfftw3-dev libzip-dev python3-dev libudev-dev \
  pkg-config libgl1-mesa-dev libxkbcommon-dev libvulkan-dev python3-pip sdcc
```

**在 Ubuntu 上安装 Qt 6.11：**
默认 apt 仓库可能不提供 Qt 6.11，需要使用 `aqtinstall` 手动安装。

*缓存提示*：`aqtinstall`（"Another Qt Installer"）在解压后会立即删除下载的归档文件以节省空间，因为它专为 CI/CD 环境设计。为避免每次清理或移动项目时重新下载 Qt，强烈建议将其安装到全局用户目录。

```bash
pip3 install aqtinstall
# 将 Qt 6.11 全局安装到 ~/Qt（只需运行一次！）
aqt install-qt linux desktop 6.11.0 linux_gcc_64 --outputdir ~/Qt
```

#### Fedora：

```bash
sudo dnf install git gcc gcc-c++ make cmake ninja-build libtool pkgconf \
  glib2-devel zlib-devel libudev-devel libusb1-devel python3-devel boost-devel \
  fftw-devel libzip-devel qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwebsockets-devel
```
*（Fedora 通常在标准仓库中提供较新的 Qt6 版本）*

#### Arch Linux：

```bash
sudo pacman -S base-devel git cmake ninja glib2 zlib libusb python boost \
  qt6-base qt6-svg qt6-websockets fftw libzip sdcc
```

#### 可选依赖：

```bash
# Ubuntu / Debian：
sudo apt install libftdi1-dev libnettle-dev

# Fedora：
sudo dnf install libftdi-devel nettle-devel

# Arch Linux：
sudo pacman -S libftdi nettle
```

### 步骤 2：获取 PXView 源代码

```bash
git clone https://github.com/PXLogic/PXView
cd PXView
```

### 步骤 3：编译

如果在步骤 1 中通过 `aqtinstall` 手动安装了 Qt（如在 Ubuntu 上），必须告诉 CMake Qt 的位置。如果使用系统包管理器安装（Arch/Fedora），可以省略 `CMAKE_PREFIX_PATH` 参数。

```bash
mkdir build && cd build

# Ubuntu 使用 aqtinstall（使用全局 ~/Qt 路径）：
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"

# Arch / Fedora（系统 Qt）：
# cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

ninja
sudo ninja install
```

当 `CMAKE_INSTALL_PREFIX` 为 `/usr` 或 `/usr/local` 时，udev rules 会安装到系统路径（如 `/usr/lib/udev/rules.d`），需要 `sudo` 权限。

**直接运行二进制文件（非 AppImage 方式）：** 如果 Qt 是通过 `aqtinstall` 安装的（不在系统库路径中），运行前必须设置 `LD_LIBRARY_PATH`：

```bash
LD_LIBRARY_PATH="$HOME/Qt/6.11.0/gcc_64/lib" ./install.dir/bin/PXView
```

也可以添加到 shell 配置文件中，方便日常使用：

```bash
echo 'export LD_LIBRARY_PATH="$HOME/Qt/6.11.0/gcc_64/lib:$LD_LIBRARY_PATH"' >> ~/.bashrc
source ~/.bashrc
```

`ninja install` 会自动安装已构建的 MCP Web 客户端。如果 Web 客户端尚未构建，则会静默跳过。

### 步骤 4（推荐）：打包为自解压 .sh 安装器（Linux）

PXView 现在改用 `packaging/make-installer.sh` 生成的 `.sh` 自解压安装器。它会把 `install.dir/usr` 暂存为可移植负载，并安装到 `/opt/PXView`，自动完成系统级集成（udev 规则、图标、两个桌面项、CLI 包装、卸载脚本）。

```bash
cd build && ninja -j "$(nproc)" && ninja install
cd ..

# 打包运行时库（Qt、插件、Python 标准库、白名单内的系统依赖）并写入 bin/qt.conf。
# 不打包 WebKitGTK/GTK/GStreamer。详见 packaging/bundle-runtime-libs.sh。
bash packaging/bundle-runtime-libs.sh install.dir

PXVIEW_VERSION=1.5.9 bash packaging/make-installer.sh
# -> PXView-Linux-x86_64-Installer-1.5.9.sh
sudo ./PXView-Linux-x86_64-Installer-1.5.9.sh
```

Tauri Agent（`PXView-Agent`）以普通文件分发，运行时从目标系统解析 `libwebkit2gtk-4.1`——**刻意不打包**，因为打包 WebKitGTK 会造成 ABI 混搭（打包库 vs 系统 `WebKitWebProcess`/`WebKitNetworkProcess`），导致 Agent 窗口全白。`packaging/check-webkit-stack.sh` 一旦检测到桌面/WebKit 库泄漏进安装树就会让构建失败。

> **说明**：下面的 AppImage 章节仅作为历史参考保留。

### 步骤 4（历史参考）：打包为 AppImage（Linux）

AppImage 将应用程序及其依赖打包为单个可移植文件。由于 AppImage 是用户态的便携包，**udev rules、desktop 文件、文档等系统级文件不应打包进去**，需要单独安装。

#### 4.1 使用本地安装前缀编译

打包 AppImage 时，应使用本地前缀（如 `../install.dir`）而非系统前缀（`/usr`），这样 udev rules 等系统文件会安装到本地目录而非系统目录，避免需要 root 权限：

```bash
mkdir build && cd build

# 使用本地前缀 — udev rules 等会安装到 install.dir 下
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../install.dir/usr \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"
ninja
ninja install
```

> **说明**：linuxdeploy 要求 AppDir 包含 `usr/{bin,share,lib}` 结构，因此安装前缀设置为 `../install.dir/usr`（而非 `../install.dir`）。

#### 4.2 打包 Python 标准库（协议解码器必需）

libsigrokdecode 内嵌了 Python 解释器，需要 Python 标准库（`encodings` 等模块）。如果不打包，当用户系统的 Python 版本与构建机器不一致时（如构建用 3.10，用户是 3.12），会出现 `ModuleNotFoundError: No module named 'encodings'` 启动失败。

```bash
PY_VERSION=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
echo "打包 Python $PY_VERSION 标准库"

# 复制标准库 .py 文件
mkdir -p install.dir/usr/lib/python$PY_VERSION
cp -a /usr/lib/python$PY_VERSION/* install.dir/usr/lib/python$PY_VERSION/ 2>/dev/null || true

# 复制 lib-dynload（encodings 等内置模块的 .so 文件）
mkdir -p install.dir/usr/lib/python$PY_VERSION/lib-dynload
cp -a /usr/lib/python$PY_VERSION/lib-dynload/* install.dir/usr/lib/python$PY_VERSION/lib-dynload/ 2>/dev/null || true

# 删除不必要的测试和 __pycache__ 以减小体积
rm -rf install.dir/usr/lib/python$PY_VERSION/test
find install.dir/usr/lib/python$PY_VERSION -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

# 验证 encodings 模块存在
if [ -d install.dir/usr/lib/python$PY_VERSION/encodings ]; then
  echo "OK: encodings 模块已找到"
else
  echo "警告: encodings 未找到，AppImage 的 Python 可能无法运行"
fi
```

#### 4.3 编译 qtwayland 平台插件（可选，用于 Wayland 支持）

`aqtinstall` 不提供 qtwayland 客户端平台插件。如果希望 AppImage 同时支持 X11（xcb）和 Wayland，需要从源码编译 qtwayland：

```bash
sudo apt install -y wayland-protocols libwayland-dev

git clone --branch v6.11.0 --depth 1 https://github.com/qt/qtwayland.git
cd qtwayland
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64" \
  -DCMAKE_INSTALL_PREFIX="$HOME/Qt/6.11.0/gcc_64" \
  -DQT_FEATURE_wayland_server=OFF \
  -DQT_FEATURE_wayland_client=ON
cmake --build build --parallel $(nproc)
cmake --install build
cd ..
```

#### 4.4 打包 AppImage

```bash
# 回到项目根目录
cd ..

wget -c "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -c "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x linuxdeploy*.AppImage

export QMAKE="$HOME/Qt/6.11.0/gcc_64/bin/qmake"
export LD_LIBRARY_PATH="$HOME/Qt/6.11.0/gcc_64/lib:$LD_LIBRARY_PATH"
export OUTPUT="PXView-x86_64.AppImage"

./linuxdeploy-x86_64.AppImage \
  --appdir install.dir \
  -e install.dir/usr/bin/PXView \
  -d install.dir/usr/share/applications/pxview.desktop \
  --plugin qt \
  --output appimage
```

#### 4.5 安装系统级文件（AppImage 外）

AppImage 不包含以下系统级文件，用户需要手动安装一次：

**udev rules（硬件访问权限）：**
```bash
sudo cp install.dir/usr/lib/udev/rules.d/60-px.rules /etc/udev/rules.d/60-px.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

**Desktop 文件（应用菜单集成）：**
```bash
sudo cp install.dir/usr/share/applications/pxview.desktop /usr/share/applications/pxview.desktop
```

**文档和资源（可选）：**
```bash
# 文档、用户手册等已包含在 AppImage 内部的 share/PXView/ 中
# 如需系统级安装：
sudo cp -r install.dir/usr/share/PXView /usr/share/PXView
```

> **提示**：也可以编写一个 `install.sh` 脚本随 AppImage 一起分发，自动完成上述系统级文件的安装。

---

## macOS

### 步骤 1：安装依赖（Homebrew）

```bash
brew install git cmake ninja gettext glib libusb zlib boost fftw python3 qt pkg-config libzip sdcc
```

*（注意：如果默认的 `qt` brew formula 尚未更新到 6.11.0，或未自动链接，可能需要手动查找 brew Qt 安装路径，通常为 `/opt/homebrew/opt/qt`）*

可选依赖：

```bash
brew install libftdi nettle
```

### 步骤 2：获取 PXView 源代码

```bash
git clone https://github.com/PXLogic/PXView
cd PXView
```

### 步骤 3：编译

```bash
mkdir build && cd build

export LDFLAGS="-L$(brew --prefix gettext)/lib"
export CPPFLAGS="-I$(brew --prefix gettext)/include"
export PKG_CONFIG_PATH="$(brew --prefix gettext)/lib/pkgconfig:$PKG_CONFIG_PATH"

cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DCMAKE_INSTALL_PREFIX=../install.dir

ninja
ninja install
```

### 步骤 4：创建 DMG

macOS 上打包 DMG 使用 `macdeployqt` 收集所有依赖到 `.app` 包中，然后用 `codesign` 进行 ad-hoc 签名，最后用 `hdiutil` 创建磁盘映像。

#### 4.1 修正 Python framework 路径

Homebrew 的 `python@3.x` 将 Python framework 放在 `<prefix>/Frameworks/Python.framework`，但 `macdeployqt` 期望在 `<prefix>/lib/Python.framework`。创建符号链接修复此问题：

```bash
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
```

#### 4.2 运行 macdeployqt

```bash
MACDEPLOYQT="$(brew --prefix qt)/bin/macdeployqt"

$MACDEPLOYQT install.dir/PXView.app \
  -always-overwrite -verbose=1 \
  || true   # macdeployqt 对找不到的 rpath 返回非零，但已复制大部分依赖
```

#### 4.3 复制缺失的传递依赖

`macdeployqt` 可能无法复制 Qt WebEngine/Pdf 的三个传递依赖（虽然 PXView 不直接使用它们）。从 Homebrew 手动复制：

```bash
BREW_LIB="$(brew --prefix)/lib"
mkdir -p install.dir/PXView.app/Contents/Frameworks

for dep in libbrotlicommon.1.dylib libsharpyuv.0.dylib libwebp.7.dylib; do
  real_path="$(readlink -f "$BREW_LIB/$dep" 2>/dev/null || echo "$BREW_LIB/$dep")"
  if [ -f "$real_path" ]; then
    real_name="$(basename "$real_path")"
    cp -f "$real_path" install.dir/PXView.app/Contents/Frameworks/"$real_name"
    if [ "$real_name" != "$dep" ]; then
      rm -f install.dir/PXView.app/Contents/Frameworks/"$dep"
      ln -s "$real_name" install.dir/PXView.app/Contents/Frameworks/"$dep"
    fi
    install_name_tool -id "@rpath/$dep" install.dir/PXView.app/Contents/Frameworks/"$real_name"
  fi
done
```

#### 4.4 复制 Python 标准库到 bundled Python.framework

`macdeployqt` 只复制 framework 的 Mach-O 二进制（Python 共享库），**不复制** `lib/python3.X/` 目录（含 `encodings`、`os.py` 等纯 Python 模块）。Homebrew 的 stdlib 位于 Python.framework 内部。如果不复制，在未安装相同 Homebrew Python 的机器上会崩溃，报 `Fatal Python error: Failed to import encodings module`。

```bash
BUNDLED_FW="install.dir/PXView.app/Contents/Frameworks/Python.framework"
if [ -d "$BUNDLED_FW" ]; then
  for ver_dir in "$BUNDLED_FW"/Versions/3.*; do
    [ -d "$ver_dir" ] || continue
    py_ver=$(basename "$ver_dir")

    # 在各种 Homebrew 路径中搜索 stdlib
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
    # 也检查 brew prefix/lib（非 framework 安装方式）
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
      echo "已复制 Python stdlib (python$py_ver) 到 bundled framework"
      if [ -d "$ver_dir/lib/python$py_ver/encodings" ]; then
        echo "验证通过: encodings 模块存在"
      else
        echo "警告: 复制后 encodings 模块缺失"
      fi
    else
      echo "警告: 未找到 python$py_ver 的 stdlib"
    fi
  done

  # 删除复制 stdlib 时引入的断链（如 site-packages 指向 Homebrew 路径）
  # 断链会导致 xattr -cr 和 codesign --verify 失败
  find "$BUNDLED_FW" -type l ! -exec test -e {} \; -delete 2>/dev/null || true

  # 创建 Current 符号链接（macdeployqt 可能不复制）
  if [ ! -e "$BUNDLED_FW/Versions/Current" ]; then
    latest_ver=$(ls -d "$BUNDLED_FW"/Versions/3.* 2>/dev/null | sort -V | tail -1)
    if [ -n "$latest_ver" ]; then
      ln -s "$(basename "$latest_ver")" "$BUNDLED_FW/Versions/Current"
      echo "已创建 Current 符号链接 -> $(basename "$latest_ver")"
    fi
  fi
else
  echo "警告: bundle 中未找到 Python.framework"
fi
```

#### 4.5 签名并创建 DMG

```bash
# 清除 Homebrew 库可能带的隔离标记
# （|| true: 容忍可能残留的断链）
xattr -cr install.dir/PXView.app 2>/dev/null || true

# 递归签名整个 .app 包
# 必须用 --deep：framework 的资源封印必须由 framework 目录签名建立，
# 单独签 framework 内二进制会破坏资源封印。
codesign --force --deep --sign - install.dir/PXView.app

# 验证签名
codesign --verify --deep --strict install.dir/PXView.app

# 创建 DMG
hdiutil create -volname PXView -srcfolder install.dir/PXView.app \
  -ov -format UDZO install.dir/PXView.dmg
```

### 步骤 5：在 ARM64 Mac 上编译 x86_64 版本（Rosetta 2 交叉编译）

Apple Silicon Mac（M1/M2/M3）可以通过 Rosetta 2 和 x86_64 Homebrew 编译 x86_64 二进制，无需 Intel Mac 即可生成 x86_64 DMG。

#### 5.1 安装 Rosetta 2 和 x86_64 Homebrew

```bash
# 安装 Rosetta 2
softwareupdate --install-rosetta --agree-to-license

# 安装 x86_64 Homebrew 到 /usr/local
#（ARM64 Homebrew 在 /opt/homebrew，x86_64 Homebrew 在 /usr/local，互不冲突）
NONINTERACTIVE=1 arch -x86_64 /bin/bash -c \
  "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 将 x86_64 Homebrew 添加到 PATH
export PATH="/usr/local/bin:$PATH"
export HOMEBREW_NO_PATH_SHADOW_CHECK=1
```

#### 5.2 安装依赖并编译

```bash
# 安装 x86_64 依赖（使用 --overwrite 解决符号链接冲突）
brew install --overwrite cmake ninja gettext glib libusb zlib boost fftw \
  python3 qt pkg-config libzip nettle libftdi sdcc

# 编译 fx2lafw 固件
bash build_fx2lafw.sh

# 配置并编译 — 必须指定 CMAKE_OSX_ARCHITECTURES=x86_64
#（系统 clang 默认编译 ARM64，但 x86_64 Homebrew 库是 x86_64 的）
mkdir build && cd build
export LDFLAGS="-L$(brew --prefix gettext)/lib"
export CPPFLAGS="-I$(brew --prefix gettext)/include"
export PKG_CONFIG_PATH="$(brew --prefix gettext)/lib/pkgconfig:$PKG_CONFIG_PATH"

cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
  -DCMAKE_INSTALL_PREFIX=../install.dir

ninja
ninja install
```

然后按照步骤 4.1–4.5 创建 DMG。测试运行时使用 `arch -x86_64`：

```bash
arch -x86_64 install.dir/PXView.app/Contents/MacOS/PXView --headless
```

### 步骤 6：编译 Universal 二进制（macOS）

Universal 二进制在一个 `.app` 包中同时包含 ARM64 和 x86_64 两个架构的代码，允许一个 DMG 在 Apple Silicon 和 Intel Mac 上原生运行。

#### 6.1 前提条件

需要先分别构建 ARM64 和 x86_64 两个 DMG（步骤 4 和步骤 5）。提取两个 `.app` 包：

```bash
mkdir -p arm64 x86_64 universal

# 提取 ARM64 app
hdiutil attach arm64/PXView.dmg -nobrowse -mountpoint /tmp/arm64-mount
cp -R /tmp/arm64-mount/PXView.app arm64/PXView.app
hdiutil detach /tmp/arm64-mount

# 提取 x86_64 app
hdiutil attach x86_64/PXView.dmg -nobrowse -mountpoint /tmp/x86_64-mount
cp -R /tmp/x86_64-mount/PXView.app x86_64/PXView.app
hdiutil detach /tmp/x86_64-mount
```

#### 6.2 用 lipo 合并

```bash
# 以 ARM64 app 为基础复制
cp -R arm64/PXView.app universal/PXView.app

# 遍历所有文件，对双方都是 Mach-O 的二进制用 lipo 合并
find x86_64/PXView.app -type f | while read -r x86_file; do
    rel="${x86_file#x86_64/PXView.app/}"
    arm_file="arm64/PXView.app/$rel"
    uni_file="universal/PXView.app/$rel"

    [ -f "$arm_file" ] || continue

    if file "$x86_file" | grep -q "Mach-O" && file "$arm_file" | grep -q "Mach-O"; then
        lipo -create "$arm_file" "$x86_file" -output "$uni_file" \
          || echo "警告: 合并 $rel 失败，保留 ARM64 版本"
    fi
done

# 验证
file universal/PXView.app/Contents/MacOS/PXView
lipo -info universal/PXView.app/Contents/MacOS/PXView
```

#### 6.3 重新签名并创建 Universal DMG

```bash
xattr -cr universal/PXView.app
codesign --force --deep --sign - universal/PXView.app
codesign --verify --deep --strict universal/PXView.app

hdiutil create -volname PXView -srcfolder universal/PXView.app \
  -ov -format UDZO universal/PXView.dmg
```

---

## 可选功能

### 构建 MCP Web 客户端（可选）

MCP Web 客户端提供基于浏览器的聊天界面，用于自然语言控制设备。需要安装 `npm`。

```bash
# 构建 Web 客户端：
ninja webui

# 重新运行 install 以复制文件：
sudo ninja install    # Linux
# ninja install       # macOS（本地前缀，无需 sudo）

# 或一步完成构建+复制：
ninja install-webui
```

Web 客户端文件将安装到 `<prefix>/bin/webui/`，由 MCP 服务器在 `http://127.0.0.1:10110/` 上提供服务。

### 编译 fx2lafw 固件（可选）

fx2lafw 固件用于基于 Cypress FX2 USB 芯片的逻辑分析仪（Saleae Logic、CWAV USBee、Cypress FX2 等）。如果已安装 `sdcc`，CMake 会在 `ninja install` 阶段自动编译并安装 15 个 `.fw` 固件文件。也可以手动编译：

```bash
bash build_fx2lafw.sh
```

如果未安装 `sdcc`，CMake 会静默跳过固件安装。使用 FX2 芯片的设备将无法识别。
