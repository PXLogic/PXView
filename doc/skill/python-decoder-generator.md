# python-decoder-generator

## Metadata

- **Name**: python-decoder-generator
- **Description**: Generate Python protocol decoders for the PXView/libsigrokdecode engine. Creates complete `__init__.py` + `pd.py` decoder modules from protocol specs or by translating C decoders back to Python.
- **Trigger**: When user asks to create/add a new Python decoder, write a Python protocol decoder from spec/datasheet, port a C decoder to Python, or implement a new protocol decoder in Python.

---

## 概述

PXView 使用 `libsigrokdecode` 解码引擎，支持 Python 和 C 两种协议解码器。Python 解码器位于 `libsigrokdecode/decoders/<name>/` 目录下，每个解码器由两个文件组成：

- `__init__.py` — 模块文档字符串 + 导入 Decoder 类
- `pd.py` — 解码器类，包含所有协议解析逻辑

Python 解码器基于 `sigrokdecode as srd` 模块，继承 `srd.Decoder` 基类，使用 `api_version = 3`。相比 C 解码器，Python 解码器开发更快速、更灵活，适合原型开发和新协议快速验证。

**适用场景**：
- 从协议规范/数据手册创建新 Python 解码器
- 从已有 C 解码器反向翻译为 Python 解码器
- 为新协议或私有协议快速实现解码器原型

**参考资源**：
- 协议分析方法论：参考 `doc/c-decoder-from-spec-guide.md`（语言无关，适用于 Python）
- C 解码器 API 对照：参考 `doc/c-decoder-guide.md` 第 3 章 Python→C 翻译规则对照表（可反向参考）
- 测试方法：参考 `doc/c-decoder-testing.md`

---

## 阶段 1: Python 解码器文件结构

### 1.1 目录结构

```
libsigrokdecode/decoders/<name>/
├── __init__.py    # 模块文档字符串 + 导入
└── pd.py          # 解码器类实现
```

### 1.2 `__init__.py` 模板

```python
##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) <year> <author>
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, see <http://www.gnu.org/licenses/>.
##

'''
<协议简短描述>
'''

from .pd import Decoder
```

### 1.3 `pd.py` 骨架模板

```python
##
## This file is part of the libsigrokdecode project.
##
## Copyright (C) <year> <author>
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program; if not, see <http://www.gnu.org/licenses/>.
##

import sigrokdecode as srd

# 通道索引常量（可选，提高可读性）
CLK = 0
DATA = 1

class Decoder(srd.Decoder):
    api_version = 3
    id = '<name>'
    name = '<Name>'
    longname = '<Long name>'
    desc = '<Description>'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = []
    tags = ['Embedded/industrial']

    channels = (
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock line(时钟线)'},
    )
    optional_channels = (
        {'id': 'cs', 'name': 'CS', 'desc': 'Chip select(片选)'},
    )
    options = (
        {'id': 'bitorder', 'desc': 'Bit order(位序)', 'default': 'msb',
            'values': ('msb', 'lsb')},
    )
    annotations = (
        ('start', 'Start condition'),
        ('data', 'Data byte'),
        ('error', 'Error'),
    )
    annotation_rows = (
        ('control', 'Control', (0, 2)),
        ('data', 'Data', (1,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.state = 'IDLE'
        self.bitcount = 0
        self.databyte = 0

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def decode(self):
        while True:
            # 等待条件并处理
            pins = self.wait({0: 'r'})
            # 处理逻辑...
```

---

## 阶段 2: Python 解码器 API 完整参考

### 2.1 类属性定义

| 属性 | 类型 | 说明 |
|------|------|------|
| `api_version` | int | 固定为 `3` |
| `id` | str | 解码器唯一标识，如 `'i2c'` |
| `name` | str | 短名称，如 `'I²C'` |
| `longname` | str | 长名称 |
| `desc` | str | 描述 |
| `license` | str | `'gplv2+'` 或 `'gplv3+'` |
| `inputs` | tuple | 输入类型列表，如 `['logic']` 或 `['i2c']` |
| `outputs` | tuple | 输出类型列表，如 `['i2c']` 或 `[]` |
| `tags` | tuple | 标签，如 `['Embedded/industrial']` |

### 2.2 通道定义

```python
# 必选通道
channels = (
    {'id': 'scl', 'name': 'SCL', 'desc': 'Serial clock line(串行时钟线)'},
    {'id': 'sda', 'name': 'SDA', 'desc': 'Serial data line(串行数据线)'},
)

# 可选通道
optional_channels = (
    {'id': 'cs', 'name': 'CS', 'desc': 'Chip select(片选)'},
)
```

| 字段 | 说明 |
|------|------|
| `id` | 通道标识，如 `'scl'` |
| `name` | 通道显示名，如 `'SCL'` |
| `desc` | 通道描述（支持中文双语） |
| `idn` | 可选，国际化文本源 ID，如 `'dec_i2c_chan_scl'` |
| `type` | 可选，通道类型: -1=COMMON, 0=SCLK, 1=SDATA, 2=ADATA |

**通道索引**：`channels` 中的通道索引从 0 开始，`optional_channels` 中的通道索引紧接 `channels` 之后。例如 `channels` 有 2 个通道，`optional_channels` 有 1 个通道，则通道索引为 0, 1, 2。

### 2.3 选项定义

