# python-decoder-from-spec

## 元数据

- **Name**: python-decoder-from-spec
- **Description**: 从协议规范/数据手册出发，独立创建 Python 协议解码器
- **Trigger**: 用户要求支持新协议、从协议规范/数据手册创建 Python 解码器、需要为新协议或私有协议快速实现解码器原型

---

## 概述

本技能指导 LLM 从零开始，仅依据协议规范文档（数据手册、协议标准、时序图），创建一个完整的、可直接运行的 Python 协议解码器。整个过程分为 5 个阶段，每个阶段都有结构化的模板和决策树，确保不遗漏任何协议细节。

**核心原则**：
- 协议规范是唯一真相来源，不是 C 解码器
- 每个设计决策都必须追溯到协议规范中的具体条款
- 状态机必须覆盖协议规范中描述的所有状态和转换
- 测试数据必须从协议时序图精确生成

**Python 解码器优势**（相较 C 解码器）：
- 开发更快速、迭代更灵活，适合原型开发和新协议快速验证
- 无需编译，修改后立即生效
- 可使用 Python 丰富的标准库（collections、struct、binascii 等）
- 适合作为 C 解码器的参考实现（后续可用 `c-decoder-generator` 技能翻译为 C）

**参考资源**：
- 协议分析方法论（语言无关）：参考 `doc/c-decoder-from-spec-guide.md`
- Python 解码器 API 参考：参考 `python-decoder-generator.md` 技能
- 测试方法：参考 `doc/c-decoder-testing.md`

---

## 阶段 1: 协议分析

目标：从协议规范中提取所有解码器需要的信息，填入结构化模板。

### 1.1 信号线表格模板

从协议规范中提取所有信号线定义，填入下表：

| 信号名 | 方向 | 有效电平 | 通道类型 | 描述 |
|--------|------|----------|----------|------|
| CLK | 主→从 | 上升沿有效 | 时钟线（SCLK 类型） | 时钟线 |
| DATA | 双向 | 高电平=1 | 串行数据线（SDATA 类型） | 数据线 |
| CS# | 主→从 | 低电平有效 | 控制信号线（COMMON 类型） | 片选线 |

**Python 通道类型说明**（无显式类型枚举，通过 `desc` 描述）：
- 时钟信号线：`'desc': 'Clock line(时钟线)'`
- 串行数据线：`'desc': 'Data line(数据线)'`
- 控制信号线：`'desc': 'Chip select(片选)'` 等控制类信号

**填写规则**：
- 必选通道放入 `channels` 元组
- 可选通道放入 `optional_channels` 元组（允许只接一部分信号）
- 每个通道是 dict，包含 `id`、`name`、`desc` 三个键
- 通道在元组中的顺序即为其索引（0-based），`self.wait()` 条件用此索引引用

### 1.2 帧格式图模板

用 ASCII 时序图描述协议帧格式：

```
        ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐   ┌───┐
CLK ────┘   └───┘   └───┘   └───┘   └───┘   └───┘   └───┘   └───┘   └───
        ┌───────────────────────────────────────┐
CS# ────┘                                       └──────────────────────────

DATA ───┤ D7 ├─┤ D6 ├─┤ D5 ├─┤ D4 ├─┤ D3 ├─┤ D2 ├─┤ D1 ├─┤ D0 ├─────────
```

帧字段表格：

| 字段名 | 位宽 | 字节序 | 取值范围 | 描述 |
|--------|------|--------|----------|------|
| ADDRESS | 7 bit | MSB first | 0x00-0x7F | 设备地址 |
| R/W | 1 bit | - | 0=写, 1=读 | 读写方向 |
| DATA | 8 bit | MSB first | 0x00-0xFF | 数据字节 |
| ACK/NACK | 1 bit | - | 0=ACK, 1=NACK | 应答位 |
| CRC | 8 bit | MSB first | - | 校验值 |

### 1.3 状态机图模板

用 ASCII 状态图描述协议状态机：

```
                    ┌──────────────┐
                    │  FIND_START  │◄─────────────────────┐
                    └──────┬───────┘                      │
                           │ START 条件                    │ STOP 条件
                           ▼                              │
                    ┌──────────────┐                      │
              ┌────►│ FIND_ADDRESS │                      │
              │     └──────┬───────┘                      │
              │            │ 8 bit 收完                    │
              │            ▼                              │
              │     ┌──────────────┐                      │
              │     │   FIND_ACK   │                      │
              │     └──────┬───────┘                      │
              │            │ ACK 收到                      │
              │            ▼                              │
              │     ┌──────────────┐── REPEAT START ──────┘
              └─────│  FIND_DATA   │
                    └──────┬───────┘
                           │ STOP 条件
                           ▼
                    ┌──────────────┐
                    │  FIND_START  │
                    └──────────────┘
```

状态转换表：

| 当前状态 | 条件 | 目标状态 | 动作 |
|----------|------|----------|------|
| FIND_START | SCL高+SDA下降沿 | FIND_ADDRESS | 记录起始位置，初始化位计数 |
| FIND_ADDRESS | SCL上升沿×8 | FIND_ACK | 采样8位地址，输出地址注解 |
| FIND_ACK | SCL上升沿 | FIND_DATA | 采样ACK/NACK，输出应答注解 |
| FIND_DATA | SCL上升沿×8 | FIND_ACK | 采样8位数据，输出数据注解 |
| FIND_DATA | SCL高+SDA上升沿 | FIND_START | 输出停止注解，重置状态 |
| FIND_DATA | SCL高+SDA下降沿 | FIND_ADDRESS | 输出重复起始注解 |

### 1.4 校验算法模板

| 参数 | 值 |
|------|-----|
| 算法名称 | CRC-8/CRC-16/CRC-32/校验和 |
| 多项式 | 0x... (十六进制) |
| 初始值 | 0x... |
| 输入反射(refin) | 是/否 |
| 输出反射(refout) | 是/否 |
| 异或输出(xorout) | 0x... |
| 计算范围 | 从字段A到字段B（不含校验字段本身） |

Python 代码模板：

```python
def compute_crc8(data: bytes) -> int:
    """CRC-8 校验计算"""
    crc = 0x00  # 初始值
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF  # 多项式
            else:
                crc = (crc << 1) & 0xFF
    return crc ^ 0x00  # xorout
```

**Python 优势**：可使用标准库简化 CRC 实现：
```python
# 使用 binascii（部分场景）
import binascii
crc = binascii.crc32(data) & 0xFFFFFFFF
```

### 1.5 选项列表模板

| 选项名 | 类型 | 默认值 | 可选值 | 描述 |
|--------|------|--------|--------|------|
| bitrate | int | 1000000 | - | 比特率(Hz) |
| bit_order | str | "msb" | "msb", "lsb" | 位序 |
| parity | str | "none" | "none", "even", "odd" | 校验模式 |
| cs_polarity | str | "active-low" | "active-low", "active-high" | 片选极性 |

