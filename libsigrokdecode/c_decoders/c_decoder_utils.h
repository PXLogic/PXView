#ifndef C_DECODER_UTILS_H
#define C_DECODER_UTILS_H

#include "libsigrokdecode.h"

/*
 * BITS message format (v2, with per-bit timestamps)
 *
 * Used by c_decoder_put_python() with cmd="BITS" in SPI and I2C decoders.
 * Upper-layer protocol decoders (recv_proto) must parse this format to
 * extract individual bit values and their sample-level timestamps.
 *
 * Layout:
 *   data[0]                       = have_mosi (bit0) | have_miso (bit1)
 *   data[1]                       = mosi_bit_count (uint8_t)
 *   data[2 .. 2+mosi_count*17-1]  = MOSI bits, each 17 bytes:
 *       [value(1B)][start_sample(8B LE)][end_sample(8B LE)]
 *   data[2+mosi_count*17]         = 0x00 (reserved / alignment)
 *   data[2+mosi_count*17+1]       = miso_bit_count (uint8_t)
 *   data[2+mosi_count*17+2 ..]    = MISO bits, each 17 bytes:
 *       [value(1B)][start_sample(8B LE)][end_sample(8B LE)]
 *
 * Byte sizes per bit entry: 1 (value) + 8 (ss LE) + 8 (es LE) = 17 bytes.
 *
 * SPI: both MOSI and MISO sections may be present (have_mosi=1, have_miso=1).
 * I2C: only MISO section is used (have_mosi=0, have_miso=1), mosi_count=0,
 *      all 8 bits go in the miso section in wire order (MSB first).
 *
 * Parsing example (C):
 *   uint8_t flags = data[0];
 *   int have_mosi = flags & 1;
 *   int have_miso = (flags >> 1) & 1;
 *   int mosi_cnt = data[1];
 *   int pos = 2;
 *   for (int i = 0; i < mosi_cnt; i++) {
 *       uint8_t val = data[pos];
 *       uint64_t ss = 0, es = 0;
 *       memcpy(&ss, data + pos + 1, 8);   // LE on LE host
 *       memcpy(&es, data + pos + 9, 8);
 *       pos += 17;
 *   }
 *   pos++;  // skip reserved byte
 *   int miso_cnt = data[pos++];
 *   for (int i = 0; i < miso_cnt; i++) {
 *       uint8_t val = data[pos];
 *       uint64_t ss = 0, es = 0;
 *       memcpy(&ss, data + pos + 1, 8);
 *       memcpy(&es, data + pos + 9, 8);
 *       pos += 17;
 *   }
 */

/*
 * C_ANN_PUT_TYPE — 带ann_type的注解输出宏
 *
 * 用途：当需要指定ann_type（如不同颜色行）时使用。C_ANN_PUT默认ann_type=0，
 *      若同一ann_class需要多种显示样式（如不同颜色行），可用此宏指定ann_type。
 *
 * 参数：
 *   di         — 解码器实例指针
 *   ss         — 起始采样号 (start_sample)
 *   es         — 结束采样号 (end_sample)
 *   out_ann    — 注解输出ID（由c_decoder_register_output注册得到）
 *   ann_class  — 注解类别索引（对应decoder定义的annotation rows）
 *   ann_type   — 注解类型索引（同一ann_class下的子类型，用于区分颜色行等）
 *   ...        — 格式化字符串及参数（如 "text" 或 "0x%02X", val）
 *
 * 示例：
 *   C_ANN_PUT_TYPE(di, ss, es, out_ann, 0, 7, "text");
 *   // ann_class=0, ann_type=7 — 使用第0类注解的第7种样式输出
 */

/*
 * C_ANN_PUT_VAL — 带数值的注解输出宏
 *
 * 用途：当注解需要关联数值（如寄存器地址、测量值）时使用。GUI可据此显示
 *      十六进制值等格式化内容。该宏会自动填充str_number_hex和numberic_value字段。
 *
 * 参数：
 *   di         — 解码器实例指针
 *   ss         — 起始采样号
 *   es         — 结束采样号
 *   out_ann    — 注解输出ID
 *   ann_class  — 注解类别索引
 *   val        — 关联的数值（如寄存器地址）
 *   ...        — 格式化字符串及参数
 *
 * 示例：
 *   C_ANN_PUT_VAL(di, ss, es, out_ann, 0, reg_addr, "REG 0x%02X", reg_addr);
 *   // 自动填充str_number_hex和numberic_value，GUI可显示十六进制值
 */