```python
options = (
    # 整数选项
    {'id': 'baudrate', 'desc': 'Baud rate(波特率)', 'default': 115200},

    # 整数选项带可选值
    {'id': 'data_bits', 'desc': 'Data bits(数据位数)', 'default': 8,
        'values': (5, 6, 7, 8, 9)},

    # 字符串选项带可选值
    {'id': 'parity', 'desc': 'Parity(校验位)', 'default': 'none',
        'values': ('none', 'odd', 'even', 'zero', 'one', 'ignore')},

    # 浮点选项
    {'id': 'stop_bits', 'desc': 'Stop bits(停止位)', 'default': 1.0,
        'values': (0.0, 0.5, 1.0, 1.5, 2.0)},

    # 布尔选项（用字符串 yes/no）
    {'id': 'invert', 'desc': 'Invert signal(反转信号)', 'default': 'no',
        'values': ('yes', 'no')},
)
```

| 字段 | 说明 |
|------|------|
| `id` | 选项标识 |
| `desc` | 选项描述（支持中文双语） |
| `default` | 默认值 |
| `values` | 可选，可选值元组 |
| `idn` | 可选，国际化文本源 ID |

**读取选项**：在 `start()` 或 `decode()` 中通过 `self.options['<id>']` 读取。

### 2.4 注解定义

```python
annotations = (
    # (id, label) 或 (id, label, description)
    ('start', 'Start condition'),           # 注解类 0
    ('address', 'Address byte'),            # 注解类 1
    ('data', 'Data byte'),                  # 注解类 2
    ('error', 'Error'),                     # 注解类 3
)

annotation_rows = (
    # (row_id, row_label, (annotation_class_indices,))
    ('control', 'Control', (0, 3)),         # 行 0: 包含注解类 0 和 3
    ('data', 'Data', (1, 2)),               # 行 1: 包含注解类 1 和 2
)
```

**注解类编号**：`annotations` 元组中的顺序决定了注解类编号（从 0 开始）。这个编号在 `self.put()` 调用中使用。

**注解类型特殊值**：
- 类型 0-16：对应不同填充颜色
- 类型 200-299：绘制边沿箭头

### 2.5 二进制输出定义

```python
binary = (
    ('raw', 'RAW file'),
    ('data', 'Data bytes'),
)
```

### 2.6 `self.wait()` — 条件等待

```python
# 返回值: 当前各通道电平值的元组
pins = self.wait(condition)
```

**条件格式**：

| Python 条件 | 含义 |
|-------------|------|
| `{0: 'r'}` | 通道 0 上升沿 |
| `{0: 'f'}` | 通道 0 下降沿 |
| `{0: 'e'}` | 通道 0 任意边沿 |
| `{0: 'h'}` | 通道 0 高电平 |
| `{0: 'l'}` | 通道 0 低电平 |
| `{0: 'n'}` | 通道 0 无边沿 |
| `{0: 'h', 1: 'f'}` | AND 条件: 通道 0 高 且 通道 1 下降沿 |
| `[{0: 'r'}, {0: 'h', 1: 'f'}]` | OR 条件: 通道 0 上升沿 或 (通道 0 高 且 通道 1 下降沿) |
| `{'skip': 100}` | 跳过 100 个采样 |
| `{}` 或 `self.wait()` | 无条件前进一个采样 |

**条件字符**：

| 字符 | 含义 |
|------|------|
| `'r'` | 上升沿 (Rising edge) |
| `'f'` | 下降沿 (Falling edge) |
| `'e'` | 任意边沿 (Either edge) |
| `'h'` | 高电平 (High) |
| `'l'` | 低电平 (Low) |
| `'n'` | 无边沿 (No edge) |

**返回值**：返回一个元组，包含所有通道（必选 + 可选）在当前采样点的电平值。元组长度 = `len(channels) + len(optional_channels)`。

```python
# 2 个通道: SCL=0, SDA=1
(scl, sda) = self.wait({0: 'r'})

# 4 个通道: CLK=0, MOSI=1, MISO=2, CS=3
(clk, mosi, miso, cs) = self.wait([{0: 'r'}, {3: 'r'}])
```

### 2.7 `self.matched` — 条件匹配位掩码

当 `self.wait()` 使用 OR 条件（列表）时，`self.matched` 指示哪个条件组被匹配：

```python
# 等待: SCL上升沿 OR (SCL高 AND SDA下降沿) OR (SCL高 AND SDA上升沿)
(scl, sda) = self.wait([{0: 'r'}, {0: 'h', 1: 'f'}, {0: 'h', 1: 'r'}])

if self.matched & (0b1 << 0):    # 第 0 个条件组匹配: SCL上升沿
    handle_data_bit(scl, sda)
elif self.matched & (0b1 << 1):  # 第 1 个条件组匹配: START条件
    handle_start(scl, sda)
elif self.matched & (0b1 << 2):  # 第 2 个条件组匹配: STOP条件
    handle_stop(scl, sda)
```

**关键**：`self.matched` 是位掩码，使用 `&` 运符检查，不是 `==`。

### 2.8 `self.put()` — 注解输出

```python
# 基本注解: self.put(start_sample, end_sample, output_id, [class_index, [text_list]])
self.put(ss, es, self.out_ann, [0, ['Start', 'S']])

# 多级文本变体: ['长文本', '中文本', '短文本']
self.put(ss, es, self.out_ann, [1, ['Address: 0x50', 'AW: 50', '50']])

# 单级文本
self.put(ss, es, self.out_ann, [2, ['Data']])
```

| 参数 | 说明 |
|------|------|
| `ss` | 起始采样号 |
| `es` | 结束采样号 |
| `output_id` | 输出 ID（由 `self.register()` 返回） |
| `class_index` | 注解类编号（对应 `annotations` 元组下标） |
| `text_list` | 文本列表，1-3 个字符串（长/中/短） |

### 2.9 `self.putp()` / 协议输出 — 向上层解码器发送数据

