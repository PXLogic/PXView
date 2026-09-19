/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef PXVIEW_PV_DATA_SAMPLE_SPAN_H
#define PXVIEW_PV_DATA_SAMPLE_SPAN_H

#include <cstdint>
#include <cstring>

namespace pv {
namespace data {

// P1-c（统一读取抽象）：一次只读的样本视图。
//
// 目的：把"3 个快照类、5 种读取约定"收敛成一个显式契约，消除调用方各自的
// 手算 stride / reinterpret_cast / 隐式取整假设。**不改变任何存储布局** ——
// 三种布局仍各自保留，统一的是"怎么读"。
//
//   LogicSnapshot   位打包 4 级 mipmap 块树   bits_per_sample=1, stride=1
//                   contiguous_samples 只保证到当前 leaf block 末尾
//                   常量块（无跳变区间）由 is_constant + constant_bit 表达
//   DsoSnapshot     按通道平面                 bits_per_sample=8, stride=1
//                   contiguous_samples 到该通道平面末尾
//   AnalogSnapshot  单块交织                   bits_per_sample=8/16/32
//                   stride = unit_bytes * channel_num
//
// ---- data 的语义（B+C 收口后三种布局统一）----
//
//   **data 一律指向"本通道第一个样本"**。
//
//   AnalogSnapshot::span() 已经把 order * unit_bytes 加进指针（见
//   analogsnapshot.cpp），所以调用方**不要**再自己加通道内偏移 —— 那正是
//   之前 rasterize.cpp 出错的地方：它把内部 order 当通道索引传给 span()，
//   对 _ch_index=[8,9] 的设备 get_ch_order(0) 返回 -1，span 变空、波形静默
//   不绘制。收口后 span() 的 channel 参数**真正影响返回值**，不再是
//   "只校验不生效"。
//
//   需要"整块（所有通道）基址"的调用方用 group_base —— 只有 Analog 与
//   data 不同，DSO/Logic 两者相同。
//
// 旧接口的差异（本结构要抹平的）：
//   - LogicSnapshot::get_samples(order, start, len) 会把 end_sample 当**出参**改写
//     （回写为 leaf block 末尾；等价于 span.start_sample + contiguous_samples - 1）
//   - LogicSnapshot::get_samples 对常量块返回 thread_local **合成缓冲**
//     （全 0x00 / 0xFF）；span 用 is_constant 表达，不需要假指针
//   - DsoSnapshot::get_samples(start, len, order)   参数顺序与 logic **相反**
//   - AnalogSnapshot::get_samples(start)            只给起始指针，stride 与
//     通道内偏移都甩给调用方自己算（通道内偏移现由 span() 代劳）
struct SampleSpan {
    // 本通道第一个样本的地址（三种布局统一语义，见文件头）。
    const uint8_t *data = nullptr;

    // 整块 / 平面基址。
    //   AnalogSnapshot：**样本组**起始（所有通道交织块的起点）——
    //                   需要按 (sample * channel_num + order) 自行遍历时用这个；
    //   DSO / Logic   ：与 data 相同（本身就是按通道平面 / 按通道位打包）。
    // 常量块（logic，data == nullptr）时为 nullptr。
    const uint8_t *group_base = nullptr;

    // data 的第一个样本的绝对索引。
    //
    // 这是本结构最关键的字段：LogicSnapshot 按字节定位，只能给出
    // floor(start/8)*8 处的指针，所以 start_sample 可能比调用方请求的 start
    // 小 0..7。调用方必须按 start_sample（而不是自己请求的 start）做索引运算，
    // 否则位偏移会整体错位 start%8 位。
    uint64_t start_sample = 0;

    // 从 start_sample 起保证连续的样本数。调用方不得越过这个上界读取。
    uint64_t contiguous_samples = 0;

    uint32_t unit_bytes = 1;        // 每样本字节数
    uint32_t stride = 1;            // 相邻样本间的字节距离（交织时 = unit_bytes * channel_num）
    uint8_t  bits_per_sample = 8;   // 1 = 位打包（logic），否则按字节寻址
    uint32_t channel = 0;           // 该 span 所属通道（信号索引，不是内部 order）

