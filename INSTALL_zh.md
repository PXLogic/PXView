# 安装指南

## 系统要求
- git
- gcc (>= 9.0) 或 clang
- g++
- make
- cmake >= 3.16
- ninja-build
- Qt >= 6.11.0 (Core, Gui, Widgets, Svg, Concurrent)
- libglib >= 2.32.0
- zlib
- libusb-1.0 >= 1.0.16
- libboost >= 1.42
- libfftw3 >= 3.3
- python >= 3.8
- pkg-config >= 0.22

## 编译与安装

### 步骤 1：安装依赖

#### Ubuntu / Debian（如 Ubuntu 22.04 / 24.04）：
```bash
sudo apt update
sudo apt install git gcc g++ make cmake ninja-build libglib2.0-dev zlib1g-dev libusb-1.0-0-dev libboost-dev libfftw3-dev python3-dev libudev-dev pkg-config libgl1-mesa-dev libxkbcommon-dev libvulkan-dev python3-pip
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
sudo dnf install git gcc gcc-c++ make cmake ninja-build libtool pkgconf glib2-devel zlib-devel libudev-devel libusb1-devel python3-devel boost-devel fftw-devel qt6-qtbase-devel qt6-qtsvg-devel
```
*（Fedora 通常在标准仓库中提供较新的 Qt6 版本）*

#### Arch Linux：
```bash
sudo pacman -S base-devel git cmake ninja glib2 zlib libusb python boost qt6-base qt6-svg fftw
```

#### macOS（Homebrew）：
```bash
brew install git cmake ninja gettext glib libusb zlib boost fftw python3 qt pkg-config
```
*（注意：如果默认的 `qt` brew formula 尚未更新到 6.11.0，或未自动链接，可能需要手动查找 brew Qt 安装路径，通常为 `/opt/homebrew/opt/qt`）*

### 步骤 2：获取 PXView 源代码
```bash
git clone https://github.com/PXLogic/PXView
cd PXView
```

### 步骤 3：编译

如果在步骤 1 中通过 `aqtinstall` 手动安装了 Qt（如在 Ubuntu 上），必须告诉 CMake Qt 的位置。如果使用系统包管理器安装（Arch/Fedora/macOS），可以省略 `CMAKE_PREFIX_PATH` 参数。

```bash
mkdir build && cd build

# Ubuntu 使用 aqtinstall（使用全局 ~/Qt 路径）：
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"

# Arch / Fedora / macOS（系统 Qt）：
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

### 步骤 3b：构建 MCP Web 客户端（可选）

MCP Web 客户端提供基于浏览器的聊天界面，用于自然语言控制设备。需要安装 `npm`。

```bash
# 构建 Web 客户端：
ninja webui

# 重新运行 install 以复制文件：
sudo ninja install

# 或一步完成构建+复制：
ninja install-webui
```

Web 客户端文件将安装到 `<prefix>/bin/webui/`，由 MCP 服务器在 `http://127.0.0.1:10110/` 上提供服务。

### 步骤 4：打包为 AppImage（Linux）

AppImage 将应用程序及其依赖打包为单个可移植文件。由于 AppImage 是用户态的便携包，**udev rules、desktop 文件、文档等系统级文件不应打包进去**，需要单独安装。

#### 4.1 使用本地安装前缀编译

打包 AppImage 时，应使用本地前缀（如 `../install.dir`）而非系统前缀（`/usr`），这样 udev rules 等系统文件会安装到本地目录而非系统目录，避免需要 root 权限：

```bash
mkdir build && cd build

# 使用本地前缀 — udev rules 等会安装到 install.dir 下
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install.dir -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"
ninja
ninja install
```

> **说明**：当 `CMAKE_INSTALL_PREFIX` 为 `/usr` 或 `/usr/local` 时，udev rules 会自动安装到系统路径（如 `/usr/lib/udev/rules.d`），需要 `sudo ninja install`。使用本地前缀时，所有文件都安装到本地目录，无需 root 权限。

#### 4.2 打包 AppImage

```bash
# 回到项目根目录
cd ..

wget -c "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -c "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x linuxdeploy*.AppImage

export QMAKE="$HOME/Qt/6.11.0/gcc_64/bin/qmake"
export LD_LIBRARY_PATH="$HOME/Qt/6.11.0/gcc_64/lib:$LD_LIBRARY_PATH"
export OUTPUT="PXView-x86_64.AppImage"

./linuxdeploy-x86_64.AppImage --appdir install.dir -e install.dir/bin/PXView -d install.dir/share/applications/pxview.desktop --plugin qt --output appimage
```

#### 4.3 安装系统级文件（AppImage 外）

AppImage 不包含以下系统级文件，用户需要手动安装一次：

**udev rules（硬件访问权限）：**
```bash
sudo cp install.dir/lib/udev/rules.d/60-px.rules /etc/udev/rules.d/60-px.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

**Desktop 文件（应用菜单集成）：**
```bash
sudo cp install.dir/share/applications/pxview.desktop /usr/share/applications/pxview.desktop
```

**文档和资源（可选）：**
```bash
# 文档、用户手册等已包含在 AppImage 内部的 share/PXView/ 中
# 如需系统级安装：
sudo cp -r install.dir/share/PXView /usr/share/PXView
```

> **提示**：也可以编写一个 `install.sh` 脚本随 AppImage 一起分发，自动完成上述系统级文件的安装。
