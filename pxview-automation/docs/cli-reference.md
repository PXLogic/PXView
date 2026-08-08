# CLI 参考

## 全局选项

所有子命令共享以下选项（放在子命令之前）：

```
--host HOST         MCP 服务器地址（默认 127.0.0.1）
--port PORT         MCP 服务器端口（默认 10110）
--timeout SECS      HTTP 超时秒数（默认 60）
--json              输出原始 JSON（方便脚本处理）
--auto-start        如果服务器不可达，自动启动 PXView --headless
--exe PATH          PXView 可执行文件路径（配合 --auto-start）
```

## 子命令

### list-devices

列出所有连接的设备。

```bash
pxview-cli list-devices
pxview-cli --json list-devices
```

输出示例：
```
ID                              Driver          Name                      Type
------------------------------------------------------------------------------
demo                            demo            Demo Device               demo
fx2lafw:usb:0x0925:0x3881      fx2lafw         LA1010                    hardware
```

### scan

热插拔扫描，返回更新后的设备列表。

```bash
pxview-cli scan
```

### channels

列出当前设备的通道。

```bash
pxview-cli channels
```

### capture

执行采集。

```bash
pxview-cli capture \
    --device demo \
    --channels 0,1,2,3 \
    --rate 1M \
    --time 2s \
    --threshold 1.8
```

参数：
- `--device` (必填)：设备 ID
- `--channels`：数字通道列表，如 `0,1,2-4`
- `--analog-channels`：模拟通道列表
- `--rate`：采样率，支持后缀 `K`/`M`/`G`，如 `1M` = 1000000
- `--time`：采集时长，如 `1s`/`500ms`/`2m`
- `--samples`：采样数（与 `--time` 二选一）
- `--threshold`：数字阈值电压
- `--trigger`：触发设置，格式 `通道:类型`，如 `0:rising`/`1:falling`/`0:pulse_high`
- `--no-wait`：不等待采集完成

### decode

添加协议解码器。

```bash
pxview-cli decode \
    --protocol i2c \
    --channel-map scl=0 \
    --channel-map sda=1

pxview-cli decode \
    --protocol uart \
    --channel-map rx=0 \
    --option baudrate=115200
```

参数：
- `--protocol` (必填)：解码器名称，如 `i2c`/`spi`/`uart`
- `--device`：设备 ID（headless 模式下需要）
- `--channel-map`：通道映射，可多次指定，格式 `名称=索引`
- `--option`：解码器选项，可多次指定，格式 `键=值`

### results

获取解码结果。

```bash
pxview-cli results --analyzer-id 1:1
pxview-cli results --analyzer-id 1:1 --max 100
```

### export

导出原始采集数据。

```bash
pxview-cli export --format csv --dir ./output
pxview-cli export --format vcd --dir ./output --digital-channels 0,1
```

参数：
- `--format`：`csv`/`binary`/`vcd`/`hex`/`bits`（默认 csv）
- `--dir` (必填)：输出目录
- `--digital-channels`：要导出的数字通道
- `--analog-channels`：要导出的模拟通道

### export-table

将解码结果导出为 CSV 表格。

```bash
pxview-cli export-table --out decoded.csv
pxview-cli export-table --analyzer-id 1:1 --out i2c.csv
```

### samples

读取原始样本。

```bash
# 逻辑通道，十六进制输出
pxview-cli samples --channel 0 --start 0 --count 100 --type logic --format hex

# 模拟通道
pxview-cli samples --channel 0 --start 0 --count 1000 --type analog

# DSO 通道
pxview-cli samples --channel 0 --start 0 --count 100 --type dso
```

参数：
- `--channel` (必填)：通道索引
- `--start`：起始样本索引（默认 0）
- `--count`：读取数量（默认到末尾）
- `--type`：`logic`/`analog`/`dso`（默认 logic）
- `--format`：`hex`/`bin`/`dec`（默认 hex，仅 logic 类型）

### status

获取当前采集状态。

```bash
pxview-cli status
```

### save

保存当前采集到 .pxc 文件。

```bash
pxview-cli save --out capture.pxc
```

### load

从 .pxc 文件加载采集。

```bash
pxview-cli load --file capture.pxc
```

### run

一站式命令：采集 + 解码 + 导出。

```bash
pxview-cli run \
    --device demo \
    --channels 0,1 \
    --rate 1M \
    --time 1s \
    --protocol i2c \
    --channel-map scl=0 \
    --channel-map sda=1 \
    --export csv:./output \
    --export vcd:./vcd_output
```

参数：
- `--device` (必填)：设备 ID
- `--channels`：数字通道
- `--rate`：采样率
- `--time`：采集时长
- `--samples`：采样数
- `--protocol`：解码器名称
- `--channel-map`：通道映射（可多次指定）
- `--option`：解码器选项（可多次指定）
- `--export`：导出规格 `格式:路径`（可多次指定）

### list-decoders

列出所有可用的协议解码器。

```bash
pxview-cli list-decoders
```

## 自动启动 PXView

如果 PXView 没有在运行，可以使用 `--auto-start` 自动启动：

```bash
pxview-cli --auto-start --exe "C:/PXView/PXView.exe" list-devices
```

如果不指定 `--exe`，会搜索以下位置：
1. `PATH` 环境变量
2. `C:\Program Files\PXView\PXView.exe`
3. `C:\Program Files (x86)\PXView\PXView.exe`
4. `C:\PXView\PXView.exe`
5. `D:\PXView\PXView.exe`

## JSON 输出

所有命令支持 `--json` 选项，输出格式化的 JSON：

```bash
pxview-cli --json list-devices | jq '.[0].id'
```
