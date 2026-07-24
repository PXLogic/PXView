# c-decoder-from-spec

## 元数据

- **Name**: c-decoder-from-spec
- **Description**: 从协议规范/数据手册出发，独立创建 C 协议解码器
- **Trigger**: 用户要求支持新协议、从协议规范创建解码器、提供数据手册要求实现解码器

---

## 概述

本技能指导 LLM 从零开始，仅依据协议规范文档（数据手册、协议标准、时序图），创建一个完整的、可编译运行的 C 协议解码器。整个过程分为 5 个阶段，每个阶段都有结构化的模板和决策树，确保不遗漏任何协议细节。

**核心原则**：
- 协议规范是唯一真相来源，不是 Python 解码器
- 每个设计决策都必须追溯到协议规范中的具体条款
- 状态机必须覆盖协议规范中描述的所有状态和转换
- 测试数据必须从协议时序图精确生成

---

## 阶段 1: 协议分析

目标：从协议规范中提取所有解码器需要的信息，填入结构化模板。

### 1.1 信号线表格模板

从协议规范中提取所有信号线定义，填入下表：

| 信号名 | 方向 | 有效电平 | 通道类型 | 描述 |
|--------|------|----------|----------|------|
| CLK | 主→从 | 上升沿有效 | SRD_CHANNEL_SCLK | 时钟线 |
| DATA | 双向 | 高电平=1 | SRD_CHANNEL_SDATA | 数据线 |
| CS# | 主→从 | 低电平有效 | SRD_CHANNEL_COMMON | 片选线 |

**通道类型说明**：
- `SRD_CHANNEL_SCLK`：时钟信号线（SCLK = Serial Clock）
- `SRD_CHANNEL_SDATA`：串行数据线（SDATA = Serial Data）
- `SRD_CHANNEL_ADATA`：模拟数据线（ADATA = Analog Data）
- `SRD_CHANNEL_COMMON`：控制信号线（如片选 CS#、使能 EN 等）

**填写规则**：
- 必选通道放入 `channels` 数组
- 可选通道放入 `optional_channels` 数组
- 通道顺序（order 字段）从 0 开始递增
- idn 字段使用 `"dec_<decoder_name>_chan_<channel_id>"` 格式（必选）或 `"dec_<decoder_name>_opt_chan_<channel_id>"` 格式（可选）

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

C 代码模板：

```c
static uint8_t compute_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0x00; /* 初始值 */
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07; /* 多项式 */
            else
                crc = crc << 1;
        }
    }
    return crc ^ 0x00; /* xorout */
}
```

### 1.5 选项列表模板

| 选项名 | 类型 | 默认值 | 可选值 | 描述 |
|--------|------|--------|--------|------|
| bitrate | int | 1000000 | - | 比特率(Hz) |
| bit_order | str | "msb" | "msb", "lsb" | 位序 |
| parity | str | "none" | "none", "even", "odd" | 校验模式 |
| cs_polarity | str | "active-low" | "active-low", "active-high" | 片选极性 |

---

## 阶段 2: 解码器设计决策树

根据阶段 1 的分析结果，通过决策树确定解码器的架构。

### 2.1 时钟采样决策

```
协议有时钟线？
├── 是 → 时钟边沿采样
│   ├── 上升沿采样 → c_wait(di, CW_R(CLK), CW_END)
│   └── 下降沿采样 → c_wait(di, CW_F(CLK), CW_END)
└── 否 → 数据边沿触发 / 定时采样
    ├── 数据边沿触发 → c_wait(di, CW_E(DATA), CW_END)
    └── 定时采样 → CW_SKIP(bit_width) + c_pin(di, DATA)
```

### 2.2 片选/使能决策

```
协议有片选/使能线？
├── 是 → CS 有效时处理，CS 无效时等待
│   ├── CS 低电平有效 → c_wait(di, CW_L(CS), CW_END) 等待 CS 变低
│   └── CS 高电平有效 → c_wait(di, CW_H(CS), CW_END) 等待 CS 变高
└── 否 → 无需处理
```

### 2.3 超时决策

```
协议有帧间超时或位间超时？
├── 是 → 使用 CW_SKIP 添加超时
│   └── c_wait(di, CW_R(CLK), CW_OR, CW_SKIP(max_samples), CW_END)
│       检查 di_matched(di) 判断是时钟还是超时
└── 否 → 无需超时
```

### 2.4 堆叠解码器决策

