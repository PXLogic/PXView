# 从协议规范创建 C 解码器开发指南

## 1. 概述

本指南面向需要**从协议规范/数据手册直接创建 C 协议解码器**的开发者。与从 Python 解码器翻译不同，从规范出发意味着没有现成的参考代码——你需要自己从时序图、帧格式表、状态描述中提取解码逻辑，并将其映射到 PXView C 解码器 API v4。

如果你需要将已有 Python 解码器翻译为 C，请参考 [`c-decoder-guide.md`](c-decoder-guide.md)。

**本指南适用场景**：

- 协议规范已存在（芯片数据手册、通信标准文档），但尚无 Python 或 C 解码器
- 需要为新协议或私有协议创建解码器
- 需要理解如何将协议规范中的时序描述转化为 `c_wait()` 条件

**阅读前提**：

- 熟悉 C 语言（C11 标准）
- 了解目标协议的基本工作原理
- 建议先阅读 [`c-decoder-guide.md`](c-decoder-guide.md) 了解 API v4 的完整参考

---

## 2. 协议分析方法论

从协议规范到可工作的解码器，核心工作是**信息提取与结构化**。协议规范通常以自然语言、时序图、帧格式表的形式呈现，而解码器需要精确的信号条件、位操作和状态转换。本章提供系统化的提取方法。

### 2.1 信号线提取

**目标**：从规范中识别所有信号线，确定其类型（时钟/数据/控制），并映射到 `srd_channel` 定义。

**从时序图识别信号线的方法**：

1. **时钟线**：时序图中呈现周期性方波的信号。时钟线定义了数据采样的节拍。
   - 特征：频率固定或可控，占空比通常接近 50%
   - 映射：`SRD_CHANNEL_SCLK`
   - 通道 `order` 通常设为 0（第一个通道）

2. **数据线**：在时钟边沿附近发生变化的信号。数据线承载实际传输的信息。
   - 特征：仅在时钟有效边沿附近改变状态，其他时间保持稳定
   - 映射：`SRD_CHANNEL_SDATA`
   - 通道 `order` 通常从 1 开始

3. **控制线**：用于标识帧边界、使能设备或指示方向的信号。
   - 特征：状态变化频率远低于时钟，通常跨越多个时钟周期
   - 常见信号：CS#（片选）、EN（使能）、RST#（复位）、DC（数据/命令选择）
   - 映射：`SRD_CHANNEL_COMMON`
   - 通常作为可选通道（`optional_channels`）

**常见协议信号线对照**：

| 协议 | 时钟线 | 数据线 | 控制线 |
|------|--------|--------|--------|
| SPI | SCLK | MOSI + MISO | CS# |
| I²C | SCL | SDA | — |
| UART | — | RX + TX | — |
| I2S | SCLK | SDIN | WS（字选择） |
| JTAG | TCK | TDI + TDO | TMS |
| SWD | SWCLK | SWDIO | — |
| MICROWIRE | SK | SI + SO | CS# |
| 1-Wire | — | DQ（数据+时钟复用） | — |

**提取步骤**：

1. 在时序图中找到所有标注的信号名称
2. 根据波形特征分类（周期性→时钟，时钟同步变化→数据，低频状态→控制）
3. 确定必需通道与可选通道（如 CS# 在 SPI 中通常是可选的）
4. 为每个通道分配 `order` 索引（必需通道从 0 开始，可选通道独立编号）

**代码映射示例**（SPI 协议）：

```c
#define CH_SCLK 0
#define CH_MOSI 1
#define CH_MISO 2

static struct srd_channel spi_channels[] = {
    {"sclk", "SCLK", "Serial clock line", 0, SRD_CHANNEL_SCLK, "dec_spi_chan_sclk"},
    {"mosi", "MOSI", "Master out, slave in", 1, SRD_CHANNEL_SDATA, "dec_spi_chan_mosi"},
    {"miso", "MISO", "Master in, slave out", 2, SRD_CHANNEL_SDATA, "dec_spi_chan_miso"},
};

static struct srd_channel spi_optional_channels[] = {
    {"cs",   "CS#",  "Chip select (active low)", 0, SRD_CHANNEL_COMMON, "dec_spi_opt_chan_cs"},
    {"wp",   "WP#",  "Write protect",            1, SRD_CHANNEL_COMMON, "dec_spi_opt_chan_wp"},
    {"hold", "HOLD#","Hold",                     2, SRD_CHANNEL_COMMON, "dec_spi_opt_chan_hold"},
};
```

**注意事项**：

- 通道 `order` 字段在必需通道和可选通道中**独立编号**，都从 0 开始
- 解码器中通过绝对索引访问通道：`c_pin(di, CH_SCLK)` 中的索引 = 必需通道数 + 可选通道相对索引
- `idn` 字段用于国际化，格式为 `"dec_<decoder>_chan_<name>"` 或 `"dec_<decoder>_opt_chan_<name>"`

---

### 2.2 帧格式分析

**目标**：从规范的帧结构图和字段描述表中提取帧边界条件和字段定义。

**帧边界识别**：

1. **起始条件**：标识一个帧开始的信号模式
   - 特定信号边沿（如 UART 的起始位：数据线从高到低）
   - 特定信号组合（如 I²C 的 START：SCL 高时 SDA 下降沿）
   - 控制信号电平（如 SPI 的 CS# 拉低）

2. **结束条件**：标识一个帧结束的信号模式
   - 特定信号边沿（如 UART 的停止位：数据线保持高电平 1-2 个位周期）
   - 特定信号组合（如 I²C 的 STOP：SCL 高时 SDA 上升沿）
   - 控制信号电平（如 SPI 的 CS# 拉高）

**字段表提取模板**：

对规范中的每个帧类型，建立如下字段表：

| 字段名 | 位宽 | 字节序 | 有效值 | 描述 |
|--------|------|--------|--------|------|
| START  | 1 bit | — | 0 | 起始位 |
| ADDR   | 7 bit | MSB first | 0x00-0x7F | 设备地址 |
| R/W    | 1 bit | — | 0=写, 1=读 | 读写方向 |
| ACK    | 1 bit | — | 0=ACK, 1=NACK | 应答 |
| DATA   | 8 bit | MSB first | 0x00-0xFF | 数据字节 |
| STOP   | — | — | — | 停止条件 |

**示例：I²C 帧格式分析**

根据 I²C 规范，一个完整的写操作帧结构为：

```
┌───────┬──────────────────┬──────┬──────────────────┬──────┬──────┐
│ START │ ADDR(7) + RW(1)  │ ACK  │ DATA(8)          │ ACK  │ STOP │
└───────┴──────────────────┴──────┴──────────────────┴──────┴──────┘
```

映射到注释类枚举：