/*
 * c_decoder_get_initial_pin — 获取初始引脚状态
 *
 * 用途：等效于Python解码器的self.initial_pins。在start()或decode()开头
 *      获取用户设置的初始引脚电平，用于确定上电默认状态（如CS默认高电平）。
 *
 * 参数：
 *   di         — 解码器实例指针
 *   channel    — 通道索引（对应decoder定义的channels数组下标）
 *
 * 返回值：
 *   0    — LOW（低电平）
 *   1    — HIGH（高电平）
 *   0xFF — 未设置或无效通道
 *
 * 示例：
 *   uint8_t init_cs = c_decoder_get_initial_pin(di, cs_channel);
 *   if (init_cs == 0xFF) init_cs = 1; // 默认高电平
 */

/*
 * c_decoder_get_last_samplenum — 获取最后一个采样号
 *
 * 用途：在end()回调中获取数据结束位置，用于输出最终标注（如帧结束标记、
 *      总线空闲区间等）。在decode()中通常不需要调用，因为条件匹配已提供
 *      当前采样位置。
 *
 * 参数：
 *   di         — 解码器实例指针
 *
 * 返回值：
 *   最后一个有效采样的编号（uint64_t）
 *
 * 示例：
 *   uint64_t last = c_decoder_get_last_samplenum(di);
 *   C_ANN_PUT(di, frame_start, last, out_ann, 0, "FRAME END");
 */

/*
 * c_cond_noedge — 无边沿条件
 *
 * 用途：等效于Python解码器的self.wait({'ch': 'n'})，等待某通道无变化
 *      （即保持当前电平不变）。通常在c_cond_or组合中使用，例如：
 *      "等待CLK上升沿 OR CS无变化（即CS持续高电平）"。
 *
 * 说明：单独使用c_cond_noedge无实际意义（任何采样点都满足"无变化"），
 *      应与c_cond_rise/fall等边沿条件组合为c_cond_or使用，表示
 *      "等待某事件发生，同时另一通道保持不变"。
 *
 * 示例（与c_cond_or组合）：
 *   c_cond_or or_cond;
 *   c_cond_or_init(&or_cond);
 *   c_cond_or_add(&or_cond, c_cond_rise(di, clk_ch));
 *   c_cond_or_add(&or_cond, c_cond_noedge(di, cs_ch));
 *   // 等待CLK上升沿，同时CS保持不变
 */

/*
 * c_decoder_put_logic — 输出逻辑通道数据
 *
 * 用途：用于输出GPIO等逻辑信号状态（如PCA9571的8个GPIO通道）。上层解码器
 *      可通过recv_logic回调接收这些逻辑数据，实现解码器级联。
 *
 * 参数：
 *   di            — 解码器实例指针
 *   ss            — 起始采样号
 *   es            — 结束采样号
 *   out_logic     —
 * 逻辑输出ID（由c_decoder_register_output注册，类型SRD_OUTPUT_LOGIC）
 *   channel_mask  — 通道掩码，bit=1表示该通道有值（如0xFF表示8个通道都有值）
 *   values        — 指向逻辑电平数据的指针，每个字节对应一个通道（0或1）
 *   num_channels  — 通道总数
 *
 * 示例（PCA9571 GPIO输出）：
 *   // 注册输出：
 *   int out_logic = c_decoder_register_output(di, SRD_OUTPUT_LOGIC, "pca9571");
 *
 *   // 输出8个GPIO通道状态：
 *   uint8_t values = gpio_state;  // 每bit代表一个GPIO通道电平
 *   c_decoder_put_logic(di, ss, es, out_logic, 0xFF, &values, 8);
 *   // channel_mask=0xFF表示8个通道都有值
 */

#endif