```
解码器需要向上堆叠（被其他解码器使用）？
├── 是 → 实现 c_proto() 协议输出
│   ├── 注册 PROTO 输出 → c_reg_out(di, SRD_OUTPUT_PROTO, "proto_id")
│   ├── 在关键事件输出 → c_proto(di, ss, es, out_proto, "CMD", C_U8(val), C_END)
│   └── 实现 decode_upper() 回调处理上层解码器的协议数据
└── 否 → 无需协议输出

解码器需要向下堆叠（依赖其他解码器的输出）？
├── 是 → inputs 设为上游解码器的 output proto_id
│   ├── inputs = {"i2c", NULL}  /* 输入来自 i2c 解码器 */
│   ├── 实现 decode_upper() 回调接收上游数据
│   └── decode_upper() 签名：
│       void decode_upper(struct srd_decoder_inst *di,
│                          uint64_t start_sample, uint64_t end_sample,
│                          const char *cmd, const c_field *fields, int n_fields)
└── 否 → inputs = {"logic", NULL}  /* 直接输入逻辑信号 */
```

### 2.5 多条件组合决策

```
同一状态需要等待多个可能的条件？
├── 是 → 使用 CW_OR 合并条件组
│   └── c_wait(di, CW_R(CLK), CW_OR, CW_H(CLK), CW_F(SDA), CW_OR, CW_H(CLK), CW_R(SDA), CW_END)
│       条件组编号从 0 开始，通过 di_matched(di) & (1ULL << N) 判断匹配
└── 否 → 单条件等待
    └── c_wait(di, CW_R(CLK), CW_END)
```

**关键规则**：绝不在同一状态中顺序调用多个 c_wait()，必须用 CW_OR 合并。

### 2.6 注解类设计

每个协议字段/事件都应该有自己的 `ANN_*` 枚举值：

```c
enum myproto_ann {
    ANN_START = 0,      /* 起始条件 */
    ANN_STOP = 1,       /* 停止条件 */
    ANN_ADDRESS = 2,    /* 地址字段 */
    ANN_DATA = 3,       /* 数据字段 */
    ANN_ACK = 4,        /* 应答 */
    ANN_ERROR = 5,      /* 错误 */
    ANN_WARNING = 6,    /* 警告 */
    NUM_ANN,            /* 注解总数，必须放在最后 */
};
```

每个注解类必须有长/中/短三级文本变体：

```c
static const char *myproto_ann_labels[][3] = {
    /* 短   中              长 */
    {"S",  "Start",         "Start condition"},        /* ANN_START */
    {"P",  "Stop",          "Stop condition"},         /* ANN_STOP */
    {"A",  "Address",       "Address byte"},           /* ANN_ADDRESS */
    {"D",  "Data",          "Data byte"},              /* ANN_DATA */
    {"A",  "ACK",           "Acknowledge"},            /* ANN_ACK */
    {"!",  "Error",         "Protocol error"},         /* ANN_ERROR */
    {"W",  "Warning",       "Protocol warning"},       /* ANN_WARNING */
};
```

### 2.7 状态结构体设计

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
    /* ... 其他跟踪变量 ... */
});
```

**注意**：如果状态结构体包含需要特殊释放的资源（如 GArray*），需要自定义 reset 和 destroy 函数，并使用 `#pragma GCC diagnostic` 抑制未使用函数警告：

```c
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
C_DECODER_STATE(myproto, {
    /* ... */
    GArray *packet_data;
});
#pragma GCC diagnostic pop

static void myproto_reset_impl(struct srd_decoder_inst *di) {
    myproto_s *old = (myproto_s *)c_decoder_get_private(di);
    if (old) {
        if (old->packet_data)
            g_array_free(old->packet_data, TRUE);
        free(old);
        c_decoder_set_private(di, NULL);
    }
    myproto_s *s = (myproto_s *)calloc(1, sizeof(myproto_s));
    c_decoder_set_private(di, s);
}

static void myproto_destroy_impl(struct srd_decoder_inst *di) {
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);
    if (s) {
        if (s->packet_data)
            g_array_free(s->packet_data, TRUE);
        free(s);
        c_decoder_set_private(di, NULL);
    }
}
```

### 2.8 输出注册设计

在 `start()` 回调中注册所有输出通道：

```c
static void myproto_start(struct srd_decoder_inst *di)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);

    s->out_ann   = c_reg_out(di, SRD_OUTPUT_ANN, "myproto");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "myproto");
    s->out_bin   = c_reg_out(di, SRD_OUTPUT_BINARY, "myproto");

    /* 如果需要 META 输出（如比特率） */
    s->out_meta  = c_reg_out(di, SRD_OUTPUT_META, "myproto");

    /* 读取选项 */
    s->samplerate = c_samplerate(di);
    const char *bit_order = c_opt_str(di, "bit_order", "msb");
    s->msb_first = (strcmp(bit_order, "msb") == 0);
}
```

---

## 阶段 3: 解码器实现