```python
# 注册协议输出
self.out_python = self.register(srd.OUTPUT_PYTHON)

# 发送协议数据: [command, data]
self.putp(ss, es, ['START', None])                    # 无数据
self.putp(ss, es, ['ADDRESS WRITE', 0x50])             # 带数据
self.putp(ss, es, ['DATA READ', data_byte])            # 带数据
self.putp(ss, es, ['BITS', bits_list])                 # 带位列表
```

**协议命令字符串**：上层解码器通过命令字符串识别协议事件。常见命令：`'START'`, `'STOP'`, `'ADDRESS READ'`, `'ADDRESS WRITE'`, `'DATA READ'`, `'DATA WRITE'`, `'ACK'`, `'NACK'`, `'BITS'`。

### 2.10 `self.register()` — 输出注册

```python
def start(self):
    self.out_ann = self.register(srd.OUTPUT_ANN)         # 注解输出
    self.out_python = self.register(srd.OUTPUT_PYTHON)   # 协议输出（用于堆叠）
    self.out_binary = self.register(srd.OUTPUT_BINARY)   # 二进制输出
    self.out_bitrate = self.register(srd.OUTPUT_META,    # 元数据输出
        meta=(int, 'Bitrate', 'Bitrate from Start bit to Stop bit'))
```

| 输出类型 | 常量 | 用途 |
|----------|------|------|
| 注解 | `srd.OUTPUT_ANN` | 在波形上显示文本标注 |
| 协议 | `srd.OUTPUT_PYTHON` | 向上层堆叠解码器发送数据 |
| 二进制 | `srd.OUTPUT_BINARY` | 输出二进制数据（可导出） |
| 元数据 | `srd.OUTPUT_META` | 输出数值元数据（如比特率） |

### 2.11 核心属性

| 属性 | 类型 | 说明 |
|------|------|------|
| `self.samplenum` | int | 当前 `wait()` 返回的采样号 |
| `self.matched` | int | 条件匹配位掩码 |
| `self.samplerate` | int/None | 采样率（Hz），通过 `metadata()` 设置 |
| `self.options` | dict | 用户配置的选项值 |
| `self.initial_pin` | list | 各通道初始引脚值（可选通道未连接时为 `None`） |

### 2.12 辅助方法

| 方法 | 说明 |
|------|------|
| `self.has_channel(ch)` | 检查通道 `ch` 是否已连接（用于可选通道） |
| `self.wait(condition)` | 等待条件，返回引脚值元组 |
| `self.put(ss, es, out, data)` | 输出注解/协议/二进制数据 |
| `self.register(type)` | 注册输出通道，返回输出 ID |

### 2.13 回调方法

| 方法 | 调用时机 | 说明 |
|------|----------|------|
| `__init__(self)` | 实例创建时 | 初始化，通常调用 `self.reset()` |
| `reset(self)` | 重置时 | 初始化所有状态变量 |
| `start(self)` | 解码开始前 | 注册输出，读取选项 |
| `metadata(self, key, value)` | 接收元数据时 | 通常接收采样率 |
| `decode(self)` | 解码主循环 | 从采样流读取并输出注解 |
| `decode(self, ss, es, data)` | 堆叠解码时 | 接收下层解码器的协议数据 |

**两种 `decode()` 签名**：
- **直接解码**（`inputs = ['logic']`）：`def decode(self):` — 使用 `self.wait()` 从采样流读取
- **堆叠解码**（`inputs = ['i2c']` 等）：`def decode(self, ss, es, data):` — 接收下层协议数据

---

## 阶段 3: 从协议规范创建 Python 解码器

### 3.1 协议分析方法论

协议分析方法论与语言无关，请参考 `doc/c-decoder-from-spec-guide.md` 第 2 章。核心步骤：

1. **信号线提取**：从时序图识别时钟/数据/控制线，映射到 `channels`/`optional_channels`
2. **帧格式分析**：提取帧边界条件和字段定义，映射到 `annotations` 枚举
3. **时序约束提取**：确定采样边沿和等待条件，映射到 `self.wait()` 条件
4. **校验机制提取**：提取 CRC/校验和参数，实现校验逻辑
5. **状态机建模**：构建状态转换图，映射到 `self.state` + `if/elif` 结构

### 3.2 时序图 → self.wait() 条件映射

| 规范描述 | Python 代码 |
|----------|-------------|
| "在 CLK 上升沿采样数据" | `self.wait({CLK: 'r'})` |
| "在 CLK 下降沿采样数据" | `self.wait({CLK: 'f'})` |
| "等待 CS# 拉低后，在 CLK 上升沿采样" | 先 `self.wait({CS: 'l'})`，再 `self.wait({CLK: 'r'})` |
| "等待 START 条件（SCL 高时 SDA 下降沿）" | `self.wait({SCL: 'h', SDA: 'f'})` |
| "等待 STOP 条件（SCL 高时 SDA 上升沿）" | `self.wait({SCL: 'h', SDA: 'r'})` |
| "等待 SCL 上升沿或 START 或 STOP" | `self.wait([{SCL: 'r'}, {SCL: 'h', SDA: 'f'}, {SCL: 'h', SDA: 'r'}])` |
| "跳过 N 个采样" | `self.wait({'skip': N})` |
| "等待 CLK 双沿（DDR 模式）" | `self.wait({CLK: 'e'})` |

### 3.3 字段表 → annotations + self.put() 映射

规范字段表中的每个独立语义字段对应一个注解类和一个 `self.put()` 调用。

**示例：假设一个简化的寄存器读协议**

规范字段表：

| 字段 | 位宽 | 描述 |
|------|------|------|
| START | 1 bit | 起始位 |
| CMD | 2 bit | 命令码 |
| ADDR | 6 bit | 寄存器地址 |
| DATA | 8 bit | 数据 |
| CRC | 4 bit | CRC 校验 |

