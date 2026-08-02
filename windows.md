# Install MSYS2

Use chololatey,

```
choco install msys2
```

Or download from <https://www.msys2.org/>

# Install dependencies

```
pacman -S mingw-w64-x86_64-pkg-config mingw-w64-x86_64-libusb mingw-w64-x86_64-toolchain mingw-w64-x86_64-boost mingw-w64-x86_64-python mingw-w64-x86_64-cmake mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-svg mingw-w64-x86_64-qt6-websockets mingw-w64-x86_64-glib2 mingw-w64-x86_64-fftw mingw-w64-x86_64-zlib mingw-w64-x86_64-nlohmann-json mingw-w64-x86_64-libzip liblzma liblzma-devel mingw-w64-x86_64-lcms2 curl unzip
```

# Optional dependencies

These are NOT required for PXLogic hardware. Only install if you need the specific drivers:

- `mingw-w64-x86_64-libftdi` — FTDI-based drivers (asix-sigma, chronovu-la, ftdi-la, ikalogic-scanaplus, pipistrello-ols). Without it, a warning is printed and those drivers are disabled; the rest of libsigrok builds normally.
- `mingw-w64-x86_64-nettle` — rdtech-tc driver (AES-256 firmware decryption). Without it, that single driver is disabled.

```
pacman -S mingw-w64-x86_64-libftdi mingw-w64-x86_64-nettle
```

# Download python embed with your python version

```
pver=$(python -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}')")
curl -L https://www.python.org/ftp/python/$pver/python-$pver-embed-amd64.zip -o python-embed.zip
mkdir python
unzip python-embed.zip -d python
```

\#git clone -b windows-hotplug --single-branch <https://github.com/sonatique/libusb.git>
\#build windows-hotplug libusb

# Build

```
mkdir build
mkdir install.dir
cd build
cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DCMAKE_INSTALL_PREFIX=../install.dir -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja
ninja install
```

# Create Package

```
cd ..
window/package.sh
```

