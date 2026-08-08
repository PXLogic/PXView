# 示例

## 示例 1：基本采集

```python
from pxview_automation import PXView

with PXView() as pxv:
    pxv.connect()

    # 使用 demo 设备
    device = pxv.find_device(demo=True)

    # 采集：通道 0 和 1，1MHz，1秒
    status = pxv.capture(
        device_id=device["id"],
        channels=[0, 1],
        sample_rate=1_000_000,
        duration_s=1.0,
    )
    print(f"采集完成: {status['state']}")

    # 读取通道 0 前 100 个样本
    samples = pxv.get_logic_samples(channel=0, start=0, count=100)
    print(f"样本字节: {samples[:16].hex()}")
```

## 示例 2：I2C 协议解码

```python
from pxview_automation import PXView

with PXView() as pxv:
    pxv.connect()
    device = pxv.find_device(demo=True)

    # 采集 + I2C 解码一条龙
    results = pxv.capture_and_decode(
        device_id=device["id"],
        protocol="i2c",
        channel_map={"scl": 0, "sda": 1},
        channels=[0, 1],
        sample_rate=1_000_000,
        duration_s=0.5,
    )

    # 打印解码结果
    for ann in results:
        texts = ann.get("texts", [])
        start = ann.get("start_sample", "?")
        end = ann.get("end_sample", "?")
        print(f"[{start}-{end}] {' | '.join(texts)}")
```

## 示例 3：SPI 解码 + 导出

```python
from pxview_automation import PXView

with PXView() as pxv:
    pxv.connect()
    device = pxv.find_device(demo=True)

    # 添加 SPI 解码器（采集前添加，自动解码）
    analyzer_id = pxv.add_decoder(
        protocol="spi",
        channel_map={"cs": 0, "clk": 1, "mosi": 2, "miso": 3},
        options={"cs_polarity": "active_low"},
        device_id=device["id"],
    )
    print(f"解码器 ID: {analyzer_id}")

    # 采集
    pxv.capture(
        device_id=device["id"],
        channels=[0, 1, 2, 3],
        sample_rate=2_000_000,
        duration_s=2.0,
    )

    # 导出原始数据为 CSV
    pxv.export(format="csv", directory="./output")

    # 导出解码表为 CSV
    pxv.export_decoder_table(filepath="./spi_decoded.csv", analyzer_id=analyzer_id)
```

## 示例 4：UART 解码（自定义波特率）

```python
from pxview_automation import PXView

with PXView() as pxv:
    pxv.connect()
    device = pxv.find_device(demo=True)

    results = pxv.capture_and_decode(
        device_id=device["id"],
        protocol="uart",
        channel_map={"rx": 0},
        decoder_options={"baudrate": "115200", "data_bits": "8"},
        channels=[0],
        sample_rate=500_000,
        duration_s=1.0,
    )

    # 提取所有解码文本
    decoded_text = ""
    for ann in results:
        texts = ann.get("texts", [])
        if texts:
            decoded_text += texts[0]
    print(f"UART 解码: {decoded_text}")
```

## 示例 5：触发采集

```python
from pxview_automation import PXView

with PXView() as pxv:
    pxv.connect()
    device = pxv.find_device(demo=True)

    # 通道 0 上升沿触发
    status = pxv.capture(
        device_id=device["id"],
        channels=[0, 1],
        sample_rate=10_000_000,
        duration_s=0.01,
        trigger_channel=0,
        trigger_type="rising",
    )
    print(f"触发状态: triggered={status.get('triggered', False)}")
```

## 示例 6：加载已保存的采集

```python
from pxview_automation import PXView

with PXView() as pxv:
    pxv.connect()

    # 加载之前的采集
    pxv.load("C:/captures/prev_capture.pxc")

    # 添加解码器分析已有数据
    analyzer_id = pxv.add_decoder(
        protocol="i2c",
        channel_map={"scl": 0, "sda": 1},
    )

    import time
    time.sleep(1)  # 等待解码完成

    results = pxv.get_decoder_results(analyzer_id)
    for ann in results:
        print(ann)
```

## 示例 7：使用低层 API（完全控制）

```python
from pxview_automation import McpClient

client = McpClient()
client.connect()

# 手动配置
client.connect_device("demo")

# 逐通道启用
for ch in [0, 1, 2, 3]:
    client.set_channel_enabled(ch, True)

client.set_sample_rate(1_000_000)

# 添加解码器
client.add_analyzer(
    analyzer_name="i2c",
    settings={"channelMap": {"scl": 0, "sda": 1}},
)

# 启动采集
client.start_capture(
    device_id="demo",
    capture_configuration={
        "timedCaptureMode": {"durationSeconds": 1.0},
    },
)

# 等待完成
client.wait_capture(timeout_seconds=60)

# 获取状态
status = client.get_capture_status()
print(f"Status: {status}")

# 获取解码器列表
decoders = client.get_active_decoders()
print(f"Active decoders: {decoders}")

# 获取解码结果
if decoders:
    anns = client.get_analyzer_results(
        analyzer_id=decoders[0]["instance_id"],
        max_count=100,
    )
    for ann in anns:
        print(ann)

client.disconnect()
```

## 示例 8：自动启动 PXView

```python
from pxview_automation import PXViewProcess, PXView

# 自动启动 PXView --headless
with PXViewProcess(exe_path="C:/PXView/PXView.exe") as proc:
    print(f"PXView started on port {proc.port}")

    with PXView(port=proc.port) as pxv:
        pxv.connect()
        devices = pxv.list_devices()
        print(f"Found {len(devices)} devices")

# PXView 进程自动停止
```

## 示例 9：CI/CD 自动化测试

```python
"""pytest fixture: 自动启动 PXView demo 设备采集。"""
import pytest
from pxview_automation import PXView, PXViewProcess, McpConnectionError


@pytest.fixture(scope="session")
def pxview_process():
    """启动 PXView headless 进程。"""
    with PXViewProcess(exe_path="C:/PXView/PXView.exe") as proc:
        yield proc


@pytest.fixture(scope="session")
def pxview(pxview_process):
    """连接 PXView 并返回高层 API。"""
    with PXView(port=pxview_process.port) as pxv:
        pxv.connect()
        yield pxv


def test_demo_device_exists(pxview):
    devices = pxview.list_devices()
    demos = [d for d in devices if d.get("is_demo")]
    assert len(demos) > 0, "No demo device found"


def test_basic_capture(pxview):
    device = pxview.find_device(demo=True)
    assert device is not None

    status = pxview.capture(
        device_id=device["id"],
        channels=[0],
        sample_rate=100_000,
        duration_s=0.1,
    )
    assert status["state"] == "completed"


def test_i2c_decode(pxview):
    device = pxview.find_device(demo=True)

    results = pxview.capture_and_decode(
        device_id=device["id"],
        protocol="i2c",
        channel_map={"scl": 0, "sda": 1},
        channels=[0, 1],
        sample_rate=100_000,
        duration_s=0.1,
    )
    assert isinstance(results, list)
```