映射到注解定义和输出：

```python
annotations = (
    ('start-bit', 'Start bit'),       # 0
    ('command', 'Command'),           # 1
    ('address', 'Address'),           # 2
    ('data', 'Data'),                 # 3
    ('crc-ok', 'CRC OK'),             # 4
    ('crc-err', 'CRC error'),         # 5
)

# 在 decode() 中
self.put(ss_start, es_start, self.out_ann, [0, ['Start bit', 'Start', 'S']])
self.put(ss_cmd, es_cmd, self.out_ann, [1, [cmd_str, 'CMD']])
self.put(ss_addr, es_addr, self.out_ann, [2, ['Address: 0x%02X' % addr, 'ADDR']])
```

### 3.4 状态图 → self.state + if/elif

```python
# 状态枚举（用字符串）
# IDLE → FIND_START → FIND_ADDRESS → FIND_DATA → FIND_CRC → IDLE

def decode(self):
    while True:
        if self.state == 'IDLE':
            # 等待起始条件
            pins = self.wait({DATA: 'f'})
            self.state = 'FIND_ADDRESS'
            self.bitcount = 0
            self.databyte = 0

        elif self.state == 'FIND_ADDRESS':
            pins = self.wait({CLK: 'r'})
            bit = pins[DATA]
            self.databyte = (self.databyte << 1) | bit
            self.bitcount += 1
            if self.bitcount >= 6:
                self.address = self.databyte
                self.state = 'FIND_DATA'

        elif self.state == 'FIND_DATA':
            pins = self.wait({CLK: 'r'})
            bit = pins[DATA]
            self.databyte = (self.databyte << 1) | bit
            self.bitcount += 1
            if self.bitcount >= 8:
                self.data = self.databyte
                self.state = 'FIND_CRC'
```

### 3.5 校验算法实现

**CRC-8 示例**：

```python
def compute_crc8(self, data, poly=0x07, init=0x00):
    crc = init
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ poly) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

# 在 decode() 中使用
if self.compute_crc8(data_bytes) == received_crc:
    self.put(ss, es, self.out_ann, [4, ['CRC OK', 'OK']])
else:
    self.put(ss, es, self.out_ann, [5, ['CRC ERROR', 'ERR']])
```

**奇偶校验示例**：

```python
def parity_ok(self, data, parity_bit, parity_type, data_bits):
    ones = bin(data).count('1') + parity_bit
    if parity_type == 'odd':
        return (ones % 2) == 1
    elif parity_type == 'even':
        return (ones % 2) == 0
    elif parity_type == 'zero':
        return parity_bit == 0
    elif parity_type == 'one':
        return parity_bit == 1
    return True  # 'ignore'
```

### 3.6 可配置参数 → options 定义

```python
options = (
    # 整数选项：波特率
    {'id': 'baudrate', 'desc': 'Baud rate(波特率)', 'default': 115200},

    # 字符串选项：位序
    {'id': 'bitorder', 'desc': 'Bit order(位序)', 'default': 'msb',
        'values': ('msb', 'lsb')},

    # 字符串选项：CS极性
    {'id': 'cs_polarity', 'desc': 'CS polarity(CS极性)', 'default': 'active-low',
        'values': ('active-low', 'active-high')},

    # 布尔选项：反转数据
    {'id': 'invert', 'desc': 'Invert data(反转数据)', 'default': 'no',
        'values': ('yes', 'no')},
)

# 在 start() 中读取
def start(self):
    self.out_ann = self.register(srd.OUTPUT_ANN)
    self.baudrate = self.options['baudrate']
    self.msb_first = self.options['bitorder'] == 'msb'
    self.cs_active_low = self.options['cs_polarity'] == 'active-low'
    self.invert = self.options['invert'] == 'yes'
```

---

## 阶段 4: 常见解码器模式

### 4.1 简单解码器（无堆叠）

参考 `decoders/counter/pd.py`、`decoders/pwm/pd.py`。特点：
- `inputs = ['logic']`, `outputs = []`
- `decode(self)` 使用 `self.wait()` 从采样流读取
- 无协议输出

```python
def decode(self):
    while True:
        # 等待上升沿
        (data,) = self.wait({0: 'r'})
        # 处理并输出注解
        self.put(self.samplenum, self.samplenum, self.out_ann, [0, ['Edge']])
```

### 4.2 复杂解码器（带协议输出，可被堆叠）

参考 `decoders/i2c/pd.py`、`decoders/spi/pd.py`、`decoders/uart/pd.py`。特点：
- `inputs = ['logic']`, `outputs = ['<proto_id>']`
- 注册 `OUTPUT_PYTHON` 输出
- `decode(self)` 使用 `self.wait()` 从采样流读取
- 通过 `self.putp()` 向上层发送协议数据

```python
def start(self):
    self.out_ann = self.register(srd.OUTPUT_ANN)
    self.out_python = self.register(srd.OUTPUT_PYTHON)

def decode(self):
    while True:
        (scl, sda) = self.wait({0: 'h', 1: 'f'})  # START
        self.put(self.samplenum, self.samplenum, self.out_ann, [0, ['Start', 'S']])
        self.putp(self.samplenum, self.samplenum, ['START', None])
        # ...
```

### 4.3 堆叠解码器（接收下层协议数据）

参考 `decoders/lm75/pd.py`、`decoders/spiflash/pd.py`。特点：
- `inputs = ['i2c']` 等上层协议 ID
- `outputs = []` 或继续向上输出
- `channels = ()` 或无通道定义（数据来自协议层）
- `decode(self, ss, es, data)` 接收协议数据