```c
enum i2c_ann {
    ANN_START         = 0,
    ANN_REPEAT_START  = 1,
    ANN_STOP          = 2,
    ANN_ACK           = 3,
    ANN_NACK          = 4,
    ANN_BIT           = 5,
    ANN_ADDRESS_READ  = 6,
    ANN_ADDRESS_WRITE = 7,
    ANN_DATA_READ     = 8,
    ANN_DATA_WRITE    = 9,
    NUM_ANN,
};
```

**字段表到代码的映射原则**：

- 每个独立语义字段 → 一个 `ANN_*` 枚举值
- 字段值 → `c_put()` 或 `c_put_v()` 输出
- 字段位宽 → 决定 `c_wait()` 循环次数
- 字节序 → 决定位移方向（MSB first: `databyte = (databyte << 1) | bit`；LSB first: `databyte |= (bit << bitcount)`）

---

### 2.3 时序约束提取

**目标**：从规范的时序参数表中确定采样策略和等待条件。

**关键时序参数**：

1. **采样边沿**：在时钟的哪个边沿采样数据
   - 上升沿采样 → `CW_R(CLK)`
   - 下降沿采样 → `CW_F(CLK)`
   - 双沿采样 → `CW_E(CLK)`（如 DDR 模式）

2. **建立/保持时间**：数据在采样边沿前后的稳定时间
   - 这决定了 `c_wait()` 条件的 AND 组合
   - 如果规范要求"CS# 必须在 SCLK 上升沿前拉低"，则需要 `CW_L(CS), CW_R(SCLK)` 的 AND 条件

3. **超时条件**：帧内最大等待时间
   - 映射为 `CW_SKIP(timeout_samples)`
   - 超时通常与 `CW_OR` 组合使用

**从时序图提取采样策略的步骤**：

1. 找到时序图中标注的采样点（通常用箭头或虚线标记）
2. 确定采样点相对于时钟边沿的位置
3. 检查是否有建立时间（setup time）要求，这决定了是否需要额外的等待条件
4. 检查是否有超时参数，这决定了是否需要 `CW_SKIP`

**示例：SPI 模式 0（CPOL=0, CPHA=0）**

根据 SPI 规范：
- SCLK 空闲状态为低电平
- 数据在 SCLK 上升沿采样
- CS# 必须在传输期间保持低电平

映射到 `c_wait()` 条件：

```c
// 等待 SCLK 上升沿（数据在上升沿有效）
ret = c_wait(di, CW_R(CH_SCLK), CW_END);
if (ret != SRD_OK) return;

// 如果有 CS#，需要额外检查 CS# 为低
if (has_cs && c_pin(di, CH_CS) != 0)
    continue;  // CS# 为高，忽略此时钟沿
```

**示例：I²C START 条件检测**

根据 I²C 规范：
- START 条件：SCL 为高电平时，SDA 从高变低

映射到 `c_wait()` 条件：

```c
// 等待 SCL 高且 SDA 下降沿
ret = c_wait(di, CW_H(CH_SCL), CW_F(CH_SDA), CW_END);
```

**超时处理示例**：

假设规范要求"从 START 到第一个数据位必须在 1000 个采样内完成"：

```c
// 等待 SCL 上升沿或超时
ret = c_wait(di, CW_R(CH_SCL), CW_OR, CW_SKIP(1000), CW_END);
if (ret != SRD_OK) return;

if (di_matched(di) & (1ULL << 1)) {
    // 超时匹配 — 处理超时
    s->state = STATE_IDLE;
    continue;
}

// SCL 上升沿匹配 — 正常处理
```

---

### 2.4 校验机制提取

**目标**：从规范中提取 CRC/校验和参数，并在解码器中实现校验逻辑。

**常见校验类型及参数**：

1. **CRC（循环冗余校验）**
   - 需要提取的参数：多项式（polynomial）、初始值（init）、输入反转（refin）、输出反转（refout）、输出异或值（xorout）
   - 规范通常以表格形式给出这些参数

2. **简单校验和**
   - 异或校验：所有字节的异或值
   - 累加和：所有字节的算术和（取低字节）
   - 奇偶校验：单个字节的奇偶位

3. **专用校验**
   - 某些协议使用自定义校验算法（如 CAN 的位填充）

**CRC 参数提取示例**：

规范中的 CRC 描述通常如下：

> CRC-8-SMBus: polynomial = x⁸ + x² + x + 1, init = 0x00, refin = false, refout = false, xorout = 0x00

转换为代码参数：

| 参数 | 值 | 说明 |
|------|----|------|
| poly | 0x07 | x⁸ + x² + x + 1 = 100000111 → 去掉最高位 → 00000111 = 0x07 |
| init | 0x00 | 初始值 |
| refin | false | 输入不反转 |
| refout | false | 输出不反转 |
| xorout | 0x00 | 输出不异或 |

**CRC 查找表生成**：

```c
static uint8_t crc8_table[256];

static void crc8_init_table(uint8_t poly)
{
    for (int i = 0; i < 256; i++) {
        uint8_t crc = (uint8_t)i;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ poly;
            else
                crc <<= 1;
        }
        crc8_table[i] = crc;
    }
}

static uint8_t crc8_calc(const uint8_t *data, int len, uint8_t init)
{
    uint8_t crc = init;
    for (int i = 0; i < len; i++)
        crc = crc8_table[crc ^ data[i]];
    return crc;
}
```

**逐位 CRC 计算（适用于流式解码）**：

```c
static uint8_t crc8_update(uint8_t crc, uint8_t bit, uint8_t poly)
{
    crc <<= 1;
    if ((crc & 0x80) ^ (bit ? 0x80 : 0x00))
        crc ^= poly;
    return crc;
}
```

**异或校验和**：

```c
static uint8_t xor_checksum(const uint8_t *data, int len)
{
    uint8_t cs = 0;
    for (int i = 0; i < len; i++)
        cs ^= data[i];
    return cs;
}
```

**奇偶校验**：

```c
static int parity_check(uint8_t byte, int parity_type)
{
    int ones = 0;
    for (int i = 0; i < 8; i++)
        if (byte & (1 << i)) ones++;
    // parity_type: 0 = even, 1 = odd
    return (ones % 2) == parity_type;
}
```

**校验结果输出**：

```c
if (crc == received_crc) {
    c_put(di, ss, es, s->out_ann, ANN_CRC_OK, "CRC OK");
} else {
    c_put(di, ss, es, s->out_ann, ANN_CRC_ERR, "CRC ERROR",
          err_str);
}
```

---

### 2.5 状态机建模

**目标**：从协议描述中构建状态转换图，并映射到 C 解码器的 `switch/case` 结构。

**状态识别方法**：

