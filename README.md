![PXLogic](PXView/icons/logo.svg)

# PXView

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Version](https://img.shields.io/badge/version-1.5.6-green.svg)](#)

PXView is a Qt-based signal analysis software for [PXLogic](https://marrychip.com) logic analyzers and a wide range of third-party instruments. It is built upon the [sigrok](https://sigrok.org) project and [DSView](https://www.dreamsourcelab.com), providing a comprehensive GUI for signal capture, protocol decoding, and automated testing.

## Key Features

- **Multi-mode signal acquisition** — Logic analyzer, oscilloscope (DSO), and mixed-signal oscilloscope (MSO) modes with simultaneous digital and analog channel display
- **220+ protocol decoders** — I²C, SPI, UART, CAN, PWM, USB, JTAG, I²S, Modbus, IR (NEC/RC5/RC6/SIRC), 1-Wire, SD card, and many more, with both Python and high-performance C implementations
- **187+ hardware drivers** — Native support for PXLogic devices plus broad compatibility with Saleae Logic, ChronoVu LA, ASIX Sigma, FTDI-based analyzers, Hantek DSO, Rigol DS, and many other instruments via libsigrok
- **MCP (Model Context Protocol) server** — Built-in HTTP API server (port 10110) enabling AI tools like Claude Code, Codex, and OpenCode to control devices, capture signals, and decode protocols through natural language
- **Headless mode** — Run `PXView --headless` for automated testing and CI/CD pipelines without a GUI, with full API access via MCP and WebSocket (port 10430)
- **USB hotplug** — Cross-platform hotplug support with automatic device detection and reconnection
- **Glitch filter** — Pulse analysis tool with histogram visualization, threshold adjustment, and undo support
- **Data export** — Export raw capture data and decoded results to CSV, binary, or session files (`.pxc`)
- **Cross-platform** — Linux (AppImage), macOS (Universal DMG for ARM64 + x86_64), and Windows

## PXLogic Hardware

PXView provides native support for PXLogic USB logic analyzers:

| Model | Channels | Max Sample Rate | Interface |
|-------|----------|-----------------|-----------|
| PX-Logic 32 | 32 ch | 1 GSa/s (buffer), 50 MSa/s (stream) | USB 3.0 / 2.0 |
| PX-Logic 16 Pro | 16 ch | 500 MSa/s (buffer), 125 MSa/s (stream) | USB 3.0 / 2.0 |
| PX-Logic 16 Plus | 16 ch | 500 MSa/s (buffer), 125 MSa/s (stream) | USB 3.0 / 2.0 |
| PX-Logic 16 Base | 16 ch | 250 MSa/s (buffer), 125 MSa/s (stream) | USB 3.0 / 2.0 |

## Installation

See the detailed build and installation guides:

- [INSTALL.md](INSTALL.md) — English
- [INSTALL_zh.md](INSTALL_zh.md) — 中文

Pre-built packages are available for download from the [releases page](https://github.com/PXLogic/PXView/releases).

## Source Code (Git Submodules)

PXView references the sigrok project's official upstream repositories via git submodules, ensuring complete open-source repository paths and git history are included in source distribution. Clone with `--recursive`:

```bash
git clone --recursive <PXView-repo-url> PXView
cd PXView
```

If already cloned without submodules, initialize them manually:

```bash
git submodule update --init --recursive
```

Included submodules (located at the repository root, alongside `libsigrok/`):

| Submodule | Upstream Repository | Purpose |
|-----------|-------------------|---------|
| `libsigrok/` | `https://github.com/sigrokproject/libsigrok` (pxview-fork branch) | Upstream libsigrok 0.6.0 (187+ drivers) + PXLogic/sipeed-slogic drivers + sr_compat layer + CMake build |
| `libsigrokdecode/` | `https://github.com/sigrokproject/libsigrokdecode` (pxview-fork branch) | Upstream libsigrokdecode + C decoder subsystem (215+ C decoders + API framework, API v4) |
| `sigrok-firmware/` | `git://sigrok.org/sigrok-firmware` | Redistributable firmware for ASIX Sigma + Sysclk LWLA (vendor-licensed) |
| `sigrok-firmware-fx2lafw/` | `git://sigrok.org/sigrok-firmware-fx2lafw` | Open-source Cypress FX2 firmware source (GPLv2+, requires sdcc to compile) |
| `sigrok-util/` | `git://sigrok.org/sigrok-util` | Vendor firmware extraction scripts (`firmware/` subdirectory) |

### Building fx2lafw Firmware

The `sigrok-firmware-fx2lafw/` submodule contains source code but no pre-compiled `.fw` files. Build them on-site using the sdcc compiler:

```bash
# Install sdcc (example: Ubuntu)
sudo apt install sdcc

# From the repository root
bash build_fx2lafw.sh
```

This produces 15 `fx2lafw-*.fw` files under `sigrok-firmware-fx2lafw/hw/*/`. The `ninja install` step automatically detects and packages them into `share/sigrok-firmware/`.

If fx2lafw is not built, CMake will emit a WARNING and only the 8 ASIX Sigma + Sysclk LWLA firmware files will be packaged (FX2-based devices will not be recognized).

## MCP API and AI Integration

PXView includes a built-in MCP server that allows AI programming tools to control hardware devices through a standardized protocol. The server starts automatically on port 10110.

### Connecting AI Tools

```bash
# Claude Code
claude mcp add --transport http pxview http://127.0.0.1:10110

# Codex
codex mcp add --url http://127.0.0.1:10110 pxview

# OpenCode
opencode --mcp http://127.0.0.1:10110
```

### Available MCP Tools

| Category | Tools |
|----------|-------|
| Device management | `get_devices`, `get_channels` |
| Capture control | `start_capture`, `stop_capture`, `wait_capture`, `get_capture_status` |
| Protocol decoding | `list_analyzers`, `get_analyzer_options`, `add_analyzer`, `remove_analyzer`, `get_analyzer_results` |
| Data export | `export_raw_data_csv`, `export_raw_data_binary`, `export_data_table_csv` |
| File operations | `load_capture`, `save_capture`, `close_capture` |

See the [MCP usage guide](doc/MCP与Web客户端使用指南.md) for detailed documentation.

## Useful Links

- [PXLogic](https://marrychip.com) — Official website
- [sigrok.org](https://sigrok.org) — Upstream sigrok project
- [DreamSourceLab](https://www.dreamsourcelab.com) — DSView project

## Status

PXView is in a usable state with official releases. However, it is still a work in progress — some basic functionality is available and working, while other features remain on the TODO list.

## Copyright and License

PXView is licensed under the terms of the GNU General Public License (GPL), version 3 or later.

While some individual source code files are licensed under the GPLv2+, and some files are licensed under the GPLv3+, this doesn't change the fact that the program as a whole is licensed under the terms of the GPLv3+ (e.g. also due to the fact that it links against GPLv3+ libraries).

Please see the individual source files for the full list of copyright holders.