```python
class Decoder(srd.Decoder):
    api_version = 3
    id = 'mydevice'
    name = 'MyDevice'
    longname = 'My Device'
    desc = 'My device decoder, stacks on top of i2c'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = []
    tags = ['IC']

    annotations = (
        ('temperature', 'Temperature'),
        ('config', 'Configuration'),
        ('warnings', 'Warnings'),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 'IDLE'
        self.databytes = []

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def decode(self, ss, es, data):
        # data 是下层解码器通过 putp 发送的 [cmd, pdata]
        cmd, databyte = data
        self.ss, self.es = ss, es

        if self.state == 'IDLE':
            if cmd != 'START':
                return
            self.state = 'GET SLAVE ADDR'
        elif self.state == 'GET SLAVE ADDR':
            if cmd in ('ADDRESS READ', 'ADDRESS WRITE'):
                # 检查地址是否匹配
                if databyte == self.expected_addr:
                    self.state = 'READ REGS' if 'READ' in cmd else 'WRITE REGS'
                else:
                    self.state = 'IDLE'
        elif self.state == 'READ REGS':
            if cmd == 'DATA READ':
                self.databytes.append(databyte)
            elif cmd == 'STOP':
                self.process_data()
                self.state = 'IDLE'
```

### 4.4 ATK 颜色注解

许多解码器在 `decode()` 开头输出 ATK 颜色注解，用于前端自动着色：

```python
def decode(self):
    # ATK 颜色注解（在 decode 开头输出一次）
    self.put(self.ss, self.ss, self.out_ann, [ANN_ATK_DATA_POINT, ["color:#4edc44"]])
    self.put(self.ss, self.ss, self.out_ann, [ANN_ATK_RISING_EDGE, ["color:#4edc44"]])

    while True:
        # 正常解码逻辑...
```

颜色参考表：
```
#EF2929 #F66A32 #FCAE3E #FBCA47 #FCE94F #CDF040 #8AE234 #4EDC44
#55D795 #64D1D2 #729FCF #D476C4 #9D79B9 #AD7FA8 #C2629B #D7476F
```

### 4.5 数据包（Packet）注解

部分解码器（如 i2c）输出数据包级注解，将整个传输序列汇总：

```python
def handle_packet(self):
    if not len(self.packet_data):
        return
    # 格式化数据包字符串
    packet_str = "0x%02X %s: %s" % (self.address, 'RD' if self.wr == 0 else 'WR',
                                     ' '.join('%02X' % b for b in self.packet_data))
    self.put(self.packet_ss, self.packet_es, self.out_ann,
             [ANN_PACKET, [packet_str, packet_str[2:]]])
```

---

## 阶段 5: 从 C 解码器反向翻译为 Python

如果已有 C 解码器（`c_decoders/<name>_c.c`），需要反向翻译为 Python：

### 5.1 C → Python API 映射

| C | Python |
|---|--------|
| `c_wait(di, CW_R(0), CW_END)` | `self.wait({0: 'r'})` |
| `c_wait(di, CW_F(0), CW_END)` | `self.wait({0: 'f'})` |
| `c_wait(di, CW_E(0), CW_END)` | `self.wait({0: 'e'})` |
| `c_wait(di, CW_H(0), CW_END)` | `self.wait({0: 'h'})` |
| `c_wait(di, CW_L(0), CW_END)` | `self.wait({0: 'l'})` |
| `c_wait(di, CW_R(0), CW_OR, CW_F(1), CW_END)` | `self.wait([{0: 'r'}, {1: 'f'}])` |
| `c_wait(di, CW_SKIP(100), CW_END)` | `self.wait({'skip': 100})` |
| `c_wait(di, CW_END)` | `self.wait()` |
| `di_samplenum(di)` | `self.samplenum` |
| `di_matched(di) & (1ULL << 0)` | `self.matched & (0b1 << 0)` |
| `c_pin(di, 0)` | `pins[0]`（wait 返回值） |
| `c_put(di, ss, es, out, cls, "A", "B")` | `self.put(ss, es, out, [cls, ['A', 'B']])` |
| `c_proto(di, ss, es, out, "CMD", C_U8(v), C_END)` | `self.putp(ss, es, ['CMD', v])` |
| `c_opt_int(di, "key", def)` | `self.options['key']` |
| `c_opt_str(di, "key", def)` | `self.options['key']` |
| `c_opt_bool(di, "key", 0)` | `self.options['key'] == 'yes'` |
| `c_has_ch(di, ch)` | `self.has_channel(ch)` |
| `c_samplerate(di)` | `self.samplerate` |
| `c_reg_out(di, SRD_OUTPUT_ANN, "id")` | `self.register(srd.OUTPUT_ANN)` |
| `c_reg_out(di, SRD_OUTPUT_PROTO, "id")` | `self.register(srd.OUTPUT_PYTHON)` |
| `c_reg_out(di, SRD_OUTPUT_BINARY, "id")` | `self.register(srd.OUTPUT_BINARY)` |

### 5.2 C 状态结构体 → Python 实例变量

```c
// C
C_DECODER_STATE(myproto, {
    enum myproto_state state;
    int bitcount;
    uint8_t databyte;
    uint64_t ss_byte;
    int out_ann;
});
```

```python
# Python
def reset(self):
    self.state = 'IDLE'
    self.bitcount = 0
    self.databyte = 0
    self.ss_byte = -1
    self.out_ann = None  # 在 start() 中设置
```

### 5.3 C switch/case → Python if/elif