1. 在规范中寻找"模式"、"阶段"、"状态"等关键词
2. 识别协议操作的有序步骤（如：等待起始 → 接收地址 → 接收数据 → 校验 → 结束）
3. 每个步骤对应一个状态
4. 步骤之间的转换条件对应状态转换

**状态转换图绘制**：

使用 ASCII 图表示状态机，便于后续编码：

```
                    ┌──────────────┐
                    │  FIND_START  │◄──────────────────────┐
                    └──────┬───────┘                       │
                     SDA↓ while SCL=H                     │
                           │                               │
                    ┌──────▼───────┐                       │
                    │ FIND_ADDRESS │◄──┐                   │
                    └──────┬───────┘   │                   │
                     8 bits on SCL↑   │                   │
                           │           │                   │
                    ┌──────▼───────┐   │                   │
                    │   FIND_ACK   │   │                   │
                    └──────┬───────┘   │                   │
                     ACK: continue     │  STOP condition   │
                     NACK: ────────┐   │  (SCL=H, SDA↑)   │
                           │       │   │        │          │
                    ┌──────▼───────┐  │   ┌──────▼───────┐ │
                    │  FIND_DATA   │──┘   │  FIND_START  │─┘
                    └──────────────┘      └──────────────┘
```

**状态枚举定义**：

```c
enum myproto_state {
    STATE_FIND_START,
    STATE_FIND_ADDRESS,
    STATE_FIND_ACK,
    STATE_FIND_DATA,
};
```

**状态转换表**：

| 当前状态 | 条件 | 动作 | 下一状态 |
|----------|------|------|----------|
| FIND_START | SCL=H, SDA↓ | 输出 START 注解 | FIND_ADDRESS |
| FIND_ADDRESS | SCL↑, 8 bits collected | 输出地址注解 | FIND_ACK |
| FIND_ACK | SCL↑, SDA=0 | 输出 ACK 注解 | FIND_DATA |
| FIND_ACK | SCL↑, SDA=1 | 输出 NACK 注解 | FIND_DATA |
| FIND_DATA | SCL↑, 8 bits collected | 输出数据注解 | FIND_ACK |
| FIND_DATA | SCL=H, SDA↑ | 输出 STOP 注解 | FIND_START |
| FIND_DATA | SCL=H, SDA↓ | 输出重复 START 注解 | FIND_ADDRESS |

**状态机代码骨架**：

```c
static void myproto_decode(struct srd_decoder_inst *di)
{
    myproto_state *s = (myproto_state *)c_decoder_get_private(di);

    while (1) {
        int ret;
        switch (s->state) {

        case STATE_FIND_START:
            ret = c_wait(di, CW_H(CH_SCL), CW_F(CH_SDA), CW_END);
            if (ret != SRD_OK) return;
            handle_start(di, s);
            s->state = STATE_FIND_ADDRESS;
            break;

        case STATE_FIND_ADDRESS:
            ret = c_wait(di, CW_R(CH_SCL), CW_END);
            if (ret != SRD_OK) return;
            if (handle_address_bit(di, s))  // returns 1 when 8 bits done
                s->state = STATE_FIND_ACK;
            break;

        case STATE_FIND_ACK:
            ret = c_wait(di, CW_R(CH_SCL), CW_END);
            if (ret != SRD_OK) return;
            handle_ack(di, s);
            s->state = STATE_FIND_DATA;
            break;

        case STATE_FIND_DATA:
            ret = c_wait(di, CW_R(CH_SCL), CW_OR,
                         CW_H(CH_SCL), CW_F(CH_SDA), CW_OR,
                         CW_H(CH_SCL), CW_R(CH_SDA), CW_END);
            if (ret != SRD_OK) return;

            if (di_matched(di) & (1ULL << 0)) {
                // SCL 上升沿 — 数据位
                if (handle_data_bit(di, s))
                    s->state = STATE_FIND_ACK;
            } else if (di_matched(di) & (1ULL << 1)) {
                // START 条件
                handle_start(di, s);
                s->state = STATE_FIND_ADDRESS;
            } else if (di_matched(di) & (1ULL << 2)) {
                // STOP 条件
                handle_stop(di, s);
                s->state = STATE_FIND_START;
            }
            break;
        }
    }
}
```

---

## 3. 解码器设计决策树

从协议规范提取信息后，需要做出一系列设计决策。以下决策树帮助你系统化地确定解码器的架构。

### 3.1 核心决策树

```
协议有独立的时钟线吗？
├── 是 → 使用 CW_R(CLK) 或 CW_F(CLK) 作为主等待条件
│   │
│   ├── 有片选信号吗？
│   │   ├── 是 → 添加 CS 守卫条件（CW_L(CS) 或在 c_wait 后检查 c_pin）
│   │   └── 否 → 不需要 CS 条件
│   │
│   ├── 数据在哪个边沿采样？
│   │   ├── 上升沿 → CW_R(CLK)
│   │   ├── 下降沿 → CW_F(CLK)
│   │   └── 双沿 → CW_E(CLK)
│   │
│   └── 有超时要求吗？
│       ├── 是 → CW_R(CLK), CW_OR, CW_SKIP(timeout)
│       └── 否 → 仅 CW_R(CLK)
│
└── 否 → 使用 CW_E(DATA) 或 CW_R(DATA)/CW_F(DATA) 作为主等待条件
    │
    ├── 有固定波特率吗？
    │   ├── 是 → 用 CW_SKIP(bit_period) 模拟时钟
    │   └── 否 → 用 CW_E(DATA) 检测边沿
    │
    └── 有起始位检测吗？
        ├── 是 → CW_F(DATA) 检测起始位，然后 CW_SKIP(bit_period) 逐位采样
        └── 否 → CW_E(DATA) 检测每个边沿
```

### 3.2 堆叠决策

```
解码器需要向上层提供协议数据吗？
├── 是 → 需要 c_proto() 输出
│   ├── 定义 outputs = {"proto_id", NULL}
│   ├── 在 start() 中注册 c_reg_out(di, SRD_OUTPUT_PROTO, "proto_id")
│   └── 在解码逻辑中调用 c_proto(di, ss, es, out_proto, "CMD", C_U8(val), C_END)
│
└── 否 → 不需要协议输出
    └── outputs = {NULL}, num_outputs = 0

解码器需要接收下层协议数据吗？
├── 是 → 需要 decode_upper 回调
│   ├── 定义 inputs = {"lower_proto_id", NULL}
│   ├── channels = NULL, num_channels = 0
│   ├── 实现 decode_upper() 回调函数
│   └── decode() 可以为空
│
└── 否 → 直接从采样流解码
    ├── inputs = {"logic", NULL}
    └── 实现 decode() 函数
```

### 3.3 注释类设计