**Python 选项 dict 字段**：
- `id`：选项标识符
- `desc`：显示描述（建议中英双语）
- `default`：默认值
- `values`（可选）：可选值元组，提供下拉选择

---

## 阶段 2: 解码器设计决策树

根据阶段 1 的分析结果，通过决策树确定解码器的架构。

### 2.1 时钟采样决策

```
协议有时钟线？
├── 是 → 时钟边沿采样
│   ├── 上升沿采样 → self.wait({CLK: 'r'})
│   └── 下降沿采样 → self.wait({CLK: 'f'})
└── 否 → 数据边沿触发 / 定时采样
    ├── 数据边沿触发 → self.wait({DATA: 'e'})
    └── 定时采样 → self.wait({'skip': bit_width_samples}) + 读取 pins
```

### 2.2 片选/使能决策

```
协议有片选/使能线？
├── 是 → CS 有效时处理，CS 无效时等待
│   ├── CS 低电平有效 → self.wait({CS: 'l'}) 等待 CS 变低
│   └── CS 高电平有效 → self.wait({CS: 'h'}) 等待 CS 变高
└── 否 → 无需处理
```

### 2.3 超时决策

```
协议有帧间超时或位间超时？
├── 是 → 使用 'skip' 条件添加超时
│   └── (pins) = self.wait([{CLK: 'r'}, {'skip': max_samples}])
│       检查 self.matched & (0b1 << N) 判断是时钟还是超时
└── 否 → 无需超时
```

### 2.4 堆叠解码器决策

```
解码器需要向上堆叠（被其他解码器使用）？
├── 是 → 实现 OUTPUT_PYTHON 协议输出
│   ├── 注册输出 → self.out_python = self.register(srd.OUTPUT_PYTHON)
│   ├── 在关键事件输出 → self.put(ss, es, self.out_python, ['CMD', val])
│   └── outputs 元组声明协议 id → outputs = ['myproto']
└── 否 → 无需协议输出

解码器需要向下堆叠（依赖其他解码器的输出）？
├── 是 → inputs 设为上游解码器的 output 协议 id
│   ├── inputs = ['i2c']  # 输入来自 i2c 解码器
│   ├── 实现 decode(self, ss, es, data) 签名（注意不是 decode(self)）
│   └── data 参数为上游 self.put() 输出的列表，如 ['ADDRESS READ', 0x50]
└── 否 → inputs = ['logic']  # 直接输入逻辑信号
```

**关键区别**：
- 直接处理逻辑信号：`def decode(self):` — 用 `self.wait()` 获取引脚
- 堆叠在上游解码器上：`def decode(self, ss, es, data):` — 用 `data` 接收协议包

### 2.5 多条件组合决策

```
同一状态需要等待多个可能的条件？
├── 是 → 使用条件列表（list of dicts）
│   └── (pins) = self.wait([{0: 'r'}, {0: 'h', 1: 'f'}, {0: 'h', 1: 'r'}])
│       条件组编号从 0 开始，通过 self.matched & (0b1 << N) 判断匹配
│       - 条件0: SCL 上升沿
│       - 条件1: SCL 高 + SDA 下降沿 (START)
│       - 条件2: SCL 高 + SDA 上升沿 (STOP)
└── 否 → 单条件等待
    └── (pins) = self.wait({0: 'r'})
```

**关键规则**：
- 单 dict 表示 AND（同一时刻满足所有条件）
- list of dicts 表示 OR（任一 dict 满足即可）
- 绝不在同一状态中顺序调用多个 `self.wait()`，必须用 list 合并

### 2.6 注解类设计

Python 解码器的注解通过类属性 `annotations` 元组定义，每个条目是 `(id, label)` 二元组：

```python
class Decoder(srd.Decoder):
    annotations = (
        ('start',   'Start condition'),   # 索引 0
        ('stop',    'Stop condition'),    # 索引 1
        ('address', 'Address byte'),      # 索引 2
        ('data',    'Data byte'),         # 索引 3
        ('ack',     'Acknowledge'),       # 索引 4
        ('error',   'Protocol error'),    # 索引 5
        ('warning', 'Protocol warning'),  # 索引 6
    )
```

**注解输出格式**：`self.put(ss, es, self.out_ann, [class_index, [text1, text2, text3]])`
- `class_index`：注解类索引（对应 annotations 元组位置）
- 文本列表支持 1-3 个变体：`[long, mid, short]`（长/中/短），前端按缩放级别选择

### 2.7 注解行设计

通过 `annotation_rows` 元组定义注解在 UI 中的分行显示：

```python
class Decoder(srd.Decoder):
    annotation_rows = (
        ('control', 'Control', (0, 1, 4, 5)),         # 起始/停止/ACK/错误 → 控制行
        ('data',    'Data',    (2, 3)),               # 地址/数据 → 数据行
        ('warning', 'Warning', (6,)),                 # 警告 → 警告行
    )
```

每个条目是 `(row_id, row_label, tuple_of_class_indices)`。

### 2.8 状态变量设计

Python 不需要像 C 那样预定义结构体，状态变量直接作为实例属性在 `reset()` 中初始化：

```python
def reset(self):
    self.samplerate = None
    self.state = 'FIND START'   # 字符串状态名（Python 惯例）
    self.bitcount = 0
    self.databyte = 0
    self.ss = self.es = self.ss_byte = -1
    self.wr = -1
    self.bits = []              # 列表自动管理，无需预分配
    self.packet_data = []       # 复杂数据结构直接用 list/deque
```

**Python 优势**：
- 无需 `C_DECODER_STATE` 宏，无需 `calloc`/`free`
- 无需手动管理内存，GC 自动回收
- 列表/字典自动增长，无需预分配容量
- 状态名用字符串（如 `'FIND ADDRESS'`）或枚举均可，字符串更可读

### 2.9 输出注册设计

在 `start()` 方法中注册所有输出通道：

```python
def start(self):
    self.out_ann = self.register(srd.OUTPUT_ANN)
    self.out_python = self.register(srd.OUTPUT_PYTHON)   # 如需向上堆叠
    self.out_binary = self.register(srd.OUTPUT_BINARY)   # 如需二进制输出
    self.out_meta = self.register(srd.OUTPUT_META,       # 如需元数据输出
        meta=(int, 'Bitrate', 'Bitrate from Start to Stop'))

    # 读取选项
    self.msb_first = (self.options['bit_order'] == 'msb')
    baud = self.options.get('baudrate', 9600)
    if self.samplerate and baud > 0:
        self.bit_width = self.samplerate / baud
```

---

## 阶段 3: 解码器实现

按以下顺序逐步实现解码器。

### 3.1 完整骨架代码模板

以下是一个通用 Python 解码器的完整骨架，包含两个文件：

#### `__init__.py`

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

#### `pd.py`

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

# 通道索引（语义化常量，避免代码中出现魔数）
CLK = 0
DATA = 1
# CS = 2  # 如有片选