```c
// C
switch (s->state) {
case STATE_IDLE:
    ret = c_wait(di, CW_R(0), CW_END);
    if (ret != SRD_OK) return;
    s->state = STATE_DATA;
    break;
case STATE_DATA:
    ...
}
```

```python
# Python
while True:
    if self.state == 'IDLE':
        pins = self.wait({0: 'r'})
        self.state = 'DATA'
    elif self.state == 'DATA':
        pins = self.wait({0: 'r'})
        ...
```

### 5.4 C decode_upper → Python decode(ss, es, data)

```c
// C — decode_upper 回调
static void myproto_decode_upper(di, ss, es, cmd, fields, n_fields)
{
    if (strcmp(cmd, "ADDRESS WRITE") == 0) {
        uint8_t addr = fields[0].u8;
        // 处理地址
    }
}
```

```python
# Python — decode(ss, es, data) 方法
def decode(self, ss, es, data):
    cmd, databyte = data
    self.ss, self.es = ss, es
    if cmd == 'ADDRESS WRITE':
        addr = databyte
        # 处理地址
```

---

## 阶段 6: 完整解码器示例

### 6.1 简单时钟同步协议示例

以下是一个完整的简单协议解码器示例（SPI-like 协议）：

```python
# __init__.py
'''
Simple synchronous serial protocol decoder.
CLK: clock line, DATA: data line, CS#: chip select (optional).
'''

from .pd import Decoder
```

```python
# pd.py
import sigrokdecode as srd

CLK, DATA, CS = range(3)

class Decoder(srd.Decoder):
    api_version = 3
    id = 'myspi'
    name = 'MySPI'
    longname = 'My Simple SPI'
    desc = 'Simple synchronous serial protocol(简单同步串行协议)'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['myspi']
    tags = ['Embedded/industrial']

    channels = (
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock line(时钟线)'},
        {'id': 'data', 'name': 'DATA', 'desc': 'Data line(数据线)'},
    )
    optional_channels = (
        {'id': 'cs', 'name': 'CS', 'desc': 'Chip select(片选)'},
    )
    options = (
        {'id': 'clock_edge', 'desc': 'Clock edge(时钟沿)', 'default': 'rising',
            'values': ('rising', 'falling')},
        {'id': 'bitorder', 'desc': 'Bit order(位序)', 'default': 'msb',
            'values': ('msb', 'lsb')},
        {'id': 'wordsize', 'desc': 'Word size(字宽)', 'default': 8,
            'values': (8, 16, 32)},
    )
    annotations = (
        ('data', 'Data byte'),
        ('bit', 'Data bit'),
        ('cs', 'CS transition'),
    )
    annotation_rows = (
        ('bits', 'Bits', (1,)),
        ('data', 'Data', (0,)),
        ('control', 'Control', (2,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.bitcount = 0
        self.databyte = 0
        self.ss_byte = -1
        self.cs_active = False

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.edge = 'r' if self.options['clock_edge'] == 'rising' else 'f'
        self.msb_first = self.options['bitorder'] == 'msb'
        self.wordsize = self.options['wordsize']

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def decode(self):
        has_cs = self.has_channel(CS)

        while True:
            # 等待时钟沿或 CS 变化
            if has_cs:
                (clk, data, cs) = self.wait([{CLK: self.edge}, {CS: 'e'}])

                if self.matched & (0b1 << 1):
                    # CS 边沿
                    self.cs_active = (cs == 0)
                    self.put(self.samplenum, self.samplenum, self.out_ann,
                             [2, ['CS %s' % ('active' if self.cs_active else 'idle'),
                                  'CS']])
                    continue

                if not self.cs_active:
                    continue
            else:
                (clk, data, cs) = self.wait({CLK: self.edge})

            # 记录第一个位的起始位置
            if self.bitcount == 0:
                self.ss_byte = self.samplenum

            # 采样数据位
            if self.msb_first:
                self.databyte = (self.databyte << 1) | data
            else:
                self.databyte |= (data << self.bitcount)

            # 输出位注解
            self.put(self.samplenum, self.samplenum, self.out_ann,
                     [1, ['%d' % data]])

            self.bitcount += 1

            # 字完成
            if self.bitcount >= self.wordsize:
                self.ss = self.ss_byte
                self.es = self.samplenum

                # 输出数据注解
                hex_str = '0x%0*X' % ((self.wordsize + 3) // 4, self.databyte)
                self.putx([0, ['Data: %s' % hex_str, hex_str]])

                # 输出协议数据
                self.put(self.ss_byte, self.samplenum, self.out_python,
                         ['DATA', self.databyte])

                # 重置
                self.bitcount = 0
                self.databyte = 0
```

### 6.2 堆叠解码器示例

以下是一个堆叠在 I²C 之上的设备解码器示例：

```python
# __init__.py
'''
MySensor temperature sensor decoder, stacks on top of i2c.
'''

from .pd import Decoder
```