```
协议有哪些语义单元需要独立标注？
├── 帧控制信号 → ANN_START, ANN_STOP, ANN_ACK, ANN_NACK
├── 地址字段   → ANN_ADDRESS_READ, ANN_ADDRESS_WRITE
├── 数据字段   → ANN_DATA_READ, ANN_DATA_WRITE
├── 校验字段   → ANN_CRC_OK, ANN_CRC_ERR, ANN_PARITY
├── 控制字段   → ANN_COMMAND, ANN_STATUS
└── 错误标注   → ANN_ERROR, ANN_WARNING
```

**多级文本变体设计**：

每个注释类应提供 2-3 级文本变体，对应不同的缩放级别显示：

| 级别 | 用途 | 示例 |
|------|------|------|
| 长 | 放大时显示完整信息 | `"Address write: 0x50"` |
| 中 | 正常缩放时显示 | `"AW: 50"` |
| 短 | 缩小时显示 | `"50"` |

```c
c_put_v(di, ss, es, out_ann, ANN_ADDRESS_WRITE, addr,
        "Address write: 0x50", "AW: 50", "50");
```

### 3.4 状态结构体设计

使用 `C_DECODER_STATE` 宏自动生成状态结构体、reset 和 destroy 函数：

```c
C_DECODER_STATE(myproto, {
    enum myproto_state state;
    int bitcount;
    uint8_t databyte;
    uint64_t ss_byte;
    uint64_t samplerate;
    int out_ann;
    int out_proto;
});
```

**状态结构体设计原则**：

- `state` 枚举必须包含（用 `calloc` 零初始化后值为 0 的状态应作为初始状态）
- 所有需要在 `c_wait()` 调用之间保持的变量都应放入状态结构体
- 非零初始值在 `start()` 回调中设置（`C_DECODER_STATE` 的 `reset` 使用 `calloc` 零初始化）
- 如果状态结构体包含需要特殊释放的资源（如 `GArray *`），需自行实现 `reset`/`destroy`

### 3.5 输出注册

在 `start()` 回调中注册所有输出通道：

```c
static void myproto_start(struct srd_decoder_inst *di)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);

    s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "myproto");
    s->out_proto  = c_reg_out(di, SRD_OUTPUT_PROTO, "myproto");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "myproto");
    s->out_meta   = c_reg_meta(di, SRD_OUTPUT_META, "myproto",
                               "int", "bitrate", "Bitrate");
    s->samplerate = c_samplerate(di);
}
```

---

## 4. 从规范到代码的映射规则

本章提供协议规范中常见描述到 C 解码器代码的具体映射规则。

### 4.1 时序图 → c_wait() 条件

| 规范描述 | c_wait() 代码 |
|----------|---------------|
| "在 CLK 上升沿采样数据" | `c_wait(di, CW_R(CLK), CW_END)` |
| "在 CLK 下降沿采样数据" | `c_wait(di, CW_F(CLK), CW_END)` |
| "等待 CS# 拉低后，在 CLK 上升沿采样" | `c_wait(di, CW_L(CS), CW_R(CLK), CW_END)` |
| "等待 START 条件（SCL 高时 SDA 下降沿）" | `c_wait(di, CW_H(SCL), CW_F(SDA), CW_END)` |
| "等待 STOP 条件（SCL 高时 SDA 上升沿）" | `c_wait(di, CW_H(SCL), CW_R(SDA), CW_END)` |
| "等待数据边沿或超时" | `c_wait(di, CW_E(DATA), CW_OR, CW_SKIP(timeout), CW_END)` |
| "等待 SCL 上升沿或 START 或 STOP" | `c_wait(di, CW_R(SCL), CW_OR, CW_H(SCL), CW_F(SDA), CW_OR, CW_H(SCL), CW_R(SDA), CW_END)` |
| "无条件前进一个采样" | `c_wait(di, CW_END)` |
| "跳过 N 个采样" | `c_wait(di, CW_SKIP(N), CW_END)` |
| "等待 CLK 双沿（DDR 模式）" | `c_wait(di, CW_E(CLK), CW_END)` |
| "等待 CS# 上升沿（帧结束）" | `c_wait(di, CW_R(CS), CW_END)` |

**AND 条件 vs OR 条件**：

- **AND 条件**：同一 `c_wait()` 调用中不含 `CW_OR` 的条件，必须同时满足
  ```c
  // SCL 高 AND SDA 下降沿 — 必须在同一采样点同时成立
  c_wait(di, CW_H(SCL), CW_F(SDA), CW_END);
  ```

- **OR 条件**：由 `CW_OR` 分隔的条件组，任一组满足即返回
  ```c
  // SCL 上升沿 OR (SCL 高 AND SDA 下降沿)
  c_wait(di, CW_R(SCL), CW_OR, CW_H(SCL), CW_F(SDA), CW_END);
  ```

**CS# 守卫条件的两种实现方式**：

方式一：在 `c_wait()` 后检查 CS# 电平（适用于 CS# 不参与条件匹配的场景）

```c
while (1) {
    ret = c_wait(di, CW_R(CH_SCLK), CW_END);
    if (ret != SRD_OK) return;

    // CS# 为高时忽略此时钟沿
    if (has_cs && c_pin(di, CH_CS) != 0)
        continue;

    // 正常处理数据位
    int mosi = c_pin(di, CH_MOSI);
    // ...
}
```

方式二：将 CS# 纳入 `c_wait()` OR 条件（适用于需要检测 CS# 边沿的场景）

```c
while (1) {
    ret = c_wait(di, CW_R(CH_SCLK), CW_OR, CW_R(CH_CS), CW_OR, CW_F(CH_CS), CW_END);
    if (ret != SRD_OK) return;

    if (di_matched(di) & (1ULL << 0)) {
        // SCLK 上升沿 — 检查 CS# 后处理
        if (has_cs && c_pin(di, CH_CS) != 0)
            continue;
        // 处理数据位
    } else if (di_matched(di) & (1ULL << 1)) {
        // CS# 上升沿 — 帧结束
    } else if (di_matched(di) & (1ULL << 2)) {
        // CS# 下降沿 — 帧开始
    }
}
```

---

### 4.2 字段表 → 注解枚举 + c_put() 调用

**映射规则**：规范字段表中的每个独立语义字段对应一个 `ANN_*` 枚举值和一个 `c_put()` 调用。

**示例：假设一个简化的寄存器读协议**

规范字段表：

| 字段 | 位宽 | 描述 |
|------|------|------|
| START | 1 bit | 起始位（固定为 0） |
| CMD | 2 bit | 命令码：00=读, 01=写, 10=复位, 11=保留 |
| ADDR | 6 bit | 寄存器地址 |
| DATA | 8 bit | 数据（仅写命令） |
| CRC | 4 bit | CRC-4 校验 |
| STOP | 1 bit | 停止位（固定为 1） |

映射到枚举和标签：

