# INSTALL

## Requirements

- git
- gcc (>= 9.0) or clang
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
- nlohmann-json >= 3.2.0 (header-only; auto-downloaded from GitHub if not installed)
- sdcc >= 4.0 (optional; for building fx2lafw firmware for Cypress FX2 based logic analyzers)

### Optional dependencies (NOT required for PXLogic hardware)

These are NOT required for PXLogic hardware. Only install if you need the specific legacy drivers:

- **libftdi1** — FTDI-based drivers (asix-sigma, chronovu-la, ftdi-la, ikalogic-scanaplus, pipistrello-ols). Without it, a warning is printed and those drivers are disabled; the rest of libsigrok builds normally.
- **nettle** — rdtech-tc driver (AES-256 firmware decryption). Without it, that single driver is disabled.

---

## Linux

### Step 1: Installing the requirements

#### Ubuntu / Debian (e.g. Ubuntu 22.04 / 24.04):

```bash
sudo apt update
sudo apt install git gcc g++ make cmake ninja-build libglib2.0-dev zlib1g-dev \
  libusb-1.0-0-dev libboost-dev libfftw3-dev libzip-dev python3-dev libudev-dev \
  pkg-config libgl1-mesa-dev libxkbcommon-dev libvulkan-dev python3-pip sdcc
```

**Installing Qt 6.11 on Ubuntu:**
The default apt repository may not provide Qt 6.11, so you must install it manually using `aqtinstall`.

*Note on caching*: `aqtinstall` ("Another Qt Installer") deletes downloaded archives immediately after extraction to save space, as it was designed for CI/CD environments. To avoid re-downloading Qt every time you clean or move your project, it is highly recommended to install it to a global user directory.

```bash
pip3 install aqtinstall
# Install Qt 6.11 globally to ~/Qt (only needs to be run once!)
aqt install-qt linux desktop 6.11.0 linux_gcc_64 --outputdir ~/Qt
```

#### Fedora:

```bash
sudo dnf install git gcc gcc-c++ make cmake ninja-build libtool pkgconf \
  glib2-devel zlib-devel libudev-devel libusb1-devel python3-devel boost-devel \
  fftw-devel libzip-devel qt6-qtbase-devel qt6-qtsvg-devel qt6-qtwebsockets-devel
```
*(Fedora typically provides recent Qt6 versions in its standard repositories)*

#### Arch Linux:

```bash
sudo pacman -S base-devel git cmake ninja glib2 zlib libusb python boost \
  qt6-base qt6-svg qt6-websockets fftw libzip sdcc
```

#### Optional dependencies:

```bash
# Ubuntu / Debian:
sudo apt install libftdi1-dev libnettle-dev

# Fedora:
sudo dnf install libftdi-devel nettle-devel

# Arch Linux:
sudo pacman -S libftdi nettle
```

### Step 2: Get the PXView source code

```bash
git clone https://github.com/PXLogic/PXView
cd PXView
```

### Step 3: Building

If you installed Qt manually via `aqtinstall` (e.g. on Ubuntu), you must tell CMake where to find it. Otherwise, if you used system packages (Arch/Fedora), you can omit the `CMAKE_PREFIX_PATH` flag.

```bash
mkdir build && cd build

# For Ubuntu with aqtinstall (using the global ~/Qt path):
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"

# For Arch / Fedora (System Qt):
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

### Step 4 (recommended): Packaging as a .sh Installer (Linux)

PXView now ships a self-extracting `.run` installer built by `packaging/make-installer.sh`. It stages `install.dir/usr` into a portable payload and installs it to `/opt/PXView` with full system integration (udev rules, icons, two desktop entries, CLI wrappers, uninstaller).

```bash
cd build && ninja -j "$(nproc)" && ninja install
cd ..

# Bundle runtime libs (Qt, plugins, Python stdlib, whitelisted system deps) and
# write bin/qt.conf. Does NOT bundle WebKitGTK/GTK/GStreamer. See
# packaging/bundle-runtime-libs.sh.
bash packaging/bundle-runtime-libs.sh install.dir