按以下顺序逐步实现解码器。

### 3.1 完整骨架代码模板

以下是一个通用解码器的完整骨架，基于 counter_c.c 模式但泛化：

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== 通道索引定义 ===== */
#define CLK 0
#define DATA 1
/* #define CS 2 */  /* 如果有片选 */

/* ===== 注解类枚举 ===== */
enum myproto_ann {
    ANN_START = 0,
    ANN_ADDRESS = 1,
    ANN_DATA = 2,
    ANN_ACK = 3,
    ANN_ERROR = 4,
    NUM_ANN,
};

/* ===== 状态机枚举 ===== */
enum myproto_state {
    STATE_IDLE,
    STATE_ADDRESS,
    STATE_DATA,
    STATE_ACK,
};

/* ===== 状态结构体 ===== */
C_DECODER_STATE(myproto, {
    enum myproto_state state;
    int bitcount;
    uint8_t databyte;
    uint64_t ss_byte;
    uint64_t samplerate;
    int out_ann;
    int out_proto;
    int out_bin;
});

/* ===== 通道定义 ===== */
static struct srd_channel myproto_channels[] = {
    {"clk",  "CLK",  "Clock line(时钟线)",      0, SRD_CHANNEL_SCLK,  "dec_myproto_chan_clk"},
    {"data", "DATA", "Data line(数据线)",        1, SRD_CHANNEL_SDATA, "dec_myproto_chan_data"},
};

/* static struct srd_channel myproto_optional_channels[] = { */
/*     {"cs", "CS", "Chip select(片选)", 0, SRD_CHANNEL_COMMON, "dec_myproto_opt_chan_cs"}, */
/* }; */

/* ===== 注解标签 ===== */
static const char *myproto_ann_labels[][3] = {
    /* 短   中              长 */
    {"S",  "Start",         "Start condition"},
    {"A",  "Address",       "Address byte"},
    {"D",  "Data",          "Data byte"},
    {"A",  "ACK",           "Acknowledge"},
    {"!",  "Error",         "Protocol error"},
};

/* ===== 注解行 ===== */
static const int myproto_row_ctrl_classes[] = {ANN_START, ANN_ACK, ANN_ERROR, -1};
static const int myproto_row_data_classes[] = {ANN_ADDRESS, ANN_DATA, -1};
static const struct srd_c_ann_row myproto_ann_rows[] = {
    {"control", "Control", myproto_row_ctrl_classes, 3},
    {"data",    "Data",    myproto_row_data_classes, 2},
};

/* ===== 选项定义 ===== */
static struct srd_decoder_option myproto_options[] = {
    {"bit_order", "dec_myproto_opt_bit_order", "Bit order(位序)", NULL, NULL},
};

/* ===== 输入/输出/标签 ===== */
static const char *myproto_inputs[] = {"logic", NULL};
static const char *myproto_outputs[] = {"myproto", NULL};
static const char *myproto_tags[] = {"Embedded/industrial", NULL};

/* ===== reset 回调 ===== */
static void myproto_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, calloc(1, sizeof(myproto_s)));
    }
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);
    memset(s, 0, sizeof(myproto_s));
}

/* ===== start 回调 ===== */
static void myproto_start(struct srd_decoder_inst *di)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);
    s->out_ann   = c_reg_out(di, SRD_OUTPUT_ANN, "myproto");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "myproto");
    s->out_bin   = c_reg_out(di, SRD_OUTPUT_BINARY, "myproto");
    s->samplerate = c_samplerate(di);
}

/* ===== metadata 回调 ===== */
static void myproto_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