```c
enum myproto_ann {
    ANN_START_BIT  = 0,
    ANN_CMD        = 1,
    ANN_ADDR       = 2,
    ANN_DATA_WRITE = 3,
    ANN_DATA_READ  = 4,
    ANN_CRC_OK     = 5,
    ANN_CRC_ERR    = 6,
    ANN_STOP_BIT   = 7,
    NUM_ANN,
};

static const char *myproto_ann_labels[][3] = {
    {"", "Start bit",  "Start bit"},        // ANN_START_BIT
    {"", "Command",    "CMD"},              // ANN_CMD
    {"", "Address",    "ADDR"},             // ANN_ADDR
    {"", "Data write", "DW"},               // ANN_DATA_WRITE
    {"", "Data read",  "DR"},               // ANN_DATA_READ
    {"", "CRC OK",     "OK"},               // ANN_CRC_OK
    {"", "CRC error",  "ERR"},              // ANN_CRC_ERR
    {"", "Stop bit",   "Stop bit"},         // ANN_STOP_BIT
};
```

映射到 `c_put()` 调用：

```c
// START 位
c_put(di, ss_start, ss_start + bitwidth, s->out_ann, ANN_START_BIT, "Start", "S");

// CMD 字段
const char *cmd_str;
switch (s->databyte >> 6) {
    case 0: cmd_str = "Read";  break;
    case 1: cmd_str = "Write"; break;
    case 2: cmd_str = "Reset"; break;
    default: cmd_str = "Reserved"; break;
}
c_put_v(di, ss_cmd, es_cmd, s->out_ann, ANN_CMD, s->databyte >> 6,
        cmd_str, "CMD");

// ADDR 字段
char addr_str[16];
snprintf(addr_str, sizeof(addr_str), "0x%02x", addr);
c_put_v(di, ss_addr, es_addr, s->out_ann, ANN_ADDR, addr,
        addr_str, "ADDR");

// CRC 校验结果
if (crc_ok) {
    c_put(di, ss_crc, es_crc, s->out_ann, ANN_CRC_OK, "CRC OK", "OK");
} else {
    c_put(di, ss_crc, es_crc, s->out_ann, ANN_CRC_ERR, "CRC ERROR", "ERR");
}
```

---

### 4.3 状态图 → enum state + switch/case

**映射规则**：状态图中的每个状态对应一个 `enum` 值，每条转换边对应 `switch/case` 中的一个 `case` 分支。

**完整示例：简化 UART 解码器**

状态图：

```
┌─────────┐  start bit (RX↓)  ┌──────────┐  8 bits  ┌──────────┐  parity  ┌──────────┐  stop bit  ┌───────┐
│  IDLE   │─────────────────►│ DATA_BIT │────────►│ PARITY   │────────►│ STOP_BIT │──────────►│ IDLE  │
└─────────┘                   └──────────┘         └──────────┘         └──────────┘            └───────┘
                                  │                                          │ error
                                  │ framing error                            ▼
                                  └────────────────────────────────────► ANN_ERROR → IDLE
```

状态枚举：

```c
enum uart_state {
    STATE_IDLE,
    STATE_DATA_BIT,
    STATE_PARITY,
    STATE_STOP_BIT,
};
```

状态机代码：

```c
static void uart_decode(struct srd_decoder_inst *di)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    uint64_t bit_period = s->samplerate / s->baudrate;

    while (1) {
        int ret;
        switch (s->state) {

        case STATE_IDLE:
            // 等待起始位：RX 下降沿
            ret = c_wait(di, CW_F(CH_RX), CW_END);
            if (ret != SRD_OK) return;

            // 跳过半个位周期，在位中心采样
            c_wait(di, CW_SKIP(bit_period / 2), CW_END);

            // 确认起始位仍然为低
            if (c_pin(di, CH_RX) == 0) {
                s->ss_byte = di_samplenum(di);
                s->bitcount = 0;
                s->databyte = 0;
                s->state = STATE_DATA_BIT;
            }
            break;

        case STATE_DATA_BIT:
            // 跳过一个完整位周期，在位中心采样
            c_wait(di, CW_SKIP(bit_period), CW_END);

            int bit = c_pin(di, CH_RX);
            if (s->bit_order == 0)  // LSB first
                s->databyte |= (bit << s->bitcount);
            else  // MSB first
                s->databyte = (s->databyte << 1) | bit;

            s->bitcount++;
            if (s->bitcount >= s->data_bits) {
                if (s->has_parity)
                    s->state = STATE_PARITY;
                else
                    s->state = STATE_STOP_BIT;
            }
            break;

        case STATE_PARITY:
            c_wait(di, CW_SKIP(bit_period), CW_END);

            int parity_bit = c_pin(di, CH_RX);
            int computed = compute_parity(s->databyte, s->parity_type);
            if (parity_bit != computed) {
                c_put(di, s->ss_byte, di_samplenum(di), s->out_ann,
                      ANN_PARITY_ERR, "Parity error", "PE");
            } else {
                c_put(di, s->ss_byte, di_samplenum(di), s->out_ann,
                      ANN_PARITY_OK, "Parity OK", "PO");
            }
            s->state = STATE_STOP_BIT;
            break;

        case STATE_STOP_BIT:
            c_wait(di, CW_SKIP(bit_period), CW_END);

            int stop_bit = c_pin(di, CH_RX);
            if (stop_bit == 1) {
                char data_str[16];
                snprintf(data_str, sizeof(data_str), "0x%02x", s->databyte);
                c_put_v(di, s->ss_byte, di_samplenum(di), s->out_ann,
                       ANN_DATA, s->databyte, data_str, "D");
            } else {
                c_put(di, s->ss_byte, di_samplenum(di), s->out_ann,
                      ANN_FRAMING_ERR, "Framing error", "FE");
            }
            s->state = STATE_IDLE;
            break;
        }
    }
}
```

---

### 4.4 校验算法 → CRC/校验和 C 实现

**CRC 查找表实现**（适用于大多数 CRC 标准）：

```c
// CRC-8 查找表
static const uint8_t crc8_table[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15,
    0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    // ... 256 entries total, generated by crc8_init_table(0x07)
};

static uint8_t crc8_calc(const uint8_t *data, int len, uint8_t init)
{
    uint8_t crc = init;
    for (int i = 0; i < len; i++)
        crc = crc8_table[crc ^ data[i]];
    return crc;
}
```

**CRC-16 查找表实现**：

```c
static uint16_t crc16_table[256];

static void crc16_init_table(uint16_t poly)
{
    for (int i = 0; i < 256; i++) {
        uint16_t crc = (uint16_t)(i << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ poly;
            else
                crc <<= 1;
        }
        crc16_table[i] = crc;
    }
}

static uint16_t crc16_calc(const uint8_t *data, int len, uint16_t init)
{
    uint16_t crc = init;
    for (int i = 0; i < len; i++)
        crc = (crc << 8) ^ crc16_table[((crc >> 8) ^ data[i]) & 0xFF];
    return crc;
}
```