```python
# pd.py
import sigrokdecode as srd

class Decoder(srd.Decoder):
    api_version = 3
    id = 'mysensor'
    name = 'MySensor'
    longname = 'My Sensor'
    desc = 'MySensor temperature sensor(温度传感器)'
    license = 'gplv2+'
    inputs = ['i2c']
    outputs = []
    tags = ['Sensor']

    options = (
        {'id': 'address', 'desc': 'I2C slave address(从地址)', 'default': 0x48},
    )
    annotations = (
        ('temperature', 'Temperature'),
        ('config', 'Configuration'),
        ('warnings', 'Warnings'),
    )
    annotation_rows = (
        ('data', 'Data', (0, 1)),
        ('warnings', 'Warnings', (2,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 'IDLE'
        self.databytes = []
        self.reg = 0x00

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.slave_addr = self.options['address']

    def putx(self, data):
        self.put(self.ss, self.es, self.out_ann, data)

    def putb(self, data):
        self.put(self.ss_block, self.es_block, self.out_ann, data)

    def warn(self, msg):
        self.putx([2, [msg]])

    def output_temperature(self):
        if len(self.databytes) < 2:
            return
        # 温度寄存器: 2 字节，高 9 位为温度值
        raw = (self.databytes[0] << 8) | self.databytes[1]
        raw >>= 7  # 9-bit resolution
        celsius = float(raw) * 0.5
        self.putb([0, ['Temperature: %.1f °C' % celsius, '%.1f°C' % celsius]])

    def decode(self, ss, es, data):
        cmd, databyte = data
        self.ss, self.es = ss, es

        if self.state == 'IDLE':
            if cmd != 'START':
                return
            self.state = 'GET SLAVE ADDR'

        elif self.state == 'GET SLAVE ADDR':
            if cmd in ('ADDRESS READ', 'ADDRESS WRITE'):
                if databyte != self.slave_addr:
                    self.warn('Warning: address 0x%02x does not match' % databyte)
                    self.state = 'IDLE'
                else:
                    self.state = 'READ REGS' if 'READ' in cmd else 'WRITE REGS'
                    self.databytes = []

        elif self.state == 'WRITE REGS':
            if cmd == 'DATA WRITE':
                if len(self.databytes) == 0:
                    self.reg = databyte  # 第一个字节是寄存器地址
                self.databytes.append(databyte)
            elif cmd == 'STOP':
                self.state = 'IDLE'

        elif self.state == 'READ REGS':
            if cmd == 'DATA READ':
                self.ss_block = self.ss if len(self.databytes) == 0 else self.ss_block
                self.es_block = self.es
                self.databytes.append(databyte)
            elif cmd == 'STOP':
                if self.reg == 0x00:  # 温度寄存器
                    self.output_temperature()
                self.state = 'IDLE'
```

---

## 阶段 7: 测试

### 7.1 测试数据目录结构

```
libsigrokdecode/tests/testdata/
└── <decoder_name>/
    └── default/
        ├── config.json    # 解码器配置
        └── input.bin      # 位打包逻辑信号数据
```

### 7.2 config.json 格式

```json
{
    "decoder": "myspi",
    "samplerate": 1000000,
    "num_channels": 3,
    "sample_count": 10000,
    "channels": {
        "clk": 0,
        "data": 1,
        "cs": 2
    },
    "options": {
        "clock_edge": "rising",
        "bitorder": "msb",
        "wordsize": 8
    }
}
```

### 7.3 input.bin 格式

位打包格式，每个通道数据独立存储：
- 每个通道占用 `ceil(sample_count / 8)` 字节
- 位序：LSB-first（byte 0 的 bit 0 = sample 0）
- 总文件大小 = `num_channels × ceil(sample_count / 8)` 字节

### 7.4 测试数据生成脚本

```python
#!/usr/bin/env python3
"""Generate test data for myspi decoder."""

import json
import math
import os

class BitstreamBuilder:
    def __init__(self, num_channels, sample_count, samplerate=1000000):
        self.num_channels = num_channels
        self.sample_count = sample_count
        self.samplerate = samplerate
        self.channels = [[0] * sample_count for _ in range(num_channels)]
        self.pos = 0

    def set_level(self, ch, level, duration_samples=1):
        for i in range(duration_samples):
            if self.pos + i < self.sample_count:
                self.channels[ch][self.pos + i] = 1 if level else 0
        self.pos += duration_samples

    def write_channels(self, channel_levels, duration_samples):
        for ch, level in channel_levels.items():
            for i in range(duration_samples):
                if self.pos + i < self.sample_count:
                    self.channels[ch][self.pos + i] = 1 if level else 0
        self.pos += duration_samples

    def set_idle(self, ch, level):
        for i in range(self.pos, self.sample_count):
            self.channels[ch][i] = 1 if level else 0

    def get_bitpacked(self):
        result = bytearray()
        bytes_per_channel = math.ceil(self.sample_count / 8)
        for ch in range(self.num_channels):
            packed = bytearray(bytes_per_channel)
            for i, val in enumerate(self.channels[ch]):
                if val:
                    packed[i // 8] |= (1 << (i % 8))
            result.extend(packed)
        return bytes(result)


class MySPIGenerator:
    def __init__(self, builder, clk_ch, data_ch, cs_ch):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.cs = cs_ch
        self.half_period = 5  # 10 samples per clock at 1MHz

    def write_byte(self, byte_val):
        for i in range(7, -1, -1):  # MSB first
            bit = (byte_val >> i) & 1
            self.builder.write_channels({self.clk: 0, self.data: bit, self.cs: 0},
                                        self.half_period)
            self.builder.write_channels({self.clk: 1, self.data: bit, self.cs: 0},
                                        self.half_period)


def main():
    samplerate = 1000000
    sample_count = 10000
    num_channels = 3

    builder = BitstreamBuilder(num_channels, sample_count, samplerate)
    builder.pos = 200

    # CS idle high
    builder.set_idle(2, 1)
    builder.write_channels({2: 1}, builder.pos)

    gen = MySPIGenerator(builder, clk_ch=0, data_ch=1, cs_ch=2)

    # CS active (low)
    builder.write_channels({2: 0}, 2)

    # Write 0xDE, 0xAD
    gen.write_byte(0xDE)
    gen.write_byte(0xAD)

    # CS inactive (high)
    builder.write_channels({2: 1}, 2)

    # Set idle
    builder.set_idle(0, 0)
    builder.set_idle(1, 0)
    builder.set_idle(2, 1)

    config = {
        "decoder": "myspi",
        "samplerate": samplerate,
        "num_channels": num_channels,
        "sample_count": sample_count,
        "channels": {"clk": 0, "data": 1, "cs": 2},
        "options": {
            "clock_edge": "rising",
            "bitorder": "msb",
            "wordsize": 8
        }
    }

    output_dir = "testdata/myspi/default"
    os.makedirs(output_dir, exist_ok=True)

    with open(os.path.join(output_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=2, ensure_ascii=False)
        f.write("\n")

    with open(os.path.join(output_dir, "input.bin"), "wb") as f:
        f.write(builder.get_bitpacked())

    print(f"Generated test data in {output_dir}")


if __name__ == "__main__":
    main()
```

