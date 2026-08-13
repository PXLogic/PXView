# PXView Agent Desktop (Tauri v2)

将 `web/` 中的 AI Agent 网页打包为桌面端应用，同时以 headless 模式自动启动 PXView 后端。

## 架构概览

```
┌──────────────────────────────────────────┐
│         Tauri Desktop Window             │
│  ┌────────────────────────────────────┐  │
│  │     React + Vite + TailwindCSS     │  │
│  │     (web/ 前端，不变)               │  │
│  │                                    │  │
│  │  fetch ──► HTTP JSON-RPC           │  │
│  └──────────────┬─────────────────────┘  │
│                 │ 127.0.0.1:10110         │
│  ┌──────────────▼─────────────────────┐  │
│  │  Rust 后端 (src-tauri/)            │  │
│  │  • 启动时 spawn PXView.exe         │  │
│  │    --headless --port 10110         │  │
│  │  • 退出时 kill 子进程               │  │
│  │  • expose: pxview_status/restart   │  │
│  └──────────────┬─────────────────────┘  │
└─────────────────┼────────────────────────┘
                  │
┌─────────────────▼────────────────────────┐
│  PXView.exe --headless                   │
│  • MCP Server  (HTTP JSON-RPC, :10110)   │
│  • WebSocket Server (:10430)             │
│  • 采集 / 解码 / 设备管理                  │
│  • 无 GUI (QCoreApplication)             │
└──────────────────────────────────────────┘
```

**关键点：**
- PXView 已内置 `--headless` 模式（见 `PXView/main.cpp`），无需 GUI 即可运行 MCP + WS API
- 前端代码 **零改动** 即可工作——它已经通过 `http://127.0.0.1:10110` 连接 MCP 服务
- Tauri Rust 后端负责在启动时 spawn PXView.exe，在退出时 kill 它

## 前置条件

| 依赖 | 版本 | 安装方式 |
|------|------|----------|
| Node.js | ≥ 18 | https://nodejs.org |
| Rust | ≥ 1.77 | https://rustup.rs |
| PXView.exe | 已编译 | `cd build.dir && cmake --build .` 或运行 `build_windows.sh` |

### 安装 Rust (Windows)

```powershell
# 下载并运行 rustup-init
Invoke-WebRequest -Uri https://win.rustup.rs/x86_64 -OutFile rustup-init.exe
.\rustup-init.exe -y
# 重启终端后验证
rustc --version
cargo --version
```

## 开发模式

```bash
# 1. 确保 PXView.exe 已编译
#    项目根目录下应有 build.dir/PXView.exe

# 2. 进入 web 目录
cd web

# 3. 安装前端依赖（首次）
npm install

# 4. 启动 Tauri 开发模式
#    这会同时启动:
#    - Vite dev server (localhost:3000)
#    - Rust 编译 + Tauri 窗口
#    - PXView.exe --headless (自动 spawn)
npm run tauri:dev
```

首次运行会编译 Rust 依赖，可能需要几分钟。后续增量编译很快。

## 生产构建

```bash
cd web
npm install
npm run tauri:build
```

产物位于 `web/src-tauri/target/release/`：
- `PXView Agent.exe` — 独立可执行文件
- `bundle/` — 安装包（NSIS `.exe` / `.msi`）

## 打包分发

生产构建时需要将 PXView.exe 及其依赖 DLL 打包到 Tauri 产物中。

### 方法 1: 复制到产物目录（推荐）

```bash
# 1. 先构建 Tauri
cd web && npm run tauri:build

# 2. 将 PXView 运行时复制到产物目录
#    PXView 需要的文件包括:
#    - PXView.exe
#    - Qt6*.dll, libsigrok.a, libusb-1.0.dll 等
#    - res/ (固件), lang/ (语言), build.dir/decoders/ (解码器)
xcopy /E /I ..\build.dir\* src-tauri\target\release\pxview\
copy ..\build.dir\PXView.exe src-tauri\target\release\PXView.exe
```

### 方法 2: 配置 tauri.conf.json resources

编辑 `web/src-tauri/tauri.conf.json`，在 `bundle.resources` 中添加需要打包的文件：

```json
"resources": {
  "../../build.dir/PXView.exe": "PXView.exe",
  "../../build.dir/Qt6Core.dll": "Qt6Core.dll"
  // ... 其他 DLL
}
```

然后重新 `npm run tauri:build`。

## PXView.exe 查找顺序

Tauri 启动时按以下顺序查找 PXView.exe：

1. **当前可执行文件同目录** — `./PXView.exe`（生产模式）
2. **子目录 `pxview/`** — `./pxview/PXView.exe`（生产模式，打包在子目录）
3. **开发构建目录** — `../../../../build.dir/PXView.exe`（开发模式，相对于 `src-tauri/target/debug/`）
4. **工作目录相对路径** — `../../build.dir/PXView.exe`（开发模式备选）
5. **系统 PATH** — `where PXView.exe`（全局安装）

如果都找不到，Tauri 窗口仍会打开，但 MCP 连接会失败。可在设置面板中手动启动 PXView。

## Tauri Commands (前端可调用)

| 命令 | 说明 | 参数 |
|------|------|------|
| `pxview_status` | 查询 PXView headless 进程状态 | 无 |
| `pxview_restart` | 重启 PXView headless 进程 | 无 |

前端通过 `@tauri-apps/api/core` 的 `invoke()` 调用，封装在 `src/lib/tauri-bridge.ts` 中。

## 文件结构

```
web/
├── src-tauri/              # Tauri v2 Rust 项目
│   ├── Cargo.toml          # Rust 依赖
│   ├── tauri.conf.json     # Tauri 配置
│   ├── build.rs            # Tauri 构建脚本
│   ├── .gitignore
│   ├── icons/
│   │   ├── icon.ico        # Windows 图标
│   │   └── icon.png        # 通用图标
│   └── src/
│       ├── main.rs         # 入口
│       └── lib.rs          # App builder + PXView spawn/kill 逻辑
├── src/
│   ├── lib/
│   │   └── tauri-bridge.ts # 前端 Tauri API 封装
│   └── components/
│       ├── TopBar.tsx      # 桌面模式标识
│       └── SettingsDrawer.tsx # 后端管理面板
├── package.json            # 含 tauri 脚本
└── vite.config.ts          # 适配 Tauri 的 Vite 配置
```

## 纯浏览器模式 (不使用 Tauri)

如果不用 Tauri，仍可直接 `npm run dev` 在浏览器中开发。此时需手动启动 PXView：

```bash
# 终端 1: 启动 PXView headless
./build.dir/PXView.exe --headless --port 10110 --ws-port 10430

# 终端 2: 启动前端
cd web && npm run dev
# 打开 http://localhost:3000
```

`tauri-bridge.ts` 中的 `isTauri()` 会返回 `false`，所有 Tauri 相关 UI 元素自动隐藏。