**带反转的 CRC 计算**（如 CRC-32/MPEG-2、CRC-16/MODBUS）：

```c
static uint8_t reflect8(uint8_t val)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; i++)
        if (val & (1 << i)) r |= (1 << (7 - i));
    return r;
}

static uint16_t reflect16(uint16_t val)
{
    uint16_t r = 0;
    for (int i = 0; i < 16; i++)
        if (val & (1 << i)) r |= (1 << (15 - i));
    return r;
}

// CRC-16/MODBUS: poly=0x8005, init=0xFFFF, refin=true, refout=true, xorout=0x0000
static uint16_t crc16_modbus(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)reflect8(data[i]) << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x8005;
            else
                crc <<= 1;
        }
    }
    return reflect16(crc);
}
```

**在流式解码中使用 CRC**：

解码器逐位接收数据，无法一次性获得完整帧。可以逐位更新 CRC：

```c
// 在状态结构体中维护 CRC 状态
C_DECODER_STATE(myproto, {
    // ...
    uint8_t crc;
    uint8_t data_bytes[256];
    int data_count;
});

// 每收到一个完整字节，更新 CRC
s->crc = crc8_table[s->crc ^ byte];
s->data_bytes[s->data_count++] = byte;

// 帧结束时验证 CRC
if (s->crc == 0) {
    c_put(di, ss, es, s->out_ann, ANN_CRC_OK, "CRC OK", "OK");
} else {
    char err[32];
    snprintf(err, sizeof(err), "CRC error (got 0x%02x)", s->crc);
    c_put(di, ss, es, s->out_ann, ANN_CRC_ERR, err, "ERR");
}
```

---

### 4.5 可配置参数 → srd_decoder_option 定义

**整数选项**：波特率、位宽、地址宽度等

```c
static struct srd_decoder_option myproto_options[] = {
    {"baudrate", "dec_myproto_opt_baudrate", "Baud rate", NULL, NULL},
    {"data_bits", "dec_myproto_opt_data_bits", "Data bits per frame", NULL, NULL},
};

// 在 srd_c_decoder_entry() 中设置默认值
myproto_options[0].def = g_variant_new_int64(115200);
myproto_options[1].def = g_variant_new_int64(8);

// 在 start() 中读取
int64_t baudrate = c_opt_int(di, "baudrate", 115200);
int64_t data_bits = c_opt_int(di, "data_bits", 8);
```

**字符串选项**：信号极性、位序、地址格式等

```c
static struct srd_decoder_option myproto_options[] = {
    {"bitorder", "dec_myproto_opt_bitorder", "Bit order", NULL, NULL},
    {"cs_polarity", "dec_myproto_opt_cs_polarity", "CS# polarity", NULL, NULL},
};

// 在 srd_c_decoder_entry() 中设置默认值和可选值列表
GSList *bitorder_vals = NULL;
bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("msb"));
bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("lsb"));
myproto_options[0].def = g_variant_new_string("msb");
myproto_options[0].values = bitorder_vals;

GSList *cs_pol_vals = NULL;
cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-low"));
cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-high"));
myproto_options[1].def = g_variant_new_string("active-low");
myproto_options[1].values = cs_pol_vals;

// 在 start() 中读取
const char *bitorder = c_opt_str(di, "bitorder", "msb");
int is_msb = (strcmp(bitorder, "msb") == 0);
```

**布尔选项**：反转数据、显示数据点等

```c
static struct srd_decoder_option myproto_options[] = {
    {"invert_data", "dec_myproto_opt_invert_data", "Invert data line", NULL, NULL},
};

// 在 srd_c_decoder_entry() 中设置默认值
GSList *invert_vals = NULL;
invert_vals = g_slist_append(invert_vals, g_variant_new_string("no"));
invert_vals = g_slist_append(invert_vals, g_variant_new_string("yes"));
myproto_options[0].def = g_variant_new_string("no");
myproto_options[0].values = invert_vals;

// 在 start() 中读取
int invert = c_opt_bool(di, "invert_data", 0);
```

**枚举选项**：模式选择（用字符串选项模拟）

```c
static struct srd_decoder_option myproto_options[] = {
    {"spi_mode", "dec_myproto_opt_spi_mode", "SPI mode (CPOL/CPHA)", NULL, NULL},
};

// 在 srd_c_decoder_entry() 中设置可选值
GSList *mode_vals = NULL;
mode_vals = g_slist_append(mode_vals, g_variant_new_string("0"));  // CPOL=0, CPHA=0
mode_vals = g_slist_append(mode_vals, g_variant_new_string("1"));  // CPOL=0, CPHA=1
mode_vals = g_slist_append(mode_vals, g_variant_new_string("2"));  // CPOL=1, CPHA=0
mode_vals = g_slist_append(mode_vals, g_variant_new_string("3"));  // CPOL=1, CPHA=1
myproto_options[0].def = g_variant_new_string("0");
myproto_options[0].values = mode_vals;

// 在 start() 中读取
int spi_mode = (int)c_opt_int(di, "spi_mode", 0);
int cpol = (spi_mode >> 1) & 1;
int cpha = spi_mode & 1;
```

---

## 5. 测试数据设计方法

解码器开发完成后，需要设计测试数据来验证其正确性。本章介绍如何基于协议规范设计测试向量。

### 5.1 正常帧测试向量

**从规范示例提取测试向量**：

1. 查找规范中的"典型应用"或"通信示例"章节
2. 将示例中的帧序列转换为测试数据
3. 确保覆盖所有帧类型（读、写、应答等）

**最小有效帧**：协议允许的最短帧

```
例如 I²C 最小帧：[START][ADDR+W][ACK][STOP]
```

**典型帧**：包含所有字段的完整帧

```
例如 I²C 完整写帧：[START][ADDR+W][ACK][DATA0][ACK][DATA1][ACK]...[STOP]
```

**多帧序列**：连续多个帧的传输

```
例如 I²C 重复 START：
[START][ADDR+W][ACK][REG_ADDR][ACK][SR][ADDR+R][ACK][DATA][NACK][STOP]
```

### 5.2 错误帧测试

| 错误类型 | 构造方法 | 预期解码器行为 |
|----------|----------|----------------|
| CRC 错误 | 翻转有效帧中的一个位 | 输出 `ANN_CRC_ERR` |
| 格式错误 | 省略停止条件 | 输出 `ANN_FRAMING_ERR` |
| 超时 | 发送不完整帧后停止 | 解码器应回到 IDLE 状态 |
| NACK | 在期望 ACK 的位置发送 NACK | 输出 `ANN_NACK` |
| 非法字段值 | 使用规范中标记为"保留"的值 | 输出 `ANN_WARNING` 或 `ANN_RESERVED` |