# 注解类索引（可与 annotations 元组顺序对应）
ANN_START = 0
ANN_ADDRESS = 1
ANN_DATA = 2
ANN_ACK = 3
ANN_ERROR = 4

# 协议命令 → 注解类/标签映射（用于 OUTPUT_PYTHON 输出，便于上层堆叠解码器解析）
proto = {
    'START':   [ANN_START,   'Start',   'S'],
    'ADDRESS': [ANN_ADDRESS, 'Address', 'A'],
    'DATA':    [ANN_DATA,    'Data',    'D'],
    'ACK':     [ANN_ACK,     'ACK',     'A'],
    'NACK':    [ANN_ERROR,   'NACK',    'N'],
}

class Decoder(srd.Decoder):
    api_version = 3
    id = 'myproto'
    name = 'MyProto'
    longname = 'My Protocol'
    desc = 'My protocol decoder description.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['myproto']
    tags = ['Embedded/industrial']

    # 必选通道
    channels = (
        {'id': 'clk',  'name': 'CLK',  'desc': 'Clock line(时钟线)'},
        {'id': 'data', 'name': 'DATA', 'desc': 'Data line(数据线)'},
    )
    # 可选通道（如有）
    # optional_channels = (
    #     {'id': 'cs', 'name': 'CS', 'desc': 'Chip select(片选)'},
    # )

    # 选项
    options = (
        {'id': 'bit_order', 'desc': 'Bit order(位序)', 'default': 'msb',
            'values': ('msb', 'lsb')},
    )

    # 注解类
    annotations = (
        ('start',   'Start condition'),
        ('address', 'Address byte'),
        ('data',    'Data byte'),
        ('ack',     'ACK'),
        ('error',   'Protocol error'),
    )

    # 注解行
    annotation_rows = (
        ('control', 'Control', (0, 3, 4)),
        ('data',    'Data',    (1, 2)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.state = 'IDLE'
        self.bitcount = 0
        self.databyte = 0
        self.ss = self.es = self.ss_byte = -1
        self.msb_first = True
        self.out_ann = None
        self.out_python = None

    def metadata(self, key, value):
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = value

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.out_python = self.register(srd.OUTPUT_PYTHON)
        self.msb_first = (self.options['bit_order'] == 'msb')

    # ===== 辅助方法：输出注解/协议 =====
    def putx(self, data):
        """输出注解到当前 ss..es 区间"""
        self.put(self.ss, self.es, self.out_ann, data)

    def putp(self, data):
        """输出协议包到当前 ss..es 区间"""
        self.put(self.ss, self.es, self.out_python, data)

    # ===== 主状态机 =====
    def decode(self):
        while True:
            if self.state == 'IDLE':
                # 等待片选有效（如有）或起始条件
                self.wait({CLK: 'r'})  # 示例：等待时钟上升沿
                self.state = 'ADDRESS'
                self.bitcount = 0
                self.databyte = 0
                self.ss_byte = self.samplenum

            elif self.state == 'ADDRESS':
                # 等待时钟上升沿采样
                (clk, data) = self.wait({CLK: 'r'})
                if self.msb_first:
                    self.databyte = (self.databyte << 1) | data
                else:
                    self.databyte |= (data << self.bitcount)
                self.bitcount += 1

                if self.bitcount == 1:
                    self.ss_byte = self.samplenum

                if self.bitcount >= 8:
                    self.ss, self.es = self.ss_byte, self.samplenum
                    val_str = '0x%02X' % self.databyte
                    self.putx([ANN_ADDRESS, ['Address: ' + val_str, val_str, val_str]])
                    self.putp(['ADDRESS', self.databyte])
                    self.bitcount = 0
                    self.databyte = 0
                    self.state = 'DATA'

            elif self.state == 'DATA':
                (clk, data) = self.wait({CLK: 'r'})
                if self.msb_first:
                    self.databyte = (self.databyte << 1) | data
                else:
                    self.databyte |= (data << self.bitcount)
                self.bitcount += 1

                if self.bitcount == 1:
                    self.ss_byte = self.samplenum

                if self.bitcount >= 8:
                    self.ss, self.es = self.ss_byte, self.samplenum
                    val_str = '0x%02X' % self.databyte
                    self.putx([ANN_DATA, ['Data: ' + val_str, val_str, val_str]])
                    self.putp(['DATA', self.databyte])
                    self.bitcount = 0
                    self.databyte = 0
                    self.state = 'ACK'

            elif self.state == 'ACK':
                (clk, data) = self.wait({CLK: 'r'})
                self.ss, self.es = self.samplenum, self.samplenum
                if data == 0:
                    self.putx([ANN_ACK, ['ACK', 'A']])
                    self.putp(['ACK', None])
                else:
                    self.putx([ANN_ERROR, ['NACK', 'N']])
                    self.putp(['NACK', None])
                self.state = 'IDLE'
```

### 3.2 `start()` 方法详解

`start()` 在每次解码开始前调用，负责：

1. **注册输出通道**：每种输出类型都需要 `self.register()`
2. **读取选项**：直接从 `self.options` dict 读取
3. **初始化非零默认值**：`reset()` 只做零值初始化

```python
def start(self):
    # 1. 注册输出
    self.out_ann = self.register(srd.OUTPUT_ANN)
    self.out_python = self.register(srd.OUTPUT_PYTHON)
    self.out_binary = self.register(srd.OUTPUT_BINARY)
    self.out_meta = self.register(srd.OUTPUT_META,
        meta=(int, 'Bitrate', 'Bitrate from Start to Stop'))

    # 2. 读取选项
    self.msb_first = (self.options['bit_order'] == 'msb')
    baud = self.options.get('baudrate', 9600)
    if self.samplerate and baud > 0:
        self.bit_width = self.samplerate / baud

    # 3. 非零默认值
    self.wr = -1
```

### 3.3 `decode()` 方法详解

`decode()` 是核心状态机，使用 `self.wait()` + 引脚返回值模式：

#### 直接逻辑输入：`def decode(self):`

```python
def decode(self):
    while True:
        if self.state == 'IDLE':
            # 等待起始条件
            (clk, data) = self.wait({CLK: 'h', DATA: 'f'})
            # 处理起始条件
            self.state = 'ADDRESS'
        elif self.state == 'ADDRESS':
            # 等待时钟上升沿采样数据
            (clk, data) = self.wait({CLK: 'r'})
            # data 即为采样到的数据位
            ...
```

#### 堆叠解码器输入：`def decode(self, ss, es, data):`

```python
def decode(self, ss, es, data):
    # data 是上游解码器 put() 输出的列表，如 ['ADDRESS READ', 0x50]
    cmd, pdata = data[0], data[1] if len(data) > 1 else None

    if cmd == 'START':
        self.state = 'ADDRESS'
        self.ss = ss
    elif cmd == 'ADDRESS READ':
        self.address = pdata
        self.put(ss, es, self.out_ann, [ANN_ADDRESS, ['Addr: 0x%02X' % pdata]])
    elif cmd == 'DATA READ':
        self.put(ss, es, self.out_ann, [ANN_DATA, ['Data: 0x%02X' % pdata]])
    # 注意：堆叠解码器不用 while True + self.wait()，
    # 每次调用处理一个上游协议包
```

**关键 API 速查**：

| API | 用途 | 示例 |
|-----|------|------|
| `self.wait({ch: 'r'})` | 等待条件 | `self.wait({0: 'r'})` |
| `self.wait({...})` 返回值 | 获取引脚当前值 | `(clk, data) = self.wait({0: 'r'})` |
| `self.samplenum` | 当前采样位置 | `ss = self.samplenum` |
| `self.matched` | 条件匹配位掩码 | `if self.matched & (0b1 << 0):` |
| `self.put(ss, es, out, data)` | 输出注解/协议 | `self.put(ss, es, self.out_ann, [0, ['Text']])` |
| `self.has_channel(idx)` | 检查可选通道是否连接 | `if self.has_channel(1):` |
| `self.options[key]` | 读取选项值 | `baud = self.options['baudrate']` |

**`self.wait()` 条件字符速查**：

| 字符 | 含义 | C 等价 |
|------|------|--------|
| `'h'` | 通道高电平 | `CW_H(ch)` |
| `'l'` | 通道低电平 | `CW_L(ch)` |
| `'r'` | 通道上升沿 | `CW_R(ch)` |
| `'f'` | 通道下降沿 | `CW_F(ch)` |
| `'e'` | 通道任意边沿 | `CW_E(ch)` |
| `'n'` | 通道无变化 | `CW_N(ch)` |
| `{'skip': n}` | 跳过 n 个采样 | `CW_SKIP(n)` |

**条件组合规则**：
- 单 dict `{0: 'r', 1: 'f'}` = AND（同时满足）
- list of dicts `[{0: 'r'}, {1: 'f'}]` = OR（任一满足）
- `self.matched` 的 bit N 对应 list 中第 N 个 dict（0-based）

### 3.4 校验计算实现

如果协议包含 CRC 或校验和，在数据收集完成后计算并验证：

```python
def compute_crc8(self, data: bytes) -> int:
    """CRC-8 校验计算"""
    crc = 0x00  # 初始值
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF  # 多项式
            else:
                crc = (crc << 1) & 0xFF
    return crc ^ 0x00  # xorout

# 在 decode() 中使用
if self.compute_crc8(bytes(self.packet)) == self.crc_byte:
    self.put(crc_ss, crc_es, self.out_ann, [ANN_CRC_OK, ['CRC OK', 'OK']])
else:
    self.put(crc_ss, crc_es, self.out_ann, [ANN_CRC_ERR, ['CRC ERROR', 'ERR']])
```

### 3.5 堆叠解码器实现（接收上游协议包）

如果解码器需要接收上游解码器的协议输出（如 lm75 堆叠在 i2c 上）：

```python
class Decoder(srd.Decoder):
    api_version = 3
    id = 'mydevice'
    name = 'MyDevice'
    # ...
    inputs = ['i2c']  # 输入来自 i2c 解码器
    outputs = ['mydevice']

    annotations = (
        ('address', 'Register address'),
        ('data',    'Register data'),
    )

    def reset(self):
        self.state = 'GET ADDRESS'
        self.address = None
        self.data = []

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)

    def decode(self, ss, es, data):
        # data 是 i2c 解码器 put() 输出的列表
        cmd = data[0]
        pdata = data[1] if len(data) > 1 else None

        if self.state == 'GET ADDRESS':
            if cmd in ('ADDRESS READ', 'ADDRESS WRITE'):
                self.address = pdata
                self.put(ss, es, self.out_ann,
                    [0, ['Register: 0x%02X' % pdata, '0x%02X' % pdata]])
                self.state = 'GET DATA'
        elif self.state == 'GET DATA':
            if cmd in ('DATA READ', 'DATA WRITE'):
                self.data.append(pdata)
                self.put(ss, es, self.out_ann,
                    [1, ['Data: 0x%02X' % pdata, '0x%02X' % pdata]])
            elif cmd == 'STOP':
                # 处理完整事务
                self.state = 'GET ADDRESS'
                self.data = []
```

**关键区别**：
- `decode(self)` 用 `while True` + `self.wait()` 持续运行
- `decode(self, ss, es, data)` 每次调用处理一个上游包，不用循环

### 3.6 可选通道处理

使用 `optional_channels` 声明可选通道，运行时用 `self.has_channel()` 检查：

```python
class Decoder(srd.Decoder):
    channels = (
        {'id': 'clk', 'name': 'CLK', 'desc': 'Clock line'},
    )
    optional_channels = (
        {'id': 'miso', 'name': 'MISO', 'desc': 'Master In Slave Out'},
        {'id': 'mosi', 'name': 'MOSI', 'desc': 'Master Out Slave In'},
    )

    def decode(self):
        have_miso = self.has_channel(1)
        have_mosi = self.has_channel(2)

        while True:
            (clk,) = self.wait({0: 'r'})
            if have_miso or have_mosi:
                # wait 返回所有通道（含未连接的可选通道，值为 0）
                (clk, miso, mosi) = self.wait({0: 'r'})
                if have_miso:
                    self.handle_miso(miso)
                if have_mosi:
                    self.handle_mosi(mosi)
```

### 3.7 ATK 颜色注解（前端自动着色）

如需支持前端 ATK（Auto-Tint Kolor）颜色标记，输出特殊注解类：

```python
annotations = (
    ...,
    ('atk-data-point',  'ATK Data point'),    # 索引 N
    ('atk-rising-edge', 'ATK Rising edge'),   # 索引 N+1
)

def decode(self):
    # 在起始位置输出颜色标记（仅 1 个采样点）
    self.put(self.samplenum, self.samplenum, self.out_ann,
        [N, ["color:#4edc44"]])
    self.put(self.samplenum, self.samplenum, self.out_ann,
        [N+1, ["color:#4edc44"]])

    while True:
        ...
```

---

## 阶段 4: 测试数据生成

为解码器生成精确的测试数据，确保覆盖正常/错误/边界场景。

### 4.1 BitstreamBuilder 类

使用项目已有的 `BitstreamBuilder` 类（位于 `libsigrokdecode/tests/fuzzers/base.py`）：

```python
import math
import json
import os

class BitstreamBuilder:
    def __init__(self, num_channels, sample_count, samplerate=1000000):
        self.num_channels = num_channels
        self.sample_count = sample_count
        self.samplerate = samplerate
        self.channels = [[0] * sample_count for _ in range(num_channels)]
        self.pos = 0

    def set_pos(self, pos):
        self.pos = pos

    def get_pos(self):
        return self.pos

    def set_level(self, ch, level, duration_samples=1):
        if duration_samples == 0:
            if self.pos < self.sample_count:
                self.channels[ch][self.pos] = 1 if level else 0
            return
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
```

### 4.2 协议帧生成函数模板

以时钟同步协议为例：

```python
class MyProtoGenerator:
    def __init__(self, builder, clk_ch, data_ch, cs_ch=-1):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.cs = cs_ch
        self.half_period = 5  # 10 samples per clock at 1MHz

    def _clock_pulse(self, data_bit):
        """生成一个时钟脉冲，数据在上升沿有效"""
        # SCL low, data setup
        self.builder.write_channels({self.clk: 0, self.data: data_bit}, self.half_period)
        # SCL high, data hold
        self.builder.write_channels({self.clk: 1, self.data: data_bit}, self.half_period)

    def _write_byte_msb(self, byte_val):
        """MSB-first 发送一个字节"""
        for i in range(7, -1, -1):
            self._clock_pulse((byte_val >> i) & 1)

    def select(self):
        if self.cs >= 0:
            self.builder.set_level(self.cs, 0, 1)  # CS low (active)
            self.builder.pos += 2  # settle time

    def deselect(self):
        if self.cs >= 0:
            self.builder.set_level(self.cs, 1, 1)  # CS high (inactive)

    def write_frame(self, address, data_bytes):
        """生成完整协议帧"""
        self.select()
        self._write_byte_msb(address)
        for b in data_bytes:
            self._write_byte_msb(b)
        self.deselect()
```

### 4.3 config.json 生成模板

```python
def generate_config(decoder_id, num_channels, sample_count, samplerate,
                    channels_map, options=None):
    config = {
        "decoder": decoder_id,
        "samplerate": samplerate,
        "num_channels": num_channels,
        "sample_count": sample_count,
        "channels": channels_map,
    }
    if options:
        config["options"] = options
    return config
```

示例 config.json：

```json
{
  "decoder": "myproto",
  "samplerate": 1000000,
  "num_channels": 3,
  "sample_count": 10000,
  "channels": {
    "clk": 0,
    "data": 1,
    "cs": 2
  },
  "options": {
    "bit_order": "msb"
  }
}
```

### 4.4 完整测试数据生成脚本模板

```python
#!/usr/bin/env python3
"""Generate test data for myproto Python decoder."""

import json
import math
import os

class BitstreamBuilder:
    # ... (同 4.1)

class MyProtoGenerator:
    # ... (同 4.2)

def samples_to_bitpacked(channel_data):
    num_bytes = math.ceil(len(channel_data) / 8)
    result = bytearray(num_bytes)
    for i, val in enumerate(channel_data):
        if val:
            result[i // 8] |= (1 << (i % 8))
    return bytes(result)

def write_test_data(output_dir, builder, config):
    os.makedirs(output_dir, exist_ok=True)
    with open(os.path.join(output_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=2)
    with open(os.path.join(output_dir, "input.bin"), "wb") as f:
        for ch_idx in range(builder.num_channels):
            f.write(samples_to_bitpacked(builder.channels[ch_idx]))

def generate_normal_frame():
    """场景1: 正常帧"""
    sample_count = 10000
    num_channels = 3
    samplerate = 1000000
    builder = BitstreamBuilder(num_channels, sample_count, samplerate)
    builder.pos = 100  # 空闲起始

    gen = MyProtoGenerator(builder, clk_ch=0, data_ch=1, cs_ch=2)
    gen.write_frame(0x50, [0x00, 0xBE])

    config = {
        "decoder": "myproto",
        "samplerate": samplerate,
        "num_channels": num_channels,
        "sample_count": sample_count,
        "channels": {"clk": 0, "data": 1, "cs": 2},
    }
    write_test_data("testdata/myproto/default", builder, config)

def generate_error_frame():
    """场景2: CRC 错误帧"""
    # ... 生成故意错误的帧数据

def generate_boundary_frame():
    """场景3: 边界条件 - 全0/全1数据"""
    # ... 生成全0和全1数据

if __name__ == "__main__":
    generate_normal_frame()
    generate_error_frame()
    generate_boundary_frame()
    print("All test data generated.")
```

### 4.5 测试场景覆盖清单

| 场景类别 | 具体场景 | 目的 |
|----------|----------|------|
| 正常帧 | 标准地址+数据帧 | 验证基本解码功能 |
| 正常帧 | 多字节连续传输 | 验证状态机连续运行 |
| 正常帧 | 重复起始条件 | 验证状态机回到地址状态 |
| 错误帧 | CRC/校验错误 | 验证错误检测和注解 |
| 错误帧 | 格式错误（帧长度异常） | 验证异常处理 |
| 错误帧 | 超时（帧间间隔过长） | 验证超时检测 |
| 边界条件 | 全0数据 | 验证零值处理 |
| 边界条件 | 全1数据(0xFF) | 验证最大值处理 |
| 边界条件 | 最小帧长度 | 验证最小帧解析 |
| 边界条件 | 最大帧长度 | 验证缓冲区不溢出 |
| 可选通道 | 只连接必选通道 | 验证 optional_channels 处理 |
| 选项组合 | 不同位序/极性组合 | 验证选项读取与应用 |

---

## 阶段 5: 验证与调试

### 5.1 路径 A: 有 C 参考解码器

如果项目中已有对应协议的 C 解码器：

```bash
# 1. 运行测试（Python 和 C 解码器都跑）
cd libsigrokdecode/tests
python run_all_tests.py --decoder myproto

# 2. 比较输出
# 实际输出: testdata/myproto/default/actual_py.json
# 期望输出: testdata/myproto/default/expected_c.json

# 3. 查看差异
python -c "
import json
with open('testdata/myproto/default/actual_py.json') as f: py = json.load(f)
with open('testdata/myproto/default/expected_c.json') as f: c = json.load(f)
print(f'Py annotations: {len(py.get(\"annotations\", []))}')
print(f'C annotations:  {len(c.get(\"annotations\", []))}')
"
```

### 5.2 路径 B: 无 C 参考解码器（独立验证）

当没有 C 参考解码器时，需要手动验证：

**步骤 1: 使用 decoder_test.exe 运行 Python 解码器**

```bash
# Python 解码器用 --python 标志
build.dir/decoder_test.exe -d myproto -t testdata/myproto/default -f testdata/myproto/default/actual_py.json --python
```

**步骤 2: 手动验证注解与协议规范一致**

检查 actual_py.json 中的每个注解：
- 帧边界（start_sample, end_sample）是否与协议时序图一致
- 字段值是否与测试数据中的预期值一致
- 校验结果是否正确
- 状态转换是否完整

**步骤 3: 使用已知捕获数据验证**

如果有逻辑分析仪捕获的真实数据：
1. 将真实数据转换为 input.bin 格式
2. 运行解码器
3. 与其他工具（Wireshark、PulseView）对比

**步骤 4: 在 PXView 中加载验证**

1. 将解码器目录放入 `libsigrokdecode/decoders/`
2. 启动 PXView，加载真实采集数据
3. 添加解码器，观察注解是否正确

**步骤 5: 编写协议级验证脚本**

```python
#!/usr/bin/env python3
"""Protocol-level verification for myproto decoder output."""

import json
import sys

def verify_annotations(actual_json_path, expected_frames):
    """验证解码器输出与协议规范一致。

    expected_frames: list of dicts with keys:
        - type: 'address' or 'data'
        - value: expected byte value
        - ann_class: expected annotation class index
    """
    with open(actual_json_path) as f:
        data = json.load(f)

    annotations = data.get('annotations', [])
    errors = []

    for i, frame in enumerate(expected_frames):
        matching = [a for a in annotations
                    if a.get('ann_class') == frame['ann_class']]
        if not matching:
            errors.append(f"Frame {i}: No annotation with class {frame['ann_class']}")
            continue

        found = False
        for ann in matching:
            texts = ann.get('texts', [])
            if texts and frame['value'] is not None:
                val_str = f"0x{frame['value']:02X}"
                if val_str in texts[0] or str(frame['value']) in texts[0]:
                    found = True
                    break
        if not found:
            errors.append(f"Frame {i}: Expected value 0x{frame['value']:02X} not found")

    if errors:
        print("VERIFICATION FAILED:")
        for e in errors:
            print(f"  - {e}")
        return False
    else:
        print(f"VERIFICATION PASSED: {len(expected_frames)} frames verified")
        return True

if __name__ == "__main__":
    expected = [
        {"type": "address", "value": 0x50, "ann_class": 1},
        {"type": "data",    "value": 0x00, "ann_class": 2},
        {"type": "data",    "value": 0xBE, "ann_class": 2},
    ]
    verify_annotations(sys.argv[1], expected)
```

### 5.3 调试指南

当解码器输出与预期不符时，按以下步骤定位问题：

**步骤 1: 比较输出 JSON 文件，找到偏差位置**

```bash
python -c "
import json
with open('actual_py.json') as f: py = json.load(f)
with open('expected_c.json') as f: c = json.load(f)
py_anns = py.get('annotations', [])
c_anns = c.get('annotations', [])
for i, (pa, ca) in enumerate(zip(py_anns, c_anns)):
    if pa != ca:
        print(f'Deviation at annotation {i}:')
        print(f'  Py: {pa}')
        print(f'  C:  {ca}')
        break
"
```

**步骤 2: 通过 (start_sample, ann_class) 定位 `self.put()` 调用**

在源代码中搜索对应的 start_sample 值和 ann_class 值，找到产生该注解的 `self.put()` 调用。

**步骤 3: 检查文本格式**

- 注解文本是否包含长/中/短三级变体？（`[long, mid, short]`）
- 数值格式是否正确（十六进制、十进制、ASCII）？
- 字符串拼接是否正确？

**步骤 4: 检查采样位置**

- `self.ss` 和 `self.es` 是否与协议时序一致？
- `self.wait()` 返回后 `self.samplenum` 是否在正确的采样位置？

**步骤 5: 检查 `self.wait()` 条件**

- 条件是否与协议时序一致？
- `'r'`/`'f'`/`'h'`/`'l'` 是否与协议规范中的边沿/电平定义匹配？
- 多条件时 list of dicts 是否正确分组？
- `self.matched & (0b1 << N)` 的 N 是否对应正确的条件组？

**步骤 6: 检查 CRC/校验计算**

- CRC 多项式是否与协议规范一致？
- 初始值、反射、异或输出是否正确？
- 计算范围是否包含正确的字节？

**步骤 7: 检查堆叠解码器接口**

- `inputs` 是否与上游 `outputs` 匹配？
- `decode(self, ss, es, data)` 的 data 格式是否与上游 `put()` 输出一致？
- 状态机是否正确处理所有上游命令？

**步骤 8: 修复、重新测试（Python 无需编译）**

```bash
# Python 解码器修改后直接重新运行测试，无需编译
cd libsigrokdecode/tests
python run_all_tests.py --decoder myproto
```

**Python 调试优势**：
- 修改后立即生效，无需重新编译
- 可用 `print()` 调试输出到 stderr
- 可用 Python 交互式解释器单独测试辅助函数
- 异常堆栈直接指向问题代码行

---

## 质量检查清单

完成解码器后，逐项检查：

- [ ] `__init__.py` 和 `pd.py` 两个文件都已创建
- [ ] `__init__.py` 包含模块文档字符串和 `from .pd import Decoder`
- [ ] 类属性完整：`id`、`name`、`longname`、`desc`、`license`、`inputs`、`outputs`、`tags`
- [ ] `channels` / `optional_channels` 与协议规范一致（名称、描述、顺序）
- [ ] `options` 覆盖协议变体（位序、极性、校验模式等）
- [ ] `annotations` 覆盖协议所有字段和状态
- [ ] `annotation_rows` 合理分组注解类
- [ ] `reset()` 初始化所有状态变量
- [ ] `start()` 注册所有输出通道并读取选项
- [ ] `metadata()` 处理 `SRD_CONF_SAMPLERATE`
- [ ] `self.wait()` 条件与协议时序一致
- [ ] `self.wait()` 返回值已正确解包（引脚元组）
- [ ] 无顺序 `self.wait()` 调用（应合并为 list of dicts）
- [ ] `self.put()` 的 ann_class 索引与 annotations 元组顺序对应
- [ ] 注解文本包含长/中/短三级变体
- [ ] CRC/校验计算与协议规范一致
- [ ] 状态机覆盖协议所有状态和转换
- [ ] 堆叠解码器接口正确（`inputs`/`outputs`/`decode` 签名）
- [ ] 测试数据覆盖正常/错误/边界场景
- [ ] 解码器输出与协议规范一致

---

## 完整示例: I2C 协议

以下展示 I2C 协议的完整 5 阶段工作流程。

### 阶段 1: I2C 协议分析

#### 1.1 信号线

| 信号名 | 方向 | 有效电平 | 通道类型 | 描述 |
|--------|------|----------|----------|------|
| SCL | 主→从 | 上升沿采样 | 时钟线 | 串行时钟线 |
| SDA | 双向 | 高=1/低=0 | 串行数据线 | 串行数据线 |

#### 1.2 帧格式

```
SCL  ‾‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾\_/‾‾
SDA  ‾\_/A6\A5\A4\A3\A2\A1\A0\R/W\ACK\D7\D6\D5\D4\D3\D2\D1\D0\ACK\_/‾
     ↑                                                    ↑                ↑  ↑
   START                                                数据             ACK STOP
```

| 字段名 | 位宽 | 字节序 | 描述 |
|--------|------|--------|------|
| START | - | - | SCL高时SDA下降沿 |
| ADDRESS | 7 bit | MSB first | 设备地址 |
| R/W | 1 bit | - | 0=写, 1=读 |
| ACK/NACK | 1 bit | - | 0=ACK, 1=NACK（SDA由接收方驱动） |
| DATA | 8 bit | MSB first | 数据字节 |
| STOP | - | - | SCL高时SDA上升沿 |
| REPEAT START | - | - | 在STOP之前再次出现START条件 |

#### 1.3 状态机

```
                    ┌──────────────┐
                    │ FIND_START   │◄──────────────────┐
                    └──────┬───────┘                   │
                           │ SCL高+SDA下降沿            │ STOP: SCL高+SDA上升沿
                           ▼                           │
                    ┌──────────────┐                   │
                    │ FIND_ADDRESS │                   │
                    └──────┬───────┘                   │
                           │ 8个SCL上升沿               │
                           ▼                           │
                    ┌──────────────┐                   │
                    │  FIND_ACK    │                   │
                    └──────┬───────┘                   │
                           │ SCL上升沿                  │
                           ▼                           │
                    ┌──────────────┐── REPEAT START ───┘
                    │  FIND_DATA   │  (SCL高+SDA下降沿)
                    └──────┬───────┘
                           │ 8个SCL上升沿
                           ▼
                    ┌──────────────┐
                    │  FIND_ACK    │
                    └──────────────┘
```

### 阶段 2: I2C 设计决策

1. **有时钟线** → `self.wait({SCL: 'r'})` 上升沿采样
2. **无片选线** → 不需要 CS 处理
3. **需要向上堆叠** → `OUTPUT_PYTHON` 输出 I2C 协议包（START/STOP/ADDRESS/DATA/ACK/NACK）
4. **FIND_DATA 状态需多条件** → list of dicts 合并：`[{SCL: 'r'}, {SCL: 'h', SDA: 'f'}, {SCL: 'h', SDA: 'r'}]`

### 阶段 3: I2C 关键代码片段

以下展示 i2c/pd.py 中的核心实现模式：

#### 3.1 类属性定义

```python
SCL = 0
SDA = 1

# 协议命令 → 注解类/标签映射
proto = {
    'START':         [0,  'Start',         'S'],
    'START REPEAT':  [1,  'Start repeat',  'Sr'],
    'STOP':          [2,  'Stop',          'P'],
    'ACK':           [3,  'ACK',           'A'],
    'NACK':          [4,  'NACK',          'N'],
    'BIT':           [5,  'Bit',           'B'],
    'ADDRESS READ':  [6,  'Address read',  'AR'],
    'ADDRESS WRITE': [7,  'Address write', 'AW'],
    'DATA READ':     [8,  'Data read',     'DR'],
    'DATA WRITE':    [9,  'Data write',    'DW'],
    'PACKET':        [10, 'Packet',        'PK'],
}

class Decoder(srd.Decoder):
    api_version = 3
    id = 'i2c'
    name = 'I²C'
    longname = 'Inter-Integrated Circuit'
    desc = 'Two-wire, multi-master, serial bus.'
    license = 'gplv2+'
    inputs = ['logic']
    outputs = ['i2c']
    tags = ['Embedded/industrial']
    channels = (
        {'id': 'scl', 'name': 'SCL', 'desc': 'Serial clock line(串行时钟线)'},
        {'id': 'sda', 'name': 'SDA', 'desc': 'Serial data line(串行数据线)'},
    )
    options = (
        {'id': 'address_format', 'desc': 'Displayed slave address format(从地址格式)',
            'default': 'shifted', 'values': ('shifted', 'unshifted')},
        {'id': 'packets_format', 'desc': 'Display packets(数据格式)',
            'default': 'hex', 'values': ('none', 'hex', 'ascii', 'dec', 'bin', 'oct')},
    )
    annotations = (
        ('start', 'Start condition'),
        ('repeat-start', 'Repeat start condition'),
        ('stop', 'Stop condition'),
        ('ack', 'ACK'),
        ('nack', 'NACK'),
        ('bit', 'Data/address bit'),
        ('address-read', 'Address read'),
        ('address-write', 'Address write'),
        ('data-read', 'Data read'),
        ('data-write', 'Data write'),
        ('packet', 'Packet'),
    )
    annotation_rows = (
        ('bits', 'Bits', (5,)),
        ('addr-data', 'Address/data', (0, 1, 2, 3, 4, 6, 7, 8, 9)),
        ('packets', 'Packets', (10,)),
    )
    binary = (
        ('address-read', 'Address read'),
        ('address-write', 'Address write'),
        ('data-read', 'Data read'),
        ('data-write', 'Data write'),
    )
```

#### 3.2 状态机核心

```python
def decode(self):
    while True:
        # State machine.
        if self.state == 'FIND START':
            # 等待 START 条件: SCL = high, SDA = falling
            self.handle_start(self.wait({0: 'h', 1: 'f'}))
        elif self.state == 'FIND ADDRESS':
            # 等待数据位: SCL = rising
            self.handle_address_or_data(self.wait({0: 'r'}))
        elif self.state == 'FIND DATA':
            # 多条件等待: 数据位 OR 重复START OR STOP
            (scl, sda) = self.wait([{0: 'r'}, {0: 'h', 1: 'f'}, {0: 'h', 1: 'r'}])

            # 检查匹配了哪个条件
            if self.matched & (0b1 << 0):
                self.handle_address_or_data((scl, sda))
            elif self.matched & (0b1 << 1):
                self.handle_start((scl, sda))
            elif self.matched & (0b1 << 2):
                self.handle_stop((scl, sda))
        elif self.state == 'FIND ACK':
            # 等待 ACK/数据位: SCL = rising
            self.get_ack(self.wait({0: 'r'}))
```

#### 3.3 协议输出

```python
# START 条件输出
self.put(self.ss, self.es, self.out_python, ['START', None])
# 或重复起始
self.put(self.ss, self.es, self.out_python, ['START REPEAT', None])

# 地址输出（注意：地址不含 R/W 位）
self.put(self.ss, self.es, self.out_python,
    ['ADDRESS READ' if self.wr else 'ADDRESS WRITE', self.address])

# 数据输出
self.put(self.ss, self.es, self.out_python,
    ['DATA READ' if self.wr else 'DATA WRITE', self.databyte])

# ACK/NACK 输出
self.put(self.ss, self.es, self.out_python, ['ACK', None])
self.put(self.ss, self.es, self.out_python, ['NACK', None])

# STOP 条件输出
self.put(self.ss, self.es, self.out_python, ['STOP', None])
```

#### 3.4 注解输出（使用 proto 字典映射）

```python
# 使用 proto 字典统一管理注解类索引和文本
cmd = 'START'
self.put(self.ss, self.es, self.out_ann,
    [proto[cmd][0], proto[cmd][1:]])  # [class_idx, [long, short]]

# 带数值的注解
cmd = 'ADDRESS READ'
display = '0x%02X' % self.address
self.put(self.ss, self.es, self.out_ann,
    [proto[cmd][0],
     ['%s: %s' % (proto[cmd][1], display),
      '%s: %s' % (proto[cmd][2], display),
      display]])
```

### 阶段 4: I2C 测试数据生成

```python
class I2CGenerator:
    def __init__(self, builder, scl_ch, sda_ch):
        self.builder = builder
        self.scl = scl_ch
        self.sda = sda_ch
        self.half_period = 5

    def start(self):
        """START: SDA falls while SCL is high"""
        self.builder.write_channels({self.scl: 1, self.sda: 1}, 2)
        self.builder.write_channels({self.scl: 1, self.sda: 0}, 2)

    def stop(self):
        """STOP: SDA rises while SCL is high"""
        self.builder.write_channels({self.scl: 0, self.sda: 0}, self.half_period)
        self.builder.write_channels({self.scl: 1, self.sda: 0}, self.half_period)
        self.builder.write_channels({self.scl: 1, self.sda: 1}, 2)

    def write_byte(self, byte_val):
        """Write 8 bits MSB-first, then read ACK"""
        for i in range(7, -1, -1):
            bit = (byte_val >> i) & 1
            self.builder.write_channels({self.scl: 0, self.sda: bit}, self.half_period)
            self.builder.write_channels({self.scl: 1, self.sda: bit}, self.half_period)
        # ACK: slave pulls SDA low
        self.builder.write_channels({self.scl: 0, self.sda: 0}, self.half_period)
        self.builder.write_channels({self.scl: 1, self.sda: 0}, self.half_period)

def generate_i2c_test():
    sample_count = 10000
    builder = BitstreamBuilder(2, sample_count, 1000000)
    builder.pos = 100

    gen = I2CGenerator(builder, scl_ch=0, sda_ch=1)
    gen.start()
    gen.write_byte(0x50 << 1)     # Address 0x50, Write
    gen.write_byte(0x00)           # Data byte 1
    gen.write_byte(0xBE)           # Data byte 2
    gen.stop()

    config = {
        "decoder": "i2c",
        "samplerate": 1000000,
        "num_channels": 2,
        "sample_count": sample_count,
        "channels": {"scl": 0, "sda": 1},
    }
    # ... 写入文件
```

### 阶段 5: I2C 验证

**有 C 参考解码器**：

```bash
cd libsigrokdecode/tests
python run_all_tests.py --decoder i2c
```

**独立验证**：检查 actual_py.json 中的注解：
1. START 注解出现在 SDA 下降沿位置
2. ADDRESS WRITE 注解值为 0x50（shifted 格式）
3. DATA WRITE 注解值为 0x00 和 0xBE
4. ACK 注解出现在每个字节后
5. STOP 注解出现在 SDA 上升沿位置
6. PACKET 注解包含完整的 "0x50 WR: 00 BE" 格式

**堆叠验证**：在 i2c 上堆叠 lm75 解码器，检查 lm75 的注解是否正确解析了 i2c 输出的协议包。

---

## 附录: API 速查表

### `self.wait()` 条件字符

| 字符 | 含义 | 示例 |
|------|------|------|
| `'h'` | 通道高电平 | `{0: 'h'}` |
| `'l'` | 通道低电平 | `{0: 'l'}` |
| `'r'` | 通道上升沿 | `{0: 'r'}` |
| `'f'` | 通道下降沿 | `{0: 'f'}` |
| `'e'` | 通道任意边沿 | `{0: 'e'}` |
| `'n'` | 通道无变化 | `{0: 'n'}` |
| `'skip'` | 跳过采样数 | `{'skip': 100}` |

### 条件组合规则

| 形式 | 语义 | 示例 |
|------|------|------|
| `{0: 'r', 1: 'f'}` | AND（同时满足） | SCL上升 且 SDA下降 |
| `[{0: 'r'}, {1: 'f'}]` | OR（任一满足） | SCL上升 或 SDA下降 |
| `[{0: 'r'}, {'skip': 100}]` | OR + 超时 | SCL上升 或 超时100采样 |

### `self.register()` 输出类型

| 常量 | 用途 | 输出格式 |
|------|------|----------|
| `srd.OUTPUT_ANN` | 注解输出 | `[class_idx, [text1, text2, text3]]` |
| `srd.OUTPUT_PYTHON` | 协议输出（堆叠） | `[cmd_string, data]` |
| `srd.OUTPUT_BINARY` | 二进制输出 | `[class_idx, bytes_data]` |
| `srd.OUTPUT_META` | 元数据输出 | 单个数值（int/float） |

### `self.put()` 输出格式

| 输出类型 | 格式 | 示例 |
|----------|------|------|
| ANN | `[class_idx, [texts]]` | `[0, ['Start', 'S']]` |
| PYTHON | `[cmd, data]` | `['ADDRESS READ', 0x50]` |
| BINARY | `[class_idx, bytes]` | `[0, b'\x50']` |
| META | 单个数值 | `115200` |

### 类属性字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `api_version` | int | 必须为 `3` |
| `id` | str | 解码器ID（不用加 `_c` 后缀） |
| `name` | str | 显示名称 |
| `longname` | str | 完整名称 |
| `desc` | str | 描述 |
| `license` | str | `"gplv2+"` 或 `"gplv3+"` |
| `channels` | tuple of dict | 必选通道 |
| `optional_channels` | tuple of dict | 可选通道（可省略） |
| `options` | tuple of dict | 选项定义 |
| `annotations` | tuple of (str, str) | 注解类 `(id, label)` |
| `annotation_rows` | tuple of (str, str, tuple) | 注解行 `(row_id, label, class_indices)` |
| `binary` | tuple of (str, str) | 二进制输出类 |
| `inputs` | list of str | 输入类型（`['logic']` 或上游协议 id） |
| `outputs` | list of str | 输出类型（本解码器协议 id） |
| `tags` | list of str | 标签 |

### 回调方法

| 方法 | 调用时机 | 用途 |
|------|----------|------|
| `__init__(self)` | 实例化 | 调用 `self.reset()` |
| `reset(self)` | 重置/实例化 | 初始化所有状态变量 |
| `start(self)` | 解码开始 | 注册输出、读取选项 |
| `metadata(self, key, value)` | 元数据变更 | 获取采样率 |
| `decode(self)` | 主循环（逻辑输入） | 状态机主循环 |
| `decode(self, ss, es, data)` | 每个上游包（堆叠输入） | 处理上游协议包 |
| `end(self)` | 解码结束 | 清理（可选） |

### 实例属性

| 属性 | 类型 | 说明 |
|------|------|------|
| `self.samplenum` | int | 当前采样位置 |
| `self.matched` | int | 条件匹配位掩码 |
| `self.samplerate` | int/None | 采样率（Hz） |
| `self.options` | dict | 选项值字典 |
| `self.out_ann` | int | 注解输出 ID |
| `self.out_python` | int | 协议输出 ID |
| `self.out_binary` | int | 二进制输出 ID |
