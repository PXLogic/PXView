# pxview-automation

Python 自动化客户端和命令行工具，用于控制 [PXView](https://github.com/opensource/PXView) 逻辑分析仪/示波器。

## 概述

`pxview-automation` 封装了 PXView 通过 HTTP 暴露的全部 **61 个 MCP (Model Context Protocol) 工具**，提供三层 API：

| 层级 | 类 | 用途 |
|------|-----|------|
| **低层** | `McpClient` | 61 个工具的一对一封装，每个方法对应一个 MCP tool |
| **高层** | `PXView` | 领域级语义封装，一行代码完成采集+解码+导出 |
| **CLI** | `pxview-cli` | 命令行工具，参考 sigrok-cli 风格 |

**零运行时依赖**——仅使用 Python 标准库（`urllib`、`json`、`base64`）。

## 安装

```bash
pip install pxview-automation
```

或从源码安装：

```bash
cd pxview-automation
pip install -e .
```

## 前置条件

PXView 必须以 **headless 模式** 运行，启动 MCP 服务（默认端口 10110）：

```bash
PXView.exe --headless        # Windows
./PXView --headless          # Linux/macOS
```

启动后会看到日志：`Headless mode started. MCP port 10110, WS port 10430.`

## 快速上手

### Python API（高层）

```python
from pxview_automation import PXView

with PXView() as pxv:
    pxv.connect()

    # 列出设备
    devices = pxv.list_devices()
    demo = pxv.find_device(demo=True)

    # 采集 + I2C 解码一条龙
    results = pxv.capture_and_decode(
        device_id=demo["id"],
        channels=[0, 1],
        sample_rate=1_000_000,
        duration_s=1.0,
        protocol="i2c",
        channel_map={"scl": 0, "sda": 1},
    )

    for ann in results:
        print(ann)

    # 导出原始数据
    pxv.export(format="csv", directory="./output")
```

### Python API（低层）

```python
from pxview_automation import McpClient

client = McpClient("http://127.0.0.1:10110/mcp")
client.connect()

devices = client.get_devices()
client.start_capture(
    device_id=devices[0]["id"],
    logic_device_configuration={
        "digitalChannels": [0, 1],
        "digitalSampleRate": 1000000,
    },
    capture_configuration={
        "timedCaptureMode": {"durationSeconds": 1.0},
    },
)
client.wait_capture(timeout_seconds=60)

samples = client.get_logic_samples(channel_index=0, start_sample=0, end_sample=1000)
print(f"Got {len(samples)} bytes")

client.disconnect()
```

### 自动启动 PXView

```python
from pxview_automation import PXViewProcess, PXView

with PXViewProcess(exe_path="C:/PXView/PXView.exe") as proc:
    with PXView(port=proc.port) as pxv:
        pxv.connect()
        print(pxv.list_devices())
```

### CLI

```bash
# 列出设备
pxview-cli list-devices

# 采集（1MHz采样率，2秒，通道0和1）
pxview-cli capture --device demo --channels 0,1 --rate 1M --time 2s

# 添加 I2C 解码器
pxview-cli decode --protocol i2c --channel-map scl=0 --channel-map sda=1

# 读取解码结果
pxview-cli results --analyzer-id 1:1

# 导出原始数据
pxview-cli export --format csv --dir ./output

# 读取样本
pxview-cli samples --channel 0 --start 0 --count 100 --type logic --format hex

# 一站式：采集 + 解码 + 导出
pxview-cli run --device demo --channels 0,1 --rate 1M --time 1s \
    --protocol i2c --channel-map scl=0 --channel-map sda=1 \
    --export csv:./output

# JSON 输出（方便脚本处理）
pxview-cli --json list-devices

# 自动启动 PXView（如果没在运行）
pxview-cli --auto-start --exe "C:/PXView/PXView.exe" list-devices
```

## API 层级说明

### 低层 `McpClient`（61 个工具）

每个 MCP tool 对应一个方法，方法名与 tool name 一致（snake_case）。完整列表见 [API 文档](docs/api-reference.md)。

### 高层 `PXView`（领域语义）

| 方法 | 说明 | 对应的 MCP tools |
|------|------|-----------------|
| `list_devices()` | 列出设备 | `get_devices` |
| `find_device()` | 按条件查找设备 | `get_devices` |
| `capture()` | 配置并采集 | `start_capture` + `wait_capture` |
| `add_decoder()` | 添加解码器 | `add_analyzer` |
| `get_decoder_results()` | 获取解码结果 | `get_analyzer_results` |
| `capture_and_decode()` | 采集+解码一条龙 | 上述全部 |
| `export()` | 导出原始数据 | `export_raw_data` |
| `export_decoder_table()` | 导出解码表 | `export_data_table_csv` |
| `get_logic_samples()` | 读逻辑样本 | `get_logic_samples` |
| `get_analog_samples()` | 读模拟样本 | `get_analog_samples` |
| `load()` / `save()` | 加载/保存采集 | `load_capture` / `save_capture` |

### 异常体系

```
PxvError                          # 所有异常的基类
├── McpError                      # MCP 工具返回错误
│   └── McpConnectionError        # 无法连接 MCP 服务器
├── ProcessError                  # PXView 进程管理错误
└── ConfigError                   # 配置/参数错误
```

## 配置

### MCP 端口

PXView 默认在 `127.0.0.1:10110` 启动 MCP 服务。如果需要修改端口，修改 PXView 源码 `appcontrol.cpp` 中的端口号。

### 超时与重试

```python
client = McpClient(
    url="http://127.0.0.1:10110/mcp",
    timeout=120.0,       # 默认 HTTP 超时
    max_retries=5,       # 连接失败重试次数
    retry_delay=1.0,     # 重试间隔
)
```

## CLI 命令参考

| 命令 | 说明 |
|------|------|
| `list-devices` | 列出所有连接的设备 |
| `scan` | 热插拔扫描设备 |
| `channels` | 列出当前设备的通道 |
| `capture` | 执行采集 |
| `decode` | 添加协议解码器 |
| `results` | 获取解码结果 |
| `export` | 导出原始数据 |
| `export-table` | 导出解码表为 CSV |
| `samples` | 读取原始样本 |
| `status` | 获取采集状态 |
| `save` | 保存采集到 .pxc 文件 |
| `load` | 从 .pxc 文件加载采集 |
| `run` | 采集+解码+导出一条龙 |
| `list-decoders` | 列出可用解码器 |

完整 CLI 文档见 [CLI 参考](docs/cli-reference.md)。

## 开发

```bash
# 安装开发依赖
pip install -e ".[dev]"

# 运行测试
pytest

# 代码检查
ruff check src/
mypy src/
```

## 许可证

GPL-2.0-or-later（与 PXView 主项目一致）