### 5.3 边界条件测试

| 边界条件 | 构造方法 | 验证要点 |
|----------|----------|----------|
| 最小帧长 | 0 个数据字节的帧 | 解码器不崩溃，正确输出帧控制注解 |
| 最大帧长 | 协议允许的最大数据长度 | 解码器正确处理所有字节 |
| 全零数据 | 所有数据位为 0 | 数值解析正确 |
| 全一数据 | 所有数据位为 1 | 数值解析正确 |
| 最大地址值 | 地址字段全 1 | 地址解析正确 |
| 最小地址值 | 地址字段全 0 | 地址解析正确 |
| 连续帧 | 无间隔的连续帧传输 | 帧边界正确识别 |
| 极低采样率 | 采样率接近 2× 信号频率 | 解码器仍能工作（奈奎斯特极限） |
| 极高采样率 | 采样率远高于信号频率 | 解码器性能可接受 |

### 5.4 BitstreamBuilder 使用

以下 Python 类用于生成测试用的二进制采样数据文件，可直接加载到 PXView 中进行解码器测试。

```python
class BitstreamBuilder:
    """Build binary sample data for decoder testing.

    Each sample is one byte where each bit represents a channel.
    Channel 0 is the LSB (bit 0), channel 1 is bit 1, etc.
    """

    def __init__(self, num_channels):
        self.data = bytearray()
        self.num_channels = num_channels
        self.current_values = 0  # bitfield of current channel values

    def set_bit(self, channel, value):
        """Set a single channel value in the current sample."""
        if value:
            self.current_values |= (1 << channel)
        else:
            self.current_values &= ~(1 << channel)

    def add_samples(self, count, channel_values=None):
        """Add 'count' samples with current or specified channel values.

        Args:
            count: Number of samples to add.
            channel_values: Optional dict {channel: value} to set before adding.
                            If None, uses current_values.
        """
        if channel_values is not None:
            for ch, val in channel_values.items():
                self.set_bit(ch, val)
        for _ in range(count):
            self.data.append(self.current_values & 0xFF)

    def save(self, path):
        """Save the sample data to a binary file."""
        with open(path, 'wb') as f:
            f.write(self.data)
        print(f"Saved {len(self.data)} samples to {path}")

    def get_data(self):
        """Return the sample data as bytes."""
        return bytes(self.data)
```

**使用示例 1：I²C START + 地址字节**

```python
# I²C: SCL=ch0, SDA=ch1
b = BitstreamBuilder(num_channels=2)

# IDLE: SCL=1, SDA=1
b.add_samples(10, {0: 1, 1: 1})

# START condition: SDA falls while SCL high
b.add_samples(2, {0: 1, 1: 1})   # SCL high, SDA high
b.add_samples(2, {0: 1, 1: 0})   # SCL high, SDA low  ← START

# Address byte: 0x50 (0101 0000) + W bit (0) = 0xA0
# I²C sends MSB first, data changes while SCL low, sampled on SCL rising
addr_byte = 0xA0  # 1010 0000 in binary
for i in range(8):
    bit_val = (addr_byte >> (7 - i)) & 1
    # Data setup while SCL low
    b.add_samples(2, {0: 0, 1: bit_val})
    # SCL rising edge — data sampled here
    b.add_samples(2, {0: 1, 1: bit_val})

# ACK: slave pulls SDA low
b.add_samples(2, {0: 0, 1: 0})  # SCL low, SDA low
b.add_samples(2, {0: 1, 1: 0})  # SCL high, SDA low ← ACK

# STOP condition: SDA rises while SCL high
b.add_samples(2, {0: 0, 1: 0})  # SCL low, SDA low
b.add_samples(2, {0: 1, 1: 0})  # SCL high, SDA low
b.add_samples(2, {0: 1, 1: 1})  # SCL high, SDA high ← STOP

b.add_samples(10, {0: 1, 1: 1})  # IDLE

b.save("i2c_test.bin")
```

**使用示例 2：SPI 传输**

```python
# SPI: SCLK=ch0, MOSI=ch1, CS=ch2
b = BitstreamBuilder(num_channels=3)

# IDLE: SCLK=0, CS=1 (inactive)
b.add_samples(10, {0: 0, 1: 0, 2: 1})

# CS# active (low)
b.add_samples(5, {0: 0, 1: 0, 2: 0})

# SPI Mode 0: data on MOSI, sampled on SCLK rising edge
# Send byte 0x5A = 0101 1010 (MSB first)
data_byte = 0x5A
for i in range(8):
    bit_val = (data_byte >> (7 - i)) & 1
    # Data setup while SCLK low
    b.add_samples(2, {0: 0, 1: bit_val, 2: 0})
    # SCLK rising edge — data sampled
    b.add_samples(2, {0: 1, 1: bit_val, 2: 0})

# CS# inactive (high)
b.add_samples(5, {0: 0, 1: 0, 2: 1})

b.save("spi_test.bin")
```

**使用示例 3：UART 传输**

```python
# UART: TX=ch0
b = BitstreamBuilder(num_channels=1)

# 115200 baud, 1 MHz sample rate → ~8.68 samples per bit
SAMPLES_PER_BIT = 9  # rounded

# IDLE: TX high
b.add_samples(50, {0: 1})

# Send byte 0x55 (0101 0101) with 8N1
# UART: LSB first
data_byte = 0x55

# Start bit (0)
b.add_samples(SAMPLES_PER_BIT, {0: 0})

# Data bits (LSB first)
for i in range(8):
    bit_val = (data_byte >> i) & 1
    b.add_samples(SAMPLES_PER_BIT, {0: bit_val})

# Stop bit (1)
b.add_samples(SAMPLES_PER_BIT, {0: 1})

# IDLE
b.add_samples(50, {0: 1})

b.save("uart_test.bin")
```

**使用示例 4：带 CRC 错误的帧**

```python
# 构造一个 CRC 校验失败的帧
# 先构造正常帧，然后翻转最后一个数据位
b = BitstreamBuilder(num_channels=2)

# ... 构造正常帧的前半部分 ...

# 翻转 CRC 前的一个数据位
b.add_samples(2, {0: 0, 1: 1})  # 故意发送错误的数据位
b.add_samples(2, {0: 1, 1: 1})

# ... 继续发送剩余位（CRC 值按原始正确数据计算） ...

b.save("crc_error_test.bin")
```

---

## 6. API v4 速查表

详细的 API 参考请见 [`c-decoder-guide.md`](c-decoder-guide.md) 第 2 章。

### 核心函数