### 7.5 运行测试

```bash
# 使用 decoder_test.exe 测试 Python 解码器
cd libsigrokdecode/tests
decoder_test.exe -d myspi -t testdata/myspi/default -f actual_py.json --python --generate-only

# 如果有对应 C 解码器，可对比输出
decoder_test.exe -d myspi_c -t testdata/myspi/default -f actual_c.json --generate-only

# 使用批量测试运行器
python run_all_tests.py --decoder myspi
```

### 7.6 测试场景覆盖

| 场景 | 目的 |
|------|------|
| 正常帧 | 验证基本解码 |
| 多字节连续传输 | 验证状态机连续运行 |
| CS 无效时忽略数据 | 验证 CS 守卫 |
| 全 0 数据 | 验证零值处理 |
| 全 1 数据 (0xFF) | 验证最大值处理 |
| 不同位序（MSB/LSB） | 验证选项处理 |
| 不同字宽（8/16/32） | 验证选项处理 |

---

## 质量检查清单

完成 Python 解码器后，逐项检查：

- [ ] `__init__.py` 包含模块文档字符串和 `from .pd import Decoder`
- [ ] `pd.py` 类继承 `srd.Decoder`，`api_version = 3`
- [ ] `id` 唯一，`inputs`/`outputs` 正确设置
- [ ] `channels`/`optional_channels` 定义与协议一致
- [ ] `options` 覆盖协议变体，有 `default` 值
- [ ] `annotations` 覆盖所有需要标注的协议字段/事件
- [ ] `annotation_rows` 合理分组注解类
- [ ] `__init__` 调用 `self.reset()`
- [ ] `reset()` 初始化所有状态变量
- [ ] `start()` 注册所有需要的输出
- [ ] `metadata()` 处理采样率（如需要）
- [ ] `decode()` 使用 `self.wait()` 条件等待，无逐采样空循环
- [ ] `self.matched` 使用 `&` 位运算检查（不是 `==`）
- [ ] `self.put()` 的注解类编号与 `annotations` 元组顺序一致
- [ ] 协议输出使用 `self.putp()` 或 `self.put(..., self.out_python, ...)`
- [ ] 堆叠解码器使用 `decode(self, ss, es, data)` 签名
- [ ] 无语法错误，可被 `import sigrokdecode` 正常加载
- [ ] 测试数据覆盖正常/错误/边界场景

---

## 附录: 常见陷阱

### 陷阱 1：wait() 返回值元组长度不匹配

```python
# 错误: 只有 2 个通道但解包 3 个值
(scl, sda, cs) = self.wait({0: 'r'})  # ValueError!

# 正确: 返回值数量 = len(channels) + len(optional_channels)
(scl, sda) = self.wait({0: 'r'})  # 2 通道
(scl, sda, cs) = self.wait({0: 'r'})  # 2 必选 + 1 可选 = 3 通道
```

### 陷阱 2：matched 使用 == 而非 &

```python
# 错误
if self.matched == 1:  # 可能多个条件同时匹配

# 正确
if self.matched & (0b1 << 0):  # 位运算检查
```

### 陷阱 3：忘记在 reset() 中初始化变量

```python
# 错误: 变量未在 reset() 中初始化
def decode(self):
    while True:
        if self.bitcount == 8:  # AttributeError: bitcount 未定义
            ...

# 正确
def reset(self):
    self.bitcount = 0  # 在 reset 中初始化
```

### 陷阱 4：堆叠解码器使用错误的 decode 签名

```python
# 错误: 堆叠解码器使用了无参数 decode
def decode(self):  # 不会接收到协议数据
    ...

# 正确: 堆叠解码器使用带参数 decode
def decode(self, ss, es, data):
    cmd, databyte = data
    ...
```

### 陷阱 5：annotations 顺序与 put() 中的类编号不匹配

```python
# annotations 定义
annotations = (
    ('start', 'Start'),    # 类 0
    ('data', 'Data'),      # 类 1
    ('error', 'Error'),    # 类 2
)

# 错误: put 中使用了错误的类编号
self.put(ss, es, self.out_ann, [0, ['Data']])  # 应该是 [1, ...]

# 正确
self.put(ss, es, self.out_ann, [1, ['Data']])
```

### 陷阱 6：逐采样循环导致超时

```python
# 错误: 逐采样循环，数据量大时严重超时
while True:
    self.wait()  # 每次前进一个采样
    if pins[0] == 1:
        break

# 正确: 使用条件等待
while True:
    pins = self.wait({0: 'r'})  # 直接跳转到上升沿
```

### 陷阱 7：可选通道未检查 has_channel

```python
# 错误: 直接访问未连接的可选通道
(cs,) = self.wait({2: 'e'})  # 如果 CS 未连接会出错

# 正确: 先检查通道是否连接
has_cs = self.has_channel(CS)
if has_cs:
    pins = self.wait([{CLK: 'r'}, {CS: 'e'}])
else:
    pins = self.wait({CLK: 'r'})
```
