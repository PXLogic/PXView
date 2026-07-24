![PXLogic](PXView/icons/logo.svg)


# PXView 
- [PXLogic](https://marrychip.com)(https://marrychip.com)

PXView is a GUI program for supporting various instruments from [PXLogic](https://github.com/PXLogic/PXView), including logic analyzers, oscilloscopes, etc. PXView is based on the [sigrok project](https://sigrok.org).

The sigrok project aims at creating a portable, cross-platform, Free/Libre/Open-Source signal analysis software suite that supports various device types (such as logic analyzers, oscilloscopes, multimeters, and more).

# Status

The PXView software is in a usable state and has official tarball releases. However, it is still a work in progress. Some basic functionality is available and working, but other things are always on the TODO list.

# Useful links

- [sigrok.org](https://sigrok.org)
- [dreamsourcelab.com](https://www.dreamsourcelab.com)


# Source code (git submodules)

PXView 通过 git submodule 引用 sigrok 项目的官方上游仓库，确保源代码分发时包含完整的开源仓库路径与 git 历史。克隆时使用 `--recursive`：

```bash
git clone --recursive <PXView-repo-url> PXView
cd PXView
```

如果已经克隆但未带 submodule，手动初始化：

```bash
git submodule update --init --recursive
```

包含的 submodule（与 `libsigrok/` 同级，位于仓库根目录）：

| Submodule | 上游仓库 | 用途 |
|-----------|---------|------|
| `libsigrok/` | `https://github.com/sigrokproject/libsigrok` (pxview-fork 分支) | 上游 libsigrok 0.6.0 源码（81+ 驱动）+ PXLogic/sipeed-slogic 驱动 + sr_compat + CMake 构建 |
| `libsigrokdecode/` | `https://github.com/sigrokproject/libsigrokdecode` (pxview-fork 分支) | 上游 libsigrokdecode + C decoder 子系统（215 个 C 解码器 + API 框架，API v4） |
| `sigrok-firmware/` | `git://sigrok.org/sigrok-firmware` | asix-sigma + sysclk-lwla 可重分发固件（vendor 授权） |
| `sigrok-firmware-fx2lafw/` | `git://sigrok.org/sigrok-firmware-fx2lafw` | Cypress FX2 系列开源固件源码（GPLv2+，需用 sdcc 现场编译） |
| `sigrok-util/` | `git://sigrok.org/sigrok-util` | vendor 私有固件提取脚本（`firmware/` 子目录） |

## 构建 fx2lafw 固件

`sigrok-firmware-fx2lafw/` 含源码但不含预编译 .fw 文件。需用 sdcc 编译器现场构建：

```bash
# 安装 sdcc（MSYS2）
pacman -S mingw-w64-x86_64-sdcc

# 在仓库根目录执行
bash build_fx2lafw.sh
```

构建后会在 `sigrok-firmware-fx2lafw/hw/*/` 生成 15 个 `fx2lafw-*.fw` 文件。`ninja install` 会自动检测并打包到 `share/sigrok-firmware/`。

如未构建 fx2lafw，cmake 会输出 WARNING，install 只打包 asix-sigma + sysclk-lwla 共 8 个固件（FX2 设备将无法使用）。

## 更新 submodule（维护者）

```bash
# 更新单个 submodule 到上游最新
cd sigrok-firmware && git pull origin master && cd ..
git add sigrok-firmware
git commit -m "Update sigrok-firmware submodule"
```


# Copyright and license

PXView software is licensed under the terms of the GNU General Public License
(GPL), version 3 or later.

While some individual source code files are licensed under the GPLv2+, and
some files are licensed under the GPLv3+, this doesn't change the fact that
the program as a whole is licensed under the terms of the GPLv3+ (e.g. also
due to the fact that it links against GPLv3+ libraries).

Please see the individual source files for the full list of copyright holders.