/* ===== decode 回调 — 主状态机 ===== */
static void myproto_decode(struct srd_decoder_inst *di)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);

    while (1) {
        int ret;

        switch (s->state) {

        case STATE_IDLE:
            ret = c_wait(di, CW_L(0), CW_END);  /* 等待 CS 变低 */
            if (ret != SRD_OK) return;
            s->state = STATE_ADDRESS;
            s->bitcount = 0;
            s->databyte = 0;
            break;

        case STATE_ADDRESS:
            ret = c_wait(di, CW_R(CLK), CW_END);  /* 时钟上升沿采样 */
            if (ret != SRD_OK) return;
            {
                int bit = c_pin(di, DATA);
                s->databyte = (s->databyte << 1) | bit;
                s->bitcount++;
                if (s->bitcount == 0)
                    s->ss_byte = di_samplenum(di);
                if (s->bitcount >= 8) {
                    char val_str[16];
                    snprintf(val_str, sizeof(val_str), "0x%02X", s->databyte);
                    c_put_v(di, s->ss_byte, di_samplenum(di), s->out_ann,
                            ANN_ADDRESS, s->databyte,
                            "Address", val_str, val_str);
                    c_proto(di, s->ss_byte, di_samplenum(di), s->out_proto,
                            "ADDRESS", C_U8(s->databyte), C_END);
                    s->bitcount = 0;
                    s->databyte = 0;
                    s->state = STATE_DATA;
                }
            }
            break;

        case STATE_DATA:
            ret = c_wait(di, CW_R(CLK), CW_END);
            if (ret != SRD_OK) return;
            {
                int bit = c_pin(di, DATA);
                s->databyte = (s->databyte << 1) | bit;
                s->bitcount++;
                if (s->bitcount == 0)
                    s->ss_byte = di_samplenum(di);
                if (s->bitcount >= 8) {
                    char val_str[16];
                    snprintf(val_str, sizeof(val_str), "0x%02X", s->databyte);
                    c_put_v(di, s->ss_byte, di_samplenum(di), s->out_ann,
                            ANN_DATA, s->databyte,
                            "Data", val_str, val_str);
                    c_proto(di, s->ss_byte, di_samplenum(di), s->out_proto,
                            "DATA", C_U8(s->databyte), C_END);
                    s->bitcount = 0;
                    s->databyte = 0;
                    s->state = STATE_ACK;
                }
            }
            break;

        case STATE_ACK:
            ret = c_wait(di, CW_R(CLK), CW_END);
            if (ret != SRD_OK) return;
            {
                int ack = c_pin(di, DATA);
                if (ack == 0) {
                    c_put(di, di_samplenum(di), di_samplenum(di),
                          s->out_ann, ANN_ACK, "ACK", "A");
                    c_proto(di, di_samplenum(di), di_samplenum(di),
                            s->out_proto, "ACK", C_END);
                } else {
                    c_put(di, di_samplenum(di), di_samplenum(di),
                          s->out_ann, ANN_ERROR, "NACK", "N");
                    c_proto(di, di_samplenum(di), di_samplenum(di),
                            s->out_proto, "NACK", C_END);
                }
                s->state = STATE_IDLE;
            }
            break;
        }
    }
}

/* ===== destroy 回调 ===== */
static void myproto_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== 解码器定义结构体 ===== */
static struct srd_c_decoder myproto_c_decoder = {
    .id = "myproto_c",
    .name = "MyProto(C)",
    .longname = "My Protocol",
    .desc = "My protocol decoder description",
    .license = "gplv2+",
    .channels = myproto_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = myproto_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = myproto_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = myproto_ann_rows,
    .inputs = myproto_inputs,
    .num_inputs = 1,
    .outputs = myproto_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = myproto_tags,
    .num_tags = 1,
    .reset = myproto_reset,
    .start = myproto_start,
    .decode = myproto_decode,
    .end = NULL,
    .metadata = myproto_metadata,
    .destroy = myproto_destroy,
    .decode_upper = NULL,
    .state_size = 0,
};

/* ===== DLL 入口函数 ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    /* 设置选项默认值 */
    GSList *bit_order_vals = NULL;
    bit_order_vals = g_slist_append(bit_order_vals, g_variant_new_string("msb"));
    bit_order_vals = g_slist_append(bit_order_vals, g_variant_new_string("lsb"));
    myproto_options[0].def = g_variant_new_string("msb");
    myproto_options[0].values = bit_order_vals;

    return &myproto_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}