    // ---- 位打包（logic）快照的「常量块」表示 ----
    //
    // LogicSnapshot 的 leaf block 在"该区间无跳变"时会被释放（lbp == nullptr），
    // 电平编码在块头的 first 位里。旧 get_samples() 用一个 thread_local 合成缓冲
    // （全 0x00 / 0xFF）表达这件事 —— 那是"有数据的假指针"，且该缓冲是共享的，
    // 调用方无法长期持有。
    //
    // SampleSpan 改为**显式**表达：is_constant == true 时 data == nullptr，
    // 但 span 仍然可读（readable()），取值一律为 constant_bit。
    bool is_constant = false;
    bool constant_bit = false;      // is_constant 时该通道在整段内的恒定电平

    // 严格"有真实数据指针"。**保持不变**：既有调用方都依赖
    // `valid() ⇒ data != nullptr`，不要改语义。
    bool valid() const { return data != nullptr && contiguous_samples > 0; }

    // "本 span 可读"：有真实指针，**或**是常量块。
    // 需要处理常量块（logic 路径）的调用方用这个，并通过 bit_at() 取值 ——
    // 它已经内建常量分支，不要自己去碰 data。
    bool readable() const {
        return (data != nullptr || is_constant) && contiguous_samples > 0;
    }

    bool is_bit_packed() const { return bits_per_sample == 1; }

    uint64_t end_sample() const { return start_sample + contiguous_samples; }

    // 可安全读取的字节数（覆盖到最后一个样本的最后一个字节）。
    uint64_t bytes() const {
        if (contiguous_samples == 0) return 0;
        if (bits_per_sample == 1) return (contiguous_samples + 7) / 8;
        return (contiguous_samples - 1) * static_cast<uint64_t>(stride) + unit_bytes;
    }

    // 样本 s 相对 data 的字节偏移。
    // 前提：s ∈ [start_sample, end_sample())。
    uint64_t byte_offset_of(uint64_t s) const {
        const uint64_t rel = s - start_sample;
        return is_bit_packed() ? rel / 8 : rel * static_cast<uint64_t>(stride);
    }

    // 位打包时样本 s 在对应字节内的位掩码。
    uint8_t bit_mask_of(uint64_t s) const {
        return static_cast<uint8_t>((1u << ((s - start_sample) % 8)));
    }

    // 位打包时取样本 s 的电平。常量块直接返回 constant_bit。
    bool bit_at(uint64_t s) const {
        if (is_constant) return constant_bit;
        return (data[byte_offset_of(s)] & bit_mask_of(s)) != 0;
    }

    // 位打包常量块在"打包字节"约定下的填充值：全 1 -> 0xFF，全 0 -> 0x00
    // （LSB-first，与 LogicSnapshot::get_samples 旧合成缓冲一致）。
    // 仅 is_constant 时有意义。
    uint8_t constant_fill_byte() const {
        return constant_bit ? 0xFF : 0x00;
    }

    // ---- 交织布局（AnalogSnapshot）的样本解码 ----
    //
    // 读样本 s 的值，按 unit_bytes / is_float 解码。**取值域与
    // AnalogSignal 渲染、SessionService::export_binary 的 Analog 分支一致**：
    //   float 编码  -> 电压值 (V)
    //   整数编码    -> 原始整数计数（按 unit_bytes 小端拼接）
    //
    // B+C 收口：data 已指向**本通道第一个样本**（AnalogSnapshot::span() 内部
    // 加过 order * unit_bytes），所以这里不再需要 ch_off 参数 —— 调用方无法
    // 传错通道内偏移。
    //
    // 前提：s ∈ [start_sample, end_sample())。非位打包布局。
    double analog_value_at(uint64_t s, bool is_float) const {
        const uint8_t *p = data + byte_offset_of(s);
        if (is_float && unit_bytes == sizeof(float)) {
            float f;
            std::memcpy(&f, p, sizeof(float));
            return static_cast<double>(f);
        }
        uint64_t iv = 0;
        for (uint32_t b = 0; b < unit_bytes; b++)
            iv |= (static_cast<uint64_t>(p[b])) << (b * 8);
        return static_cast<double>(iv);
    }
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SAMPLE_SPAN_H