| API | 签名 | 用途 |
|-----|------|------|
| `c_wait` | `int c_wait(di, ...CW_END)` | 等待采样条件，返回 `SRD_OK` 或错误码 |
| `c_put` | `void c_put(di, ss, es, out, cls, ...)` | 输出注释（多级文本变体） |
| `c_put_v` | `void c_put_v(di, ss, es, out, cls, val, ...)` | 输出带数值的注释（自动生成十六进制） |
| `c_put_t` | `void c_put_t(di, ss, es, out, cls, tp, ...)` | 输出带类型的注释 |
| `c_proto` | `void c_proto(di, ss, es, out, "id", ...C_END)` | 输出协议数据（C_END 终止） |
| `c_pin` | `int c_pin(di, ch)` | 读取通道当前电平（0/1/0xFF） |
| `di_samplenum` | `uint64_t di_samplenum(di)` | 当前采样号 |
| `di_matched` | `uint64_t di_matched(di)` | 条件匹配位掩码 |
| `c_opt_int` | `int64_t c_opt_int(di, key, def)` | 读取整数选项 |
| `c_opt_str` | `const char *c_opt_str(di, key, def)` | 读取字符串选项 |
| `c_opt_dbl` | `double c_opt_dbl(di, key, def)` | 读取浮点选项 |
| `c_opt_bool` | `int c_opt_bool(di, key, def)` | 读取布尔选项（0/1） |
| `c_has_ch` | `int c_has_ch(di, ch)` | 通道是否已连接 |
| `c_samplerate` | `uint64_t c_samplerate(di)` | 采样率（Hz） |
| `c_last_samplenum` | `uint64_t c_last_samplenum(di)` | 上一次 wait 返回的采样号 |
| `c_init_pin` | `uint8_t c_init_pin(di, ch)` | 通道初始引脚值 |
| `c_reg_out` | `int c_reg_out(di, type, "id")` | 注册输出通道，返回输出 ID |
| `c_reg_meta` | `int c_reg_meta(di, type, "id", "typ", "name", "desc")` | 注册元数据输出 |
| `c_put_bin` | `int c_put_bin(di, ss, es, out, cls, size, data)` | 输出二进制数据 |
| `c_put_logic` | `int c_put_logic(di, ss, es, out, mask, vals, n)` | 输出逻辑数据 |
| `c_put_meta_int` | `int c_put_meta_int(di, ss, es, out, val)` | 输出整数元数据 |
| `c_put_meta_dbl` | `int c_put_meta_dbl(di, ss, es, out, val)` | 输出浮点元数据 |

### 条件宏

| 宏 | Python 等价 | 含义 |
|----|-------------|------|
| `CW_H(ch)` | `'h'` | 通道 ch 为高电平 |
| `CW_L(ch)` | `'l'` | 通道 ch 为低电平 |
| `CW_R(ch)` | `'r'` | 通道 ch 上升沿 |
| `CW_F(ch)` | `'f'` | 通道 ch 下降沿 |
| `CW_E(ch)` | `'e'` | 通道 ch 任意边沿 |
| `CW_N(ch)` | `'n'` | 通道 ch 无边沿 |
| `CW_SKIP(n)` | `{'skip': n}` | 跳过 n 个采样 |
| `CW_OR` | 列表分隔 | OR 组分隔符 |
| `CW_END` | — | 条件列表终止符 |

### c_field 构造宏

| 宏 | 类型 | 用途 |
|----|------|------|
| `C_U8(v)` | `uint8_t` | 8 位无符号整数 |
| `C_U16(v)` | `uint16_t` | 16 位无符号整数 |
| `C_U32(v)` | `uint32_t` | 32 位无符号整数 |
| `C_U64(v)` | `uint64_t` | 64 位无符号整数 |
| `C_I8(v)` | `int8_t` | 8 位有符号整数 |
| `C_I16(v)` | `int16_t` | 16 位有符号整数 |
| `C_I32(v)` | `int32_t` | 32 位有符号整数 |
| `C_I64(v)` | `int64_t` | 64 位有符号整数 |
| `C_F64(v)` | `double` | 64 位浮点数 |
| `C_STR(v)` | `const char *` | 字符串 |
| `C_BYTES(d, n)` | `const uint8_t *, uint32_t` | 字节数组 |
| `C_END` | 哨兵 | c_proto() 参数列表终止 |

### 结构体定义宏

| 宏 | 生成内容 |
|----|----------|
| `C_DECODER_STATE(name, fields)` | `name##_s` 类型定义 + `name##_reset()` + `name##_destroy()` |
| `C_DECODER_DEFINE(dec_name, ...)` | `srd_c_decoder` 结构体 + DLL 入口函数 |

### 输出类型常量

| 常量 | 值 | 用途 |
|------|----|------|
| `SRD_OUTPUT_ANN` | 0 | 注释输出 |
| `SRD_OUTPUT_PROTO` | 1 | 协议输出（C↔C 堆叠） |
| `SRD_OUTPUT_BINARY` | 2 | 二进制输出 |
| `SRD_OUTPUT_META` | 3 | 元数据输出 |
| `SRD_OUTPUT_LOGIC` | 4 | 逻辑输出 |

### 通道类型常量

| 常量 | 用途 |
|------|------|
| `SRD_CHANNEL_SCLK` | 时钟通道 |
| `SRD_CHANNEL_SDATA` | 串行数据通道 |
| `SRD_CHANNEL_ADATA` | 模拟数据通道 |
| `SRD_CHANNEL_COMMON` | 通用/控制通道 |

### 回调函数清单

| 回调 | 签名 | 说明 |
|------|------|------|
| `reset` | `void (*reset)(di)` | 重置状态（calloc 初始化） |
| `start` | `void (*start)(di)` | 启动（注册输出、读取选项） |
| `decode` | `void (*decode)(di)` | 主解码循环 |
| `end` | `void (*end)(di)` | 结束（可为 NULL） |
| `metadata` | `void (*metadata)(di, key, value)` | 接收元数据（如采样率） |
| `destroy` | `void (*destroy)(di)` | 释放资源 |
| `decode_upper` | `void (*decode_upper)(di, ss, es, cmd, fields, n)` | 接收上层协议数据 |

### 匹配检查

```c
// 检查第 N 个 OR 组是否匹配
if (di_matched(di) & (1ULL << N)) { ... }

// 常用模式：3 个 OR 组
c_wait(di, CW_R(CLK), CW_OR, CW_H(SCL), CW_F(SDA), CW_OR, CW_H(SCL), CW_R(SDA), CW_END);
if (di_matched(di) & (1ULL << 0)) { /* SCL 上升沿 */ }
if (di_matched(di) & (1ULL << 1)) { /* START 条件 */ }
if (di_matched(di) & (1ULL << 2)) { /* STOP 条件 */ }
```