```

### 3.2 start() 回调详解

`start()` 回调在每次解码开始前调用，负责：

1. **注册输出通道**：每种输出类型都需要注册
2. **读取选项**：使用 `c_opt_int`/`c_opt_str`/`c_opt_dbl`/`c_opt_bool`
3. **获取采样率**：`c_samplerate(di)`
4. **初始化非零默认值**：calloc 只能零初始化

```c
static void myproto_start(struct srd_decoder_inst *di)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);

    /* 1. 注册输出 */
    s->out_ann   = c_reg_out(di, SRD_OUTPUT_ANN, "myproto");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "myproto");
    s->out_bin   = c_reg_out(di, SRD_OUTPUT_BINARY, "myproto");

    /* 2. 读取选项 */
    s->samplerate = c_samplerate(di);
    const char *bit_order = c_opt_str(di, "bit_order", "msb");
    s->msb_first = (strcmp(bit_order, "msb") == 0);
    int64_t baud = c_opt_int(di, "baudrate", 9600);
    if (s->samplerate && baud > 0)
        s->bit_width = s->samplerate / baud;

    /* 3. 非零默认值 */
    s->wr = -1;
}
```

### 3.3 decode() 回调详解

`decode()` 是核心状态机，使用 `c_wait()` + `c_pin()` 模式：

```c
static void myproto_decode(struct srd_decoder_inst *di)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);

    while (1) {
        int ret;

        switch (s->state) {
        case STATE_IDLE:
            /* 等待起始条件 */
            ret = c_wait(di, CW_H(CLK), CW_F(DATA), CW_END);
            if (ret != SRD_OK) return;  /* 必须检查返回值！ */
            /* 处理起始条件 */
            s->state = STATE_ADDRESS;
            break;

        case STATE_ADDRESS:
            /* 等待时钟上升沿采样数据 */
            ret = c_wait(di, CW_R(CLK), CW_END);
            if (ret != SRD_OK) return;
            /* 采样数据 */
            int bit = c_pin(di, DATA);
            /* ... 处理位 ... */
            break;
        }
    }
}
```

**关键 API 速查**：

| API | 用途 | 示例 |
|-----|------|------|
| `c_wait(di, ..., CW_END)` | 等待条件 | `c_wait(di, CW_R(CLK), CW_END)` |
| `c_pin(di, ch)` | 读取通道当前值 | `int val = c_pin(di, DATA)` |
| `di_samplenum(di)` | 获取当前采样位置 | `uint64_t ss = di_samplenum(di)` |
| `di_matched(di)` | 获取条件匹配位掩码 | `if (di_matched(di) & 1)` |
| `c_put(di, ss, es, out, cls, ...)` | 输出注解 | `c_put(di, ss, es, out, ANN_DATA, "Data", "D")` |
| `c_put_v(di, ss, es, out, cls, val, ...)` | 输出带数值注解 | `c_put_v(di, ss, es, out, ANN_ADDR, addr, "Addr: %02X", addr_str)` |
| `c_proto(di, ss, es, out, cmd, ..., C_END)` | 输出协议数据 | `c_proto(di, ss, es, out, "DATA", C_U8(val), C_END)` |
| `c_put_bin(di, ss, es, out, cls, size, data)` | 输出二进制 | `c_put_bin(di, ss, es, out, 0, 1, &byte)` |

**c_wait() 条件宏速查**：

| 宏 | 含义 | Python 等价 |
|----|------|-------------|
| `CW_H(ch)` | 通道高电平 | `'h'` |
| `CW_L(ch)` | 通道低电平 | `'l'` |
| `CW_R(ch)` | 通道上升沿 | `'r'` |
| `CW_F(ch)` | 通道下降沿 | `'f'` |
| `CW_E(ch)` | 通道任意边沿 | `'e'` |
| `CW_N(ch)` | 通道无变化 | `'n'` |
| `CW_SKIP(n)` | 跳过 n 个采样 | `{'skip': n}` |
| `CW_OR` | 条件组分隔符 | 列表中下一个 dict |
| `CW_END` | 条件列表终止 | - |

### 3.4 校验计算实现

如果协议包含 CRC 或校验和，在数据收集完成后计算并验证：

```c
static int verify_crc(const uint8_t *data, int len, uint8_t received_crc)
{
    uint8_t crc = 0x00;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc = crc << 1;
        }
    }
    return (crc == received_crc);
}

/* 在 decode() 中使用 */
if (verify_crc(s->packet, s->packet_len, s->crc_byte)) {
    c_put(di, crc_ss, crc_es, s->out_ann, ANN_CRC_OK, "CRC OK");
} else {
    c_put(di, crc_ss, crc_es, s->out_ann, ANN_CRC_ERR, "CRC ERROR");
}
```

### 3.5 decode_upper() 回调（堆叠解码器）

如果解码器需要接收上游解码器的协议输出：

```c
static void myproto_decode_upper(struct srd_decoder_inst *di,
                                  uint64_t start_sample, uint64_t end_sample,
                                  const char *cmd, const c_field *fields, int n_fields)
{
    myproto_s *s = (myproto_s *)c_decoder_get_private(di);

    if (strcmp(cmd, "ADDRESS READ") == 0 && n_fields >= 1 && fields[0].type == C_FIELD_U8) {
        s->address = fields[0].u8;
        /* 处理地址... */
    } else if (strcmp(cmd, "DATA READ") == 0 && n_fields >= 1 && fields[0].type == C_FIELD_U8) {
        s->data = fields[0].u8;
        /* 处理数据... */
    }
}
```

### 3.6 添加到 CMakeLists.txt

在 `CMakeLists.txt` 中找到 `C_DECODERS` 列表，按字母顺序插入新解码器名称：

```cmake
set(C_DECODERS
    ...
    myproto_c
    ...
)
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
  "decoder": "myproto_c",
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
"""Generate test data for myproto_c decoder."""

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
        "decoder": "myproto_c",
        "samplerate": samplerate,
        "num_channels": num_channels,
        "sample_count": sample_count,
        "channels": {"clk": 0, "data": 1, "cs": 2},
    }
    write_test_data("testdata/myproto_c/default", builder, config)

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