PXVIEW_VERSION=1.5.9 bash packaging/make-installer.sh
# -> PXView-Linux-x86_64-Installer-1.5.9.run
sudo ./PXView-Linux-x86_64-Installer-1.5.9.run
```

The Tauri Agent (`PXView-Agent`) is shipped as a plain file and resolves `libwebkit2gtk-4.1` from the target system at runtime — it is deliberately **not** bundled, because bundling WebKitGTK produces an ABI mismatch (bundled libs vs. the system's `WebKitWebProcess`/`WebKitNetworkProcess`) that blanks the Agent window. `packaging/check-webkit-stack.sh` fails the build if any desktop/WebKit library leaks into the tree.

> **Note:** The AppImage section below is retained only as historical reference.

### Step 4 (legacy reference): Packaging as AppImage (Linux)

AppImage bundles the application and its dependencies into a single portable file. Since AppImage is a user-space portable package, **system-level files such as udev rules, desktop entries, and documentation should not be bundled inside** — they must be installed separately.

#### 4.1 Build with a local install prefix

When packaging an AppImage, use a local prefix (e.g. `../install.dir`) instead of a system prefix (`/usr`). This ensures that udev rules and other system files are installed under the local directory rather than system paths, avoiding the need for root privileges:

```bash
mkdir build && cd build

# Use a local prefix — udev rules etc. will be installed under install.dir
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../install.dir/usr \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.0/gcc_64"
ninja
ninja install
```

> **Note**: linuxdeploy expects AppDir to contain `usr/{bin,share,lib}`, so the install prefix is set to `../install.dir/usr` (not `../install.dir`).

#### 4.2 Bundle Python standard library (required for protocol decoders)

libsigrokdecode embeds a Python interpreter that requires the Python standard library (`encodings` and other modules). Without bundling it, users whose system Python version differs from the build machine's version (e.g. build with 3.10, user has 3.12) will get `ModuleNotFoundError: No module named 'encodings'` on startup.

```bash
PY_VERSION=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
echo "Bundling Python $PY_VERSION standard library"

