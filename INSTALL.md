# INSTALL

## Requirements
- git
- gcc (>= 9.0) or clang
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

## Building and installing

### Step 1: Installing the requirements

#### Ubuntu / Debian (e.g. Ubuntu 22.04 / 24.04):
```bash
sudo apt update
sudo apt install git gcc g++ make cmake ninja-build libglib2.0-dev zlib1g-dev libusb-1.0-0-dev libboost-dev libfftw3-dev python3-dev libudev-dev pkg-config libgl1-mesa-dev libxkbcommon-dev libvulkan-dev python3-pip
```

**How to install Qt 6.11 on Ubuntu:**
The default apt repository may not provide Qt 6.11, so you must install it manually using `aqtinstall`.
*Note on caching*: `aqtinstall` ("Another Qt Installer") deletes downloaded archives immediately after extraction to save space, as it was designed for CI/CD environments. To avoid re-downloading Qt every time you clean or move your project, it is highly recommended to install it to a global user directory.

```bash
pip3 install aqtinstall
# Install Qt 6.11 globally to ~/Qt (only needs to be run once!)
aqt install-qt linux desktop 6.11.0 linux_gcc_64 --outputdir ~/Qt
```

#### Fedora:
```bash
sudo dnf install git gcc gcc-c++ make cmake ninja-build libtool pkgconf glib2-devel zlib-devel libudev-devel libusb1-devel python3-devel boost-devel fftw-devel qt6-qtbase-devel qt6-qtsvg-devel
```
*(Fedora typically provides recent Qt6 versions in its standard repositories)*

#### Arch Linux:
```bash
sudo pacman -S base-devel git cmake ninja glib2 zlib libusb python boost qt6-base qt6-svg fftw
```

#### macOS (Homebrew):
```bash
brew install git cmake ninja gettext glib libusb zlib boost fftw python3 qt pkg-config
```
*(Note: If the default `qt` brew formula is not 6.11.0 yet, or if it isn't automatically linked, you may need to find the brew Qt installation path, typically `/opt/homebrew/opt/qt`)*

### Step 2: Get the PXView source code
```bash
git clone https://github.com/PXLogic/PXView
cd PXView
```

### Step 3: Building

If you installed Qt manually via `aqtinstall` (e.g. on Ubuntu) in Step 1, you must tell CMake where to find it. Otherwise, if you used system packages (Arch/Fedora/macOS), you can omit the `CMAKE_PREFIX_PATH` flag.

```bash
mkdir build && cd build

# For Ubuntu with aqtinstall (using the global ~/Qt path):
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"

# For Arch / Fedora / macOS (System Qt):
# cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr

ninja
sudo ninja install
```

When `CMAKE_INSTALL_PREFIX` is `/usr` or `/usr/local`, udev rules will be installed to the system path (e.g. `/usr/lib/udev/rules.d`), which requires `sudo`.

**Running the binary directly (without AppImage):** If Qt was installed via `aqtinstall` (not in a system library path), you must set `LD_LIBRARY_PATH` before running:

```bash
LD_LIBRARY_PATH="$HOME/Qt/6.11.0/gcc_64/lib" ./install.dir/bin/PXView
```

Or add it to your shell profile for convenience:

```bash
echo 'export LD_LIBRARY_PATH="$HOME/Qt/6.11.0/gcc_64/lib:$LD_LIBRARY_PATH"' >> ~/.bashrc
source ~/.bashrc
```

`ninja install` will automatically install the MCP web client if it has been built. If the web client has not been built yet, it will be silently skipped.

### Step 3b: Building the MCP web client (optional)

The MCP web client provides a browser-based chat interface for controlling devices with natural language. It requires `npm` to be installed.

```bash
# Build the web client:
ninja webui

# Then re-run install to copy it:
sudo ninja install

# Or build + copy in one step:
ninja install-webui
```

The web client files will be installed to `<prefix>/bin/webui/` and served by the MCP server at `http://127.0.0.1:10110/`.

### Step 4: Packaging as AppImage (Linux)

AppImage bundles the application and its dependencies into a single portable file. Since AppImage is a user-space portable package, **system-level files such as udev rules, desktop entries, and documentation should not be bundled inside** — they must be installed separately.

#### 4.1 Build with a local install prefix

When packaging an AppImage, use a local prefix (e.g. `../install.dir`) instead of a system prefix (`/usr`). This ensures that udev rules and other system files are installed under the local directory rather than system paths, avoiding the need for root privileges:

```bash
mkdir build && cd build

# Use a local prefix — udev rules etc. will be installed under install.dir
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../install.dir -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"
ninja
ninja install
```

> **Note**: When `CMAKE_INSTALL_PREFIX` is `/usr` or `/usr/local`, udev rules are automatically installed to system paths (e.g. `/usr/lib/udev/rules.d`), requiring `sudo ninja install`. With a local prefix, all files are installed locally and no root privileges are needed.

#### 4.2 Build the AppImage

```bash
# Go back to the project root directory
cd ..

wget -c "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -c "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage"
chmod +x linuxdeploy*.AppImage

export QMAKE="$HOME/Qt/6.11.0/gcc_64/bin/qmake"
export LD_LIBRARY_PATH="$HOME/Qt/6.11.0/gcc_64/lib:$LD_LIBRARY_PATH"
export OUTPUT="PXView-x86_64.AppImage"

./linuxdeploy-x86_64.AppImage --appdir install.dir -e install.dir/bin/PXView -d install.dir/share/applications/pxview.desktop --plugin qt --output appimage
```

#### 4.3 Install system-level files (outside AppImage)

The AppImage does not include the following system-level files. Users must install them manually once:

**udev rules (hardware access permissions):**
```bash
sudo cp install.dir/lib/udev/rules.d/60-px.rules /etc/udev/rules.d/60-px.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

**Desktop entry (application menu integration):**
```bash
sudo cp install.dir/share/applications/pxview.desktop /usr/share/applications/pxview.desktop
```

**Documentation and resources (optional):**
```bash
# Documentation and user manuals are already included inside the AppImage at share/PXView/
# For system-wide installation:
sudo cp -r install.dir/share/PXView /usr/share/PXView
```

> **Tip**: You can also write an `install.sh` script to distribute alongside the AppImage, automating the installation of the system-level files above.