---

## 阶段 5: 验证与调试

### 5.1 路径 A: 有 Python 参考解码器

如果项目中已有对应协议的 Python 解码器：

```bash
# 1. 运行测试
cd libsigrokdecode/tests
python run_all_tests.py --decoder myproto_c

# 2. 比较输出
# 实际输出: testdata/myproto_c/default/actual_c.json
# 期望输出: testdata/myproto_c/default/expected_py.json

# 3. 查看差异
python -c "
import json
with open('testdata/myproto_c/default/actual_c.json') as f: c = json.load(f)
with open('testdata/myproto_c/default/expected_py.json') as f: p = json.load(f)
print(f'C annotations: {len(c.get(\"annotations\", []))}')
print(f'Py annotations: {len(p.get(\"annotations\", []))}')
"
```

### 5.2 路径 B: 无 Python 参考解码器（独立验证）

当没有 Python 参考解码器时，需要手动验证：

**步骤 1: 构建并运行解码器**

```bash
# 增量构建
build_incremental.cmd

# 使用 decoder_test.exe 运行
build.dir/decoder_test.exe -d myproto_c -t testdata/myproto_c/default -f testdata/myproto_c/default/actual_c.json --generate-only
```

**步骤 2: 手动验证注解与协议规范一致**

检查 actual_c.json 中的每个注解：
- 帧边界（start_sample, end_sample）是否与协议时序图一致
- 字段值是否与测试数据中的预期值一致
- 校验结果是否正确
- 状态转换是否完整

**步骤 3: 使用已知捕获数据验证**

如果有逻辑分析仪捕获的真实数据：
1. 将真实数据转换为 input.bin 格式
2. 运行解码器
3. 与其他工具（Wireshark、PulseView + Python 解码器）对比

**步骤 4: 编写协议级验证脚本**

```python
#!/usr/bin/env python3
"""Protocol-level verification for myproto_c decoder output."""

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
# 对比 C 和 Python 输出
python -c "
import json
with open('actual_c.json') as f: c = json.load(f)
with open('expected_py.json') as f: p = json.load(f)
c_anns = c.get('annotations', [])
p_anns = p.get('annotations', [])
for i, (ca, pa) in enumerate(zip(c_anns, p_anns)):
    if ca != pa:
        print(f'Deviation at annotation {i}:')
        print(f'  C:  {ca}')
        print(f'  Py: {pa}')
        break
"
```

**步骤 2: 通过 (start_sample, ann_class) 定位 c_put() 调用**

在源代码中搜索对应的 start_sample 值和 ann_class 值，找到产生该注解的 c_put() 调用。

**步骤 3: 检查文本格式**

- 注解文本是否包含长/中/短三级变体？
- 数值格式是否正确（十六进制、十进制、ASCII）？
- c_put_v 的数值参数是否正确？

**步骤 4: 检查采样位置**

- start_sample 和 end_sample 是否与协议时序一致？
- c_wait() 返回后 di_samplenum(di) 是否在正确的采样位置？

**步骤 5: 检查 c_wait() 条件**

- 条件是否与协议时序一致？
- CW_R/CW_F/CW_H/CW_L 是否与协议规范中的边沿/电平定义匹配？
- 多条件时 CW_OR 是否正确分组？

**步骤 6: 检查 CRC/校验计算**

- CRC 多项式是否与协议规范一致？
- 初始值、反射、异或输出是否正确？
- 计算范围是否包含正确的字节？

**步骤 7: 修复、重建、重新测试**

```bash
# 修改源代码后
build_incremental.cmd

# 重新运行测试
cd libsigrokdecode/tests
python run_all_tests.py --decoder myproto_c
```

---

## 质量检查清单

完成解码器后，逐项检查：

- [ ] 通道定义与协议规范一致（名称、类型、顺序）
- [ ] 选项定义覆盖协议变体（位序、极性、校验模式等）
- [ ] 注解覆盖协议所有字段和状态
- [ ] 注解文本包含长/中/短三级变体
- [ ] c_wait() 条件与协议时序一致
- [ ] c_wait() 返回值已检查（`if (ret != SRD_OK) return;`）
- [ ] 无顺序 c_wait() 调用（应合并 CW_OR）
- [ ] c_proto() 以 C_END 结尾
- [ ] CRC/校验计算与协议规范一致
- [ ] 状态机覆盖协议所有状态和转换
- [ ] 构建通过（无编译错误/警告）
- [ ] 测试数据覆盖正常/错误/边界场景
- [ ] 解码器输出与协议规范一致

---

## 完整示例: I2C 协议