# Copy standard library .py files
mkdir -p install.dir/usr/lib/python$PY_VERSION
cp -a /usr/lib/python$PY_VERSION/* install.dir/usr/lib/python$PY_VERSION/ 2>/dev/null || true

# Copy lib-dynload (.so files for built-in modules like encodings)
mkdir -p install.dir/usr/lib/python$PY_VERSION/lib-dynload
cp -a /usr/lib/python$PY_VERSION/lib-dynload/* install.dir/usr/lib/python$PY_VERSION/lib-dynload/ 2>/dev/null || true

# Remove unnecessary test suite and __pycache__ to reduce size
rm -rf install.dir/usr/lib/python$PY_VERSION/test
find install.dir/usr/lib/python$PY_VERSION -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null || true

# Verify encodings module is present
if [ -d install.dir/usr/lib/python$PY_VERSION/encodings ]; then
  echo "OK: encodings module found"
else
  echo "WARNING: encodings not found, AppImage Python may fail"
fi
```

#### 4.3 Build qtwayland platform plugin (optional, for Wayland support)

`aqtinstall` does not provide the qtwayland client platform plugin. If you want the AppImage to support both X11 (xcb) and Wayland, build qtwayland from source:

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

#### 4.4 Build the AppImage

```bash
# Go back to the project root directory
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

#### 4.5 Install system-level files (outside AppImage)

The AppImage does not include the following system-level files. Users must install them manually once:

**udev rules (hardware access permissions):**
```bash
sudo cp install.dir/usr/lib/udev/rules.d/60-px.rules /etc/udev/rules.d/60-px.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

**Desktop entry (application menu integration):**
```bash
sudo cp install.dir/usr/share/applications/pxview.desktop /usr/share/applications/pxview.desktop
```

**Documentation and resources (optional):**
```bash
# Documentation and user manuals are already included inside the AppImage at share/PXView/
# For system-wide installation:
sudo cp -r install.dir/usr/share/PXView /usr/share/PXView
```

> **Tip**: You can also write an `install.sh` script to distribute alongside the AppImage, automating the installation of the system-level files above.

---

## macOS

### Step 1: Installing the requirements (Homebrew)

```bash
brew install git cmake ninja gettext glib libusb zlib boost fftw python3 qt pkg-config libzip sdcc
```

*(Note: If the default `qt` brew formula is not 6.11.0 yet, or if it isn't automatically linked, you may need to find the brew Qt installation path, typically `/opt/homebrew/opt/qt`)*

Optional dependencies:

```bash
brew install libftdi nettle
```

### Step 2: Get the PXView source code

```bash
git clone https://github.com/PXLogic/PXView
cd PXView
```

### Step 3: Building

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

### Step 4: Creating DMG

DMG packaging on macOS uses `macdeployqt` to collect all dependencies into the `.app` bundle, then `codesign` for ad-hoc signing, and finally `hdiutil` to create the disk image.

#### 4.1 Fix Python framework path

Homebrew's `python@3.x` places the Python framework at `<prefix>/Frameworks/Python.framework`, but `macdeployqt` expects it at `<prefix>/lib/Python.framework`. Create a symlink to fix this:

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

#### 4.2 Run macdeployqt

```bash
MACDEPLOYQT="$(brew --prefix qt)/bin/macdeployqt"

$MACDEPLOYQT install.dir/PXView.app \
  -always-overwrite -verbose=1 \
  || true   # macdeployqt returns non-zero for missing rpath, but copies most deps
```

#### 4.3 Copy missing transitive dependencies

`macdeployqt` may fail to copy three transitive dependencies of Qt WebEngine/Pdf (even though PXView does not use them directly). Copy them manually from Homebrew:

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

#### 4.4 Copy Python standard library into bundled Python.framework

`macdeployqt` only copies the framework's Mach-O binary (the Python shared library), but NOT the `lib/python3.X/` directory (which contains `encodings`, `os.py`, and other pure Python modules). Homebrew's stdlib lives inside the Python.framework, not in a separate directory. Without copying it, the app will crash with `Fatal Python error: Failed to import encodings module` on machines that don't have the same Homebrew Python installed.

```bash
BUNDLED_FW="install.dir/PXView.app/Contents/Frameworks/Python.framework"
if [ -d "$BUNDLED_FW" ]; then
  for ver_dir in "$BUNDLED_FW"/Versions/3.*; do
    [ -d "$ver_dir" ] || continue
    py_ver=$(basename "$ver_dir")

    # Search for the stdlib in various Homebrew locations
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
    # Also check brew prefix/lib (non-framework installation)
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
      echo "Copied Python stdlib (python$py_ver) to bundled framework"
      if [ -d "$ver_dir/lib/python$py_ver/encodings" ]; then
        echo "Verified: encodings module present"
      else
        echo "WARNING: encodings module missing after copy"
      fi
    else
      echo "WARNING: Python stdlib not found for $py_ver"
    fi
  done

  # Delete broken symlinks introduced by copying stdlib (e.g. site-packages
  # pointing to Homebrew paths). Broken symlinks cause xattr -cr and
  # codesign --verify to fail.
  find "$BUNDLED_FW" -type l ! -exec test -e {} \; -delete 2>/dev/null || true

  # Create Current symlink if missing (macdeployqt may not copy it)
  if [ ! -e "$BUNDLED_FW/Versions/Current" ]; then
    latest_ver=$(ls -d "$BUNDLED_FW"/Versions/3.* 2>/dev/null | sort -V | tail -1)
    if [ -n "$latest_ver" ]; then
      ln -s "$(basename "$latest_ver")" "$BUNDLED_FW/Versions/Current"
      echo "Created Current symlink -> $(basename "$latest_ver")"
    fi
  fi
else
  echo "WARNING: Python.framework not found in bundle"
fi
```

#### 4.5 Codesign and create DMG

```bash
# Remove quarantine attributes from Homebrew libraries
# (|| true: tolerate any remaining broken symlinks)
xattr -cr install.dir/PXView.app 2>/dev/null || true

# Ad-hoc sign the entire .app bundle recursively
# --deep is required: framework resource seals must be established by
# signing the framework directory, not individual binaries inside.
codesign --force --deep --sign - install.dir/PXView.app

# Verify signature
codesign --verify --deep --strict install.dir/PXView.app

# Create DMG
hdiutil create -volname PXView -srcfolder install.dir/PXView.app \
  -ov -format UDZO install.dir/PXView.dmg
```

### Step 5: Building x86_64 on ARM64 Macs (Rosetta 2 cross-compilation)

Apple Silicon Macs (M1/M2/M3) can build x86_64 binaries using Rosetta 2 and the x86_64 Homebrew. This produces a native x86_64 DMG without needing an Intel Mac.

#### 5.1 Install Rosetta 2 and x86_64 Homebrew

```bash
# Install Rosetta 2
softwareupdate --install-rosetta --agree-to-license

# Install x86_64 Homebrew to /usr/local
# (ARM64 Homebrew is at /opt/homebrew, x86_64 Homebrew at /usr/local — they coexist)
NONINTERACTIVE=1 arch -x86_64 /bin/bash -c \
  "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Add x86_64 Homebrew to PATH
export PATH="/usr/local/bin:$PATH"
export HOMEBREW_NO_PATH_SHADOW_CHECK=1
```

#### 5.2 Install dependencies and build

```bash
# Install x86_64 dependencies (use --overwrite to resolve symlink conflicts)
brew install --overwrite cmake ninja gettext glib libusb zlib boost fftw \
  python3 qt pkg-config libzip nettle libftdi sdcc

# Build fx2lafw firmware
bash build_fx2lafw.sh

# Configure and build — CMAKE_OSX_ARCHITECTURES=x86_64 is required
# (system clang defaults to ARM64, but x86_64 Homebrew libs are x86_64)
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

Then follow Steps 4.1–4.5 to create the DMG. When running the binary for testing, use `arch -x86_64`:

```bash
arch -x86_64 install.dir/PXView.app/Contents/MacOS/PXView --headless
```

### Step 6: Building Universal Binary (macOS)

A Universal binary contains both ARM64 and x86_64 slices in a single `.app` bundle, allowing one DMG to run natively on both Apple Silicon and Intel Macs.

#### 6.1 Prerequisites

You need both ARM64 and x86_64 DMGs built (Steps 4 and 5). Extract both `.app` bundles:

```bash
mkdir -p arm64 x86_64 universal

# Extract ARM64 app
hdiutil attach arm64/PXView.dmg -nobrowse -mountpoint /tmp/arm64-mount
cp -R /tmp/arm64-mount/PXView.app arm64/PXView.app
hdiutil detach /tmp/arm64-mount

# Extract x86_64 app
hdiutil attach x86_64/PXView.dmg -nobrowse -mountpoint /tmp/x86_64-mount
cp -R /tmp/x86_64-mount/PXView.app x86_64/PXView.app
hdiutil detach /tmp/x86_64-mount
```

#### 6.2 Merge with lipo

```bash
# Start from the ARM64 app as base
cp -R arm64/PXView.app universal/PXView.app

# Merge all Mach-O binaries (main executable, dylibs, frameworks)
find x86_64/PXView.app -type f | while read -r x86_file; do
    rel="${x86_file#x86_64/PXView.app/}"
    arm_file="arm64/PXView.app/$rel"
    uni_file="universal/PXView.app/$rel"

    [ -f "$arm_file" ] || continue

    if file "$x86_file" | grep -q "Mach-O" && file "$arm_file" | grep -q "Mach-O"; then
        lipo -create "$arm_file" "$x86_file" -output "$uni_file" \
          || echo "WARN: Failed to combine $rel, keeping ARM64 version"
    fi
done

# Verify
file universal/PXView.app/Contents/MacOS/PXView
lipo -info universal/PXView.app/Contents/MacOS/PXView
```

#### 6.3 Re-sign and create Universal DMG

```bash
xattr -cr universal/PXView.app
codesign --force --deep --sign - universal/PXView.app
codesign --verify --deep --strict universal/PXView.app

hdiutil create -volname PXView -srcfolder universal/PXView.app \
  -ov -format UDZO universal/PXView.dmg
```

---

## Optional Features

### Building the MCP web client (optional)

The MCP web client provides a browser-based chat interface for controlling devices with natural language. It requires `npm` to be installed.

```bash
# Build the web client:
ninja webui

# Then re-run install to copy it:
sudo ninja install    # Linux
# ninja install       # macOS (local prefix, no sudo needed)

# Or build + copy in one step:
ninja install-webui
```

The web client files will be installed to `<prefix>/bin/webui/` and served by the MCP server at `http://127.0.0.1:10110/`.

### Building fx2lafw firmware (optional)

The fx2lafw firmware is required for Cypress FX2 USB based logic analyzers (Saleae Logic, CWAV USBee, Cypress FX2, etc.). If `sdcc` is installed, CMake will automatically build and install 15 `.fw` firmware files during `ninja install`. You can also build them manually:

```bash
bash build_fx2lafw.sh
```

If `sdcc` is not installed, CMake will silently skip firmware installation. Devices using FX2 chips will not be recognized.