以下展示 I2C 协议的完整 5 阶段工作流程。

### 阶段 1: I2C 协议分析

#### 1.1 信号线

| 信号名 | 方向 | 有效电平 | 通道类型 | 描述 |
|--------|------|----------|----------|------|
| SCL | 主→从 | 上升沿采样 | SRD_CHANNEL_SCLK | 串行时钟线 |
| SDA | 双向 | 高=1/低=0 | SRD_CHANNEL_SDATA | 串行数据线 |

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

1. **有时钟线** → `CW_R(SCL)` 上升沿采样
2. **无片选线** → 不需要 CS 处理
3. **需要向上堆叠** → `c_proto()` 输出 I2C 协议包（START/STOP/ADDRESS/DATA/ACK/NACK）
4. **FIND_DATA 状态需多条件** → `CW_OR` 合并：SCL上升沿 OR (SCL高+SDA下降沿=START) OR (SCL高+SDA上升沿=STOP)

### 阶段 3: I2C 关键代码片段

以下展示 i2c_c.c 中的核心实现模式：

#### 3.1 通道和注解定义

```c
#define SCL 0
#define SDA 1

enum i2c_ann {
    ANN_START = 0, ANN_REPEAT_START = 1, ANN_STOP = 2,
    ANN_ACK = 3, ANN_NACK = 4, ANN_BIT = 5,
    ANN_ADDRESS_READ = 6, ANN_ADDRESS_WRITE = 7,
    ANN_DATA_READ = 8, ANN_DATA_WRITE = 9,
    ANN_PACKET = 10, NUM_ANN,
};

static struct srd_channel i2c_channels[] = {
    {"scl", "SCL", "Serial clock line", 0, SRD_CHANNEL_SCLK, NULL},
    {"sda", "SDA", "Serial data line",  1, SRD_CHANNEL_SDATA, NULL},
};
```

#### 3.2 状态机核心

```c
static void i2c_decode(struct srd_decoder_inst *di)
{
    i2c_s *s = (i2c_s *)c_decoder_get_private(di);

    while (1) {
        int ret;
        switch (s->state) {

        case STATE_FIND_START:
            /* 等待 START 条件: SCL高 + SDA下降沿 */
            ret = c_wait(di, CW_H(SCL), CW_F(SDA), CW_END);
            if (ret != SRD_OK) return;
            i2c_handle_start(di, s);
            break;

        case STATE_FIND_ADDRESS:
            /* 等待 SCL 上升沿采样地址位 */
            ret = c_wait(di, CW_R(SCL), CW_END);
            if (ret != SRD_OK) return;
            i2c_handle_address_or_data(di, s);
            break;

        case STATE_FIND_DATA:
            /* 多条件等待: 数据位 OR 重复START OR STOP */
            ret = c_wait(di,
                CW_R(SCL),                    /* 条件0: SCL上升沿 */
                CW_OR,
                CW_H(SCL), CW_F(SDA),         /* 条件1: START */
                CW_OR,
                CW_H(SCL), CW_R(SDA),         /* 条件2: STOP */
                CW_END);
            if (ret != SRD_OK) return;

            if (di_matched(di) & (1ULL << 0))
                i2c_handle_address_or_data(di, s);
            else if (di_matched(di) & (1ULL << 1))
                i2c_handle_start(di, s);
            else if (di_matched(di) & (1ULL << 2))
                i2c_handle_stop(di, s);
            break;

        case STATE_FIND_ACK:
            ret = c_wait(di, CW_R(SCL), CW_END);
            if (ret != SRD_OK) return;
            i2c_get_ack(di, s);
            break;
        }
    }
}
```

#### 3.3 协议输出

```c
/* START 条件输出 */
c_proto(di, samplenum, samplenum, s->out_python, "START", C_END);

/* 地址输出 */
c_proto(di, ss_byte, byte_end, s->out_python,
        s->wr ? "ADDRESS WRITE" : "ADDRESS READ",
        C_U8(d), C_END);

/* 数据输出 */
c_proto(di, ss_byte, byte_end, s->out_python,
        s->wr ? "DATA WRITE" : "DATA READ",
        C_U8(d), C_END);

/* ACK/NACK 输出 */
c_proto(di, samplenum, ack_end, s->out_python, "ACK", C_END);
c_proto(di, samplenum, ack_end, s->out_python, "NACK", C_END);

/* STOP 条件输出 */
c_proto(di, samplenum, samplenum, s->out_python, "STOP", C_END);
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
        "decoder": "i2c_c",
        "samplerate": 1000000,
        "num_channels": 2,
        "sample_count": sample_count,
        "channels": {"scl": 0, "sda": 1},
    }
    # ... 写入文件
```

### 阶段 5: I2C 验证

**有 Python 参考解码器**：

```bash
cd libsigrokdecode/tests
python run_all_tests.py --decoder i2c_c
```

**独立验证**：检查 actual_c.json 中的注解：
1. START 注解出现在 SDA 下降沿位置
2. ADDRESS WRITE 注解值为 0x50（shifted 格式）
3. DATA WRITE 注解值为 0x00 和 0xBE
4. ACK 注解出现在每个字节后
5. STOP 注解出现在 SDA 上升沿位置
6. PACKET 注解包含完整的 "0x50 WR: 00 BE" 格式

---

## 附录: API 速查表

### c_field 类型宏

| 宏 | C 类型 | Python 等价 |
|----|--------|-------------|
| `C_U8(v)` | uint8_t | int |
| `C_U16(v)` | uint16_t | int |
| `C_U32(v)` | uint32_t | int |
| `C_U64(v)` | uint64_t | int |
| `C_I8(v)` | int8_t | int |
| `C_I16(v)` | int16_t | int |
| `C_I32(v)` | int32_t | int |
| `C_I64(v)` | int64_t | int |
| `C_F64(v)` | double | float |
| `C_STR(v)` | const char* | str |
| `C_BYTES(d,n)` | uint8_t*+len | bytes |
| `C_END` | 哨兵 | - |

### 选项读取 API

| API | 返回类型 | 默认值参数 |
|-----|----------|-----------|
| `c_opt_int(di, key, defval)` | int64_t | 整数 |
| `c_opt_dbl(di, key, defval)` | double | 浮点数 |
| `c_opt_str(di, key, defval)` | const char* | 字符串 |
| `c_opt_bool(di, key, defval)` | int | 0/1 |

### 输出注册 API

| API | 用途 |
|-----|------|
| `c_reg_out(di, SRD_OUTPUT_ANN, "id")` | 注册注解输出 |
| `c_reg_out(di, SRD_OUTPUT_PROTO, "id")` | 注册协议输出 |
| `c_reg_out(di, SRD_OUTPUT_BINARY, "id")` | 注册二进制输出 |
| `c_reg_out(di, SRD_OUTPUT_META, "id")` | 注册元数据输出 |

### 注解输出宏

| 宏 | 用途 | 参数 |
|----|------|------|
| `c_put(di, ss, es, out, cls, ...)` | 基本注解 | 文本列表，NULL结尾 |
| `c_put_v(di, ss, es, out, cls, val, ...)` | 带数值注解 | 数值+文本列表 |
| `c_put_t(di, ss, es, out, cls, tp, ...)` | 带类型注解 | 类型+文本列表 |

### srd_c_decoder 结构体字段

| 字段 | 类型 | 说明 |
|------|------|------|
| `.id` | const char* | 解码器ID，必须以 `_c` 结尾 |
| `.name` | const char* | 显示名称 |
| `.longname` | const char* | 完整名称 |
| `.desc` | const char* | 描述 |
| `.license` | const char* | "gplv2+" 或 "gplv3+" |
| `.channels` | srd_channel[] | 必选通道数组 |
| `.num_channels` | int | 必选通道数 |
| `.optional_channels` | srd_channel[] | 可选通道数组（NULL如无） |
| `.num_optional_channels` | int | 可选通道数 |
| `.options` | srd_decoder_option[] | 选项数组 |
| `.num_options` | int | 选项数 |
| `.num_annotations` | int | 注解类数量（= NUM_ANN） |
| `.ann_labels` | char*[][3] | 注解标签（短/中/长） |
| `.num_annotation_rows` | int | 注解行数 |
| `.annotation_rows` | srd_c_ann_row[] | 注解行定义 |
| `.inputs` | const char** | 输入类型列表 |
| `.num_inputs` | int | 输入数量 |
| `.outputs` | const char** | 输出类型列表 |
| `.num_outputs` | int | 输出数量 |
| `.binary` | srd_decoder_binary[] | 二进制输出类 |
| `.num_binary` | int | 二进制输出类数 |
| `.tags` | const char** | 标签列表 |
| `.num_tags` | int | 标签数 |
| `.state_size` | size_t | 0（使用自定义reset/destroy）或 sizeof(state_s) |
| `.reset` | 函数指针 | 重置回调 |
| `.start` | 函数指针 | 开始回调 |
| `.decode` | 函数指针 | 解码回调（主状态机） |
| `.end` | 函数指针 | 结束回调（可NULL） |
| `.metadata` | 函数指针 | 元数据回调（可NULL） |
| `.destroy` | 函数指针 | 销毁回调 |
| `.decode_upper` | 函数指针 | 上层解码回调（可NULL） |
