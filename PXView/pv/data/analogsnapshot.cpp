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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
 
#include "analogsnapshot.h"
#include "../pxvdef.h"
#include "../log.h"

using namespace std;

namespace pv {
namespace data {

const int AnalogSnapshot::EnvelopeScalePower = 4;
const int AnalogSnapshot::EnvelopeScaleFactor = 1 << EnvelopeScalePower;
const float AnalogSnapshot::LogEnvelopeScaleFactor =
	logf(EnvelopeScaleFactor);
const uint64_t AnalogSnapshot::EnvelopeDataUnit = 64*1024;	// bytes

AnalogSnapshot::AnalogSnapshot() :
    Snapshot(sizeof(uint16_t), 1, 1)
{
	memset(_envelope_levels, 0, sizeof(_envelope_levels));
    _unit_pitch = 0;
    _data  = nullptr; 
}

AnalogSnapshot::~AnalogSnapshot()
{
    free_envelop();
}

void AnalogSnapshot::free_envelop()
{
    for (unsigned int i = 0; i < _channel_num; i++) {
        for(auto &e : _envelope_levels[i]) {
            if (e.samples)
                free(e.samples);
        }
    }
    memset(_envelope_levels, 0, sizeof(_envelope_levels));
}

void AnalogSnapshot::init()
{
    std::lock_guard<std::mutex> lock(_mutex);
    init_all();
}

void AnalogSnapshot::init_all()
{
    _sample_count = 0;
    _ring_sample_count = 0;
    _memory_failed = false;
    _last_ended = true;
    _float_min = 0.0f;
    _float_max = 0.0f;
    _float_range_valid = false;

    for (unsigned int i = 0; i < _channel_num; i++) {
        for (unsigned int level = 0; level < ScaleStepCount; level++) {
            _envelope_levels[i][level].length = 0;
            _envelope_levels[i][level].ring_length = 0;
            // fix hang issue, count should not be clear
            //_envelope_levels[i][level].count = 0;
            _envelope_levels[i][level].data_length = 0;
        }
    }
}

void AnalogSnapshot::update_float_range(const float *data, uint64_t count)
{
    // 参考 PulseView AnalogSegment::append_payload_to_envelope_levels 的 min/max 跟踪。
    // 仅对 float 电压数据调用，用于 AnalogSignal 计算 float_scale（替代 hw_offset/_scale）。
    if (!data || count == 0)
        return;
    for (uint64_t i = 0; i < count; i++) {
        const float v = data[i];
        if (!_float_range_valid) {
            _float_min = v;
            _float_max = v;
            _float_range_valid = true;
        } else {
            if (v < _float_min) _float_min = v;
            if (v > _float_max) _float_max = v;
        }
    }
}

void AnalogSnapshot::free_data()
{
    Snapshot::free_data();

    if (_data != nullptr){
        free(_data);
        _data = nullptr;
    }
}

void AnalogSnapshot::copy_from(const AnalogSnapshot &src)
{
    std::lock_guard<std::mutex> lock(_mutex);

    free_data();
    free_envelop();

    _capacity = src._capacity;
    _channel_num = src._channel_num;
    _sample_count = src._sample_count;
    _total_sample_count = src._total_sample_count;
    _ring_sample_count = src._ring_sample_count;
    _unit_size = src._unit_size;
    _unit_bytes = src._unit_bytes;
    _unit_pitch = src._unit_pitch;
    _memory_failed = src._memory_failed;
    _last_ended = src._last_ended;
    _samplerate = src._samplerate;
    _ch_index = src._ch_index;
    _enabled_channel_indexs = src._enabled_channel_indexs;

    if (src._data && src._capacity > 0) {
        _data = malloc(src._capacity);
        if (_data) {
            memcpy(_data, src._data, src._capacity);
        } else {
            _memory_failed = true;
        }
    }

    for (unsigned int i = 0; i < src._channel_num; i++) {
        for (unsigned int level = 0; level < ScaleStepCount; level++) {
            const Envelope &src_env = src._envelope_levels[i][level];
            Envelope &dst_env = _envelope_levels[i][level];

            dst_env.length = src_env.length;
            dst_env.ring_length = src_env.ring_length;
            dst_env.count = src_env.count;
            dst_env.data_length = src_env.data_length;
            dst_env.samples = nullptr;
            dst_env.max = nullptr;
            dst_env.min = nullptr;

            if (src_env.count > 0) {
                dst_env.samples = (EnvelopeSample *)malloc(src_env.count * sizeof(EnvelopeSample));
                if (dst_env.samples && src_env.samples) {
                    memcpy(dst_env.samples, src_env.samples, src_env.count * sizeof(EnvelopeSample));
                }
            }
        }
    }
}

void AnalogSnapshot::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    free_data();
    free_envelop();
    init_all();
}

void AnalogSnapshot::first_payload(const sr_datafeed_analog &analog, uint64_t total_sample_count, GSList *channels)
{
    _total_sample_count = total_sample_count;
    // CRITICAL FIX: fork 迁移遗漏 — 从上游 sr_datafeed_analog 的 encoding->unitsize
    // 读取每样本字节数。旧 fork 硬编码为 (8+7)/8=1 字节，但上游 demo 驱动发送
    // 的是 float 数据（4 字节/样本），导致数据解析完全错乱。
    // 上游 unitsize 含义：每个样本（每通道）的字节数（如 float=4, uint16=2）。
    if (analog.encoding && analog.encoding->unitsize > 0) {
        _unit_bytes = analog.encoding->unitsize;
        _is_float = analog.encoding->is_float ? true : false;
    } else {
        // Fallback: 假设 8-bit（仅用于无 encoding 信息的旧路径）
        _unit_bytes = (8 + 7) / 8;
    }
    if (_unit_bytes <= 0) {
        pxv_err("AnalogSnapshot: _unit_bytes<=0, aborting");
        return;
    }
    assert(_unit_bytes <= sizeof(uint64_t));

    _channel_num = 0; // The enabled and disabled channels count.

    for (const GSList *l = channels; l; l = l->next) {
        sr_channel *const probe = (sr_channel*)l->data;

        // TODO: data of disabled channels should not be captured.
        if (probe->type == SR_CHANNEL_ANALOG) {
            _channel_num ++;
        }
    }

    // 防止 _envelope_levels[DS_MAX_ANALOG_PROBES_NUM][ScaleStepCount] 固定数组越界:
    // PXView fork 假设最多 DS_MAX_ANALOG_PROBES_NUM(=4) 个 analog 通道(硬件限制),
    // 但 sr 上游驱动可能返回更多通道。超出时截断并记录警告,避免越界写入破坏
    // 后续成员(_enabled_channel_indexs 等)导致 SIGSEGV。
    if (_channel_num > DS_MAX_ANALOG_PROBES_NUM) {
        pxv_err("AnalogSnapshot: analog channel count %u exceeds DS_MAX_ANALOG_PROBES_NUM=%d, truncating (sr device may report more channels than PXView fork expects)",
                _channel_num, DS_MAX_ANALOG_PROBES_NUM);
        _channel_num = DS_MAX_ANALOG_PROBES_NUM;
    }

    bool isOk = true;
    uint64_t size = _total_sample_count * _channel_num * _unit_bytes + sizeof(uint64_t);

    if (size != _capacity) {
        free_data();
        _total_sample_count = total_sample_count;
        _data = malloc(size);

        if (_data) {
            free_envelop();

            for (unsigned int i = 0; i < _channel_num; i++) {
                uint64_t envelop_count = _total_sample_count / EnvelopeScaleFactor;
                for (unsigned int level = 0; level < ScaleStepCount; level++) {
                    _envelope_levels[i][level].count = envelop_count;

                    if (envelop_count == 0)
                        break;

                    _envelope_levels[i][level].samples = (EnvelopeSample*)malloc(envelop_count * sizeof(EnvelopeSample));
                    
                    if (!_envelope_levels[i][level].samples) {
                        isOk = false;
                        break;
                    }

                    envelop_count = envelop_count / EnvelopeScaleFactor;
                }
                if (!isOk)
                    break;
            }
        }
        else {
            isOk = false;
        }
    }

    if (isOk) {
        _ch_index.clear();
        _enabled_channel_indexs.clear();

        for (const GSList *l = channels; l; l = l->next) {
            sr_channel *const probe = (sr_channel*)l->data;

            // TODO: get the enabled channel index.
            if (probe->type == SR_CHANNEL_ANALOG) {
                _ch_index.push_back(probe->index);

                if (probe->enabled){
                    _enabled_channel_indexs.push_back(probe->index);
                }
            }
        }

        // 初始化每通道独立写入偏移（demo 逐通道发送时各通道从 0 开始写）
        _per_ch_ring_offset.assign(_channel_num, 0);
        _per_ch_env_prev.assign(_channel_num, 0);

        _capacity = size;
        _memory_failed = false;

        // 调试日志：打印 first_payload 关键参数，确认 _total_sample_count 和通道配置
        {
            std::string ch_idx_str, en_idx_str;
            for (size_t k = 0; k < _ch_index.size(); k++) {
                ch_idx_str += std::to_string(_ch_index[k]);
                if (k + 1 < _ch_index.size()) ch_idx_str += ",";
            }
            for (size_t k = 0; k < _enabled_channel_indexs.size(); k++) {
                en_idx_str += std::to_string(_enabled_channel_indexs[k]);
                if (k + 1 < _enabled_channel_indexs.size()) en_idx_str += ",";
            }
            pxv_info("ANALOG_FIRST_PAYLOAD total=%llu ch_num=%u en_num=%zu "
                     "is_float=%d unit_bytes=%u ch_index=[%s] enabled=[%s]",
                     (unsigned long long)_total_sample_count,
                     _channel_num, _enabled_channel_indexs.size(),
                     _is_float ? 1 : 0, _unit_bytes,
                     ch_idx_str.c_str(), en_idx_str.c_str());
        }

        append_payload(analog);
        _last_ended = false;
    }
    else {
        free_data();
        free_envelop();
        _memory_failed = true;
    }
}

void AnalogSnapshot::append_payload(const sr_datafeed_analog &analog)
{
    std::lock_guard<std::mutex> lock(_mutex);

    // CRITICAL FIX: fork 迁移遗漏 — 上游 libsigrok 的 sr_datafeed_analog 通过
    // meaning->channels 指定本包数据属于哪些通道。demo 驱动每个通道单独发送
    // （channels 只含 1 个通道），data 是单通道的 num_samples 个样本。
    // 旧 fork 假设 data 是所有 _channel_num 通道的 interleaved 数据，直接 memcpy
    // 按 _channel_num * _unit_bytes 复制，导致：
    //   1) 读越界（data 只有 num_samples*unitsize 字节，却复制 num_samples*channel_num*unitsize）
    //   2) 数据位置完全错乱（单通道数据被写入 interleaved 全部通道位置）
    //
    // 修复：检查 meaning->channels 的通道数。
    //   - 若包含全部 _channel_num 通道 → interleaved memcpy（原逻辑，兼容多通道驱动）
    //   - 若只包含部分通道 → 逐样本写入对应通道的 interleaved 位置
    uint32_t pkt_channels = 0;
    if (analog.meaning && analog.meaning->channels) {
        for (GSList *l = analog.meaning->channels; l; l = l->next)
            pkt_channels++;
    }

    if (pkt_channels == 0 || pkt_channels == _channel_num) {
        // 全通道 interleaved 数据（原逻辑）或无 channels 信息时 fallback
        // Fork libsigrok's sr_datafeed_analog had a `unit_pitch` field (decimation
        // factor: copy 1 sample every `pitch` samples). Upstream libsigrok does
        // not carry this — stub to 1 (no decimation, packed format).
        append_data(analog.data, analog.num_samples, 1);
    } else {
        // 部分通道数据（如 demo 单通道发送）— 逐通道逐样本写入 interleaved 位置
        append_data_partial(analog, pkt_channels);
    }

	// Generate the first mip-map from the data
    if (analog.num_samples != 0) // guarantee new samples to compute
        append_payload_to_envelope_levels();
}

void AnalogSnapshot::append_data_partial(const sr_datafeed_analog &analog,
                                          uint32_t pkt_channels)
{
    // 上游部分通道数据格式：data 是 pkt_channels 个通道的 interleaved 数据，
    // 每通道 num_samples 个样本。
    //
    // _data 存储格式（interleaved）：
    //   [s0_ch0][s0_ch1]...[s0_chN][s1_ch0][s1_ch1]...[s1_chN]...
    //
    // 关键：demo 驱动逐通道发送（A0 发 N 样本，A1 发 N 样本...），
    // 每个通道的样本都应写入相同的样本索引范围 [0, num_samples)，
    // 只是通道偏移不同。所以每个通道需要独立的写入位置，
    // 不能用全局 _ring_sample_count（那会在 A1 写入时从 A0 的末尾继续）。

    if (!analog.meaning || !analog.meaning->channels || !analog.data) {
        pxv_warn("AnalogSnapshot::append_data_partial: nullptr meaning/channels/data");
        return;
    }

    // 收集本包包含的通道在 _ch_index 中的 order
    std::vector<int> pkt_orders;
    pkt_orders.reserve(pkt_channels);
    for (GSList *l = analog.meaning->channels; l; l = l->next) {
        sr_channel *ch = (sr_channel*)l->data;
        if (!ch) {
            pkt_orders.push_back(-1);
            continue;
        }
        int order = get_ch_order(ch->index);
        if (order < 0) {
            pxv_warn("AnalogSnapshot::append_data_partial: channel index %d not in _ch_index, skipping", ch->index);
        }
        pkt_orders.push_back(order);
    }

    const uint8_t *src = (const uint8_t*)analog.data;
    const uint32_t src_stride = pkt_channels * _unit_bytes;
    const uint32_t dst_stride = _channel_num * _unit_bytes;
    const uint64_t samples = analog.num_samples;

    // 边界安全检查：_per_ch_ring_offset 必须已初始化且大小匹配
    if (_per_ch_ring_offset.size() < _channel_num) {
        pxv_err("AnalogSnapshot::append_data_partial: _per_ch_ring_offset size=%zu < _channel_num=%u, reinitializing",
                _per_ch_ring_offset.size(), _channel_num);
        _per_ch_ring_offset.assign(_channel_num, 0);
    }

    // float 数据范围跟踪（参考 PulseView AnalogSegment::min_value_/max_value_）
    if (_is_float && src) {
        for (uint32_t pc = 0; pc < pkt_channels; pc++) {
            const float *src_float = (const float*)(src + pc * _unit_bytes);
            for (uint64_t i = 0; i < samples; i++) {
                float v = src_float[i * pkt_channels];
                if (!_float_range_valid) {
                    _float_min = v; _float_max = v;
                    _float_range_valid = true;
                } else {
                    if (v < _float_min) _float_min = v;
                    if (v > _float_max) _float_max = v;
                }
            }
        }
    }

    // 每个通道独立的写入偏移（demo 逐通道发送，各通道样本索引从 0 开始）
    static int s_pkt_log_cnt = 0;
    bool dbg_pkt = (s_pkt_log_cnt++ < 40);
    for (uint32_t pc = 0; pc < pkt_channels; pc++) {
        int order = pkt_orders[pc];
        if (order < 0 || order >= (int)_channel_num) continue;
        if (order >= (int)_per_ch_ring_offset.size()) continue;

        uint64_t ch_ring = _per_ch_ring_offset[order];
        const uint8_t *src_ch = src + pc * _unit_bytes;

        // 调试日志：记录前几个样本值，验证数据正确性
        if (dbg_pkt && _is_float && samples > 0) {
            const float *fv = (const float*)(src_ch);
            pxv_info("ANALOG_WRITE ch_order=%d samples=%llu ch_ring_before=%llu "
                     "total=%llu v[0]=%.4f v[1]=%.4f v[2]=%.4f v[last]=%.4f",
                     order, (unsigned long long)samples,
                     (unsigned long long)ch_ring,
                     (unsigned long long)_total_sample_count,
                     fv[0], samples > 1 ? fv[1] : 0.0f,
                     samples > 2 ? fv[2] : 0.0f,
                     fv[samples - 1]);
        }

        for (uint64_t i = 0; i < samples; i++) {
            if (ch_ring >= _total_sample_count) {
                ch_ring = 0;
            }
            uint8_t *dst = (uint8_t*)_data + ch_ring * dst_stride + order * _unit_bytes;
            memcpy(dst, src_ch + i * src_stride, _unit_bytes);
            ch_ring++;
        }

        _per_ch_ring_offset[order] = ch_ring;
    }

    // 更新全局 _sample_count 和 _ring_sample_count（取所有通道最大值）
    // 用于 get_sample_count() 查询和 envelope 计算
    uint64_t max_ch_ring = 0;
    for (size_t i = 0; i < _per_ch_ring_offset.size(); i++) {
        if (_per_ch_ring_offset[i] > max_ch_ring)
            max_ch_ring = _per_ch_ring_offset[i];
    }
    _sample_count = max_ch_ring;
    _ring_sample_count = max_ch_ring;

    // 调试日志：记录每通道写入偏移和全局 _sample_count
    static int s_partial_cnt = 0;
    if (s_partial_cnt++ < 30) {
        std::string off_str;
        for (size_t i = 0; i < _per_ch_ring_offset.size(); i++) {
            off_str += std::to_string(_per_ch_ring_offset[i]);
            if (i + 1 < _per_ch_ring_offset.size()) off_str += ",";
        }
        pxv_info("ANALOG_PARTIAL_DONE max_ring=%llu sample_count=%llu "
                 "total=%llu per_ch=[%s]",
                 (unsigned long long)max_ch_ring,
                 (unsigned long long)_sample_count,
                 (unsigned long long)_total_sample_count,
                 off_str.c_str());
    }
}

void AnalogSnapshot::append_data(void *data, uint64_t samples, uint16_t pitch)
{
    int bytes_per_sample = _unit_bytes * _channel_num;

    // float 数据范围跟踪（参考 PulseView AnalogSegment::min_value_/max_value_）
    // 全通道 interleaved 路径：data 格式 [s0_ch0][s0_ch1]...[s1_ch0]...
    if (_is_float && data && samples > 0) {
        const float *fdata = (const float*)data;
        const uint32_t total_floats = (uint32_t)(samples * _channel_num);
        update_float_range(fdata, total_floats);
    }

    if (pitch <= 1) {
        if (_sample_count + samples < _total_sample_count)
            _sample_count += samples;
        else
            _sample_count = _total_sample_count;

        if (_ring_sample_count + samples >= _total_sample_count) {
            memcpy((uint8_t*)_data + _ring_sample_count * bytes_per_sample,
                data, (_total_sample_count - _ring_sample_count) * bytes_per_sample);
            data = (uint8_t*)data + (_total_sample_count - _ring_sample_count) * bytes_per_sample;
            _ring_sample_count = (samples + _ring_sample_count - _total_sample_count) % _total_sample_count;
            memcpy((uint8_t*)_data,
                data, _ring_sample_count * bytes_per_sample);
        } else {
            memcpy((uint8_t*)_data + _ring_sample_count * bytes_per_sample,
                data, samples * bytes_per_sample);
            _ring_sample_count += samples;
        }
    }
    else {
        while(samples--) {
            if (_unit_pitch == 0) {
                if (_sample_count < _total_sample_count)
                    _sample_count++;
                memcpy((uint8_t*)_data + _ring_sample_count * bytes_per_sample,
                    data, bytes_per_sample);
                data = (uint8_t*)data + bytes_per_sample*pitch;
                _ring_sample_count = (_ring_sample_count + 1) % _total_sample_count;
                _unit_pitch = pitch;
            }
            _unit_pitch--;
        }
    }
}

const uint8_t* AnalogSnapshot::get_samples(int64_t start_sample)
{
	if (start_sample < 0 || start_sample >= (int64_t)get_sample_count()) {
		pxv_warn("AnalogSnapshot::get_samples: start_sample %lld out of range (count=%llu)",
		         (long long)start_sample, (unsigned long long)get_sample_count());
		return nullptr;
	}

    return (uint8_t*)_data + start_sample * _unit_bytes * _channel_num;
}

void AnalogSnapshot::get_envelope_section(EnvelopeSection &s,
    uint64_t start, int64_t count, float min_length, int probe_index)
{
    if (count < 0) {
        pxv_warn("AnalogSnapshot::get_envelope_section: negative count %lld, aborting",
                 (long long)count);
        s.length = 0;
        return;
    }
    if (min_length <= 0) {
        pxv_warn("AnalogSnapshot::get_envelope_section: non-positive min_length %f, aborting",
                 min_length);
        s.length = 0;
        return;
    }

    const unsigned int min_level = max((int)floorf(logf(min_length) /
            LogEnvelopeScaleFactor) - 1, 0);
    const unsigned int scale_power = (min_level + 1) * EnvelopeScalePower;
	start >>= scale_power;

    s.start = start;
    s.scale = (1 << scale_power);
    s.length = (count >> scale_power);
    s.samples_num = _envelope_levels[probe_index][min_level].length;
    s.samples = _envelope_levels[probe_index][min_level].samples;

    // 调试日志：记录 envelope level 选择
    static int s_env_section_cnt = 0;
    if (s_env_section_cnt++ < 20) {
        pxv_info("ENV_SECTION ch=%d min_length=%.4f min_level=%u scale=%u "
                 "s.start=%llu s.length=%llu s.samples_num=%llu e.length=%llu",
                 probe_index, min_length, min_level, s.scale,
                 (unsigned long long)s.start, (unsigned long long)s.length,
                 (unsigned long long)s.samples_num,
                 (unsigned long long)_envelope_levels[probe_index][min_level].length);
    }
}

void AnalogSnapshot::reallocate_envelope(Envelope &e)
{
	const uint64_t new_data_length = ((e.length + EnvelopeDataUnit - 1) /
		EnvelopeDataUnit) * EnvelopeDataUnit;
    if (new_data_length > e.data_length)
	{
		e.data_length = new_data_length;
	}
}

void AnalogSnapshot::append_payload_to_envelope_levels()
{
    if (_data == nullptr) return;

    // 兼容旧路径（全通道 interleaved）：
    //   若 _per_ch_ring_offset 为空（未走 append_data_partial 路径），
    //   则所有通道共享同一个 _sample_count / _ring_sample_count 进度。
    bool use_per_ch = !_per_ch_ring_offset.empty();
    if (_per_ch_env_prev.empty() && use_per_ch) {
        _per_ch_env_prev.assign(_channel_num, 0);
    }

    int i;
    for (i = 0; i < (int)_channel_num; i++) {
        Envelope &e0 = _envelope_levels[i][0];
        uint64_t prev_length;
        EnvelopeSample *dest_ptr;

        // 用每通道独立的进度（demo 逐通道发送时各通道独立写入）
        // 而非全局 _sample_count（那是所有通道的最大值，会扫描到尚未到达的通道的未初始化 0）
        uint64_t ch_sample_count = use_per_ch ? _per_ch_ring_offset[i] : _sample_count;
        uint64_t ch_ring_count   = use_per_ch ? _per_ch_ring_offset[i] : _ring_sample_count;

        // FIX: 参考 PulseView AnalogSegment::append_payload_to_envelope_levels，
        // 使用线性计算（prev_length 跟踪 e0.length 的上一次值，而非环形 ring_length）。
        //
        // 旧代码用环形 ring_length 计算 e0_sample_num：
        //   (e0.ring_length > prev_length) ? e0.ring_length - prev_length
        //                                  : e0.ring_length + (total/ESF) - prev_length
        // 当 demo 驱动逐通道发送（其他通道数据包到达时当前通道 ch_ring_count 不变），
        // e0.ring_length == prev_length，条件为 false 走 else 分支，算出
        //   ring_length + total/ESF - prev_length = 63 + 312 - 63 = 312
        // 但实际无新样本（应为 0），导致读取越界数据并覆盖正确 envelope，
        // 迷你波形显示错误甚至 PXView 卡死（CPU 70%+）。
        prev_length = e0.length;
        e0.length = ch_sample_count / EnvelopeScaleFactor;
        e0.ring_length = ch_ring_count / EnvelopeScaleFactor;

        // 没有新样本需要计算（其他通道的数据包到达时当前通道无新增数据）
        if (e0.length <= prev_length) {
            continue;
        }

        // 调试日志：记录 envelope level-0 生成
        static int s_env_log_cnt = 0;
        bool dbg_env = (s_env_log_cnt++ < 30);
        if (dbg_env) {
            pxv_info("ENV_GEN_L0 ch=%d ch_sample_count=%llu ch_ring_count=%llu "
                     "prev_length=%llu e0.length=%llu ESF=%d _channel_num=%u "
                     "_unit_bytes=%u _ch_index_size=%zu",
                     i, (unsigned long long)ch_sample_count,
                     (unsigned long long)ch_ring_count,
                     (unsigned long long)prev_length,
                     (unsigned long long)e0.length,
                     EnvelopeScaleFactor, _channel_num,
                     _unit_bytes, _ch_index.size());
        }

        dest_ptr = e0.samples + prev_length;

        // 线性遍历原始数据：从 prev_length*ESF 到 e0.length*ESF
        // （数据在非 loop 模式下线性写入 _data，无需环形回绕）
        const uint64_t start_sample = prev_length * EnvelopeScaleFactor;
        const uint64_t end_sample = e0.length * EnvelopeScaleFactor;
        uint8_t *src_ptr = (uint8_t*)_data +
                    (start_sample * _channel_num + i) * _unit_bytes;

        // 调试日志：记录前 2 个 envelope 样本的详细读取值
        bool dbg_env_detail = dbg_env && prev_length == 0;
        for (uint64_t j = start_sample; j < end_sample; j += EnvelopeScaleFactor) {
            EnvelopeSample sub_sample;
            // float 数据：用 reinterpret_cast 读取；整数数据：按 unit_bytes 拼接
            if (_is_float && _unit_bytes == sizeof(float)) {
                sub_sample.min = *reinterpret_cast<const float*>(src_ptr);
                sub_sample.max = sub_sample.min;
                src_ptr += _channel_num * _unit_bytes;
                for (int k = 1; k < EnvelopeScaleFactor; k++) {
                    float v = *reinterpret_cast<const float*>(src_ptr);
                    sub_sample.min = min(sub_sample.min, v);
                    sub_sample.max = max(sub_sample.max, v);
                    src_ptr += _channel_num * _unit_bytes;
                }

                // 调试日志：记录前 2 个 envelope 样本读取的 16 个 float 值
                if (dbg_env_detail && (j == start_sample || j == start_sample + EnvelopeScaleFactor)) {
                    uint8_t *dbg_ptr = (uint8_t*)_data +
                        (j * _channel_num + i) * _unit_bytes;
                    pxv_info("ENV_READ ch=%d env_idx=%llu src_offset=%llu "
                             "v[0]=%.4f v[1]=%.4f v[5]=%.4f v[10]=%.4f v[15]=%.4f "
                             "min=%.4f max=%.4f",
                             i, (unsigned long long)(j / EnvelopeScaleFactor),
                             (unsigned long long)((j * _channel_num + i) * _unit_bytes),
                             *(float*)dbg_ptr,
                             *(float*)(dbg_ptr + _channel_num * _unit_bytes),
                             *(float*)(dbg_ptr + 5 * _channel_num * _unit_bytes),
                             *(float*)(dbg_ptr + 10 * _channel_num * _unit_bytes),
                             *(float*)(dbg_ptr + 15 * _channel_num * _unit_bytes),
                             sub_sample.min, sub_sample.max);
                }
            } else {
                sub_sample.min = (float)(*src_ptr);
                sub_sample.max = sub_sample.min;
                src_ptr += _channel_num * _unit_bytes;
                for (int k = 1; k < EnvelopeScaleFactor; k++) {
                    sub_sample.min = min(sub_sample.min, (float)(*src_ptr));
                    sub_sample.max = max(sub_sample.max, (float)(*src_ptr));
                    src_ptr += _channel_num * _unit_bytes;
                }
            }

            *dest_ptr++ = sub_sample;
        }

        // 调试日志：记录生成的前几个 envelope 样本
        if (dbg_env && e0.length > 0) {
            pxv_info("ENV_SAMPLES ch=%d e0.length=%llu s[0].min=%.4f s[0].max=%.4f "
                     "s[1].min=%.4f s[1].max=%.4f s[last].min=%.4f s[last].max=%.4f",
                     i, (unsigned long long)e0.length,
                     e0.samples[0].min, e0.samples[0].max,
                     e0.length > 1 ? e0.samples[1].min : 0.0f,
                     e0.length > 1 ? e0.samples[1].max : 0.0f,
                     e0.samples[e0.length - 1].min,
                     e0.samples[e0.length - 1].max);
        }

        // Compute higher level mipmaps
        // FIX: 参考 PulseView AnalogSegment::append_payload_to_envelope_levels，
        // 使用纯线性迭代（无环形回绕）。旧代码的 dest_ptr/src_ptr 回绕逻辑在
        // e.count == e.length 时（深层 mipmap，如 count=1）会导致无限循环：
        //   *dest_ptr++ → dest_ptr = e.samples + 1
        //   if (dest_ptr >= e.samples + e.count) → true (1 >= 1)
        //   dest_ptr = e.samples → 回到起点 → while(dest_ptr < end_dest_ptr) 永远为 true
        for (unsigned int level = 1; level < ScaleStepCount; level++)
        {
            Envelope &e = _envelope_levels[i][level];
            const Envelope &el = _envelope_levels[i][level-1];

            // prev_length 必须在赋新值之前保存（跟踪上一次的 e.length）
            prev_length = e.length;
            e.length = el.length / EnvelopeScaleFactor;
            e.ring_length = el.ring_length / EnvelopeScaleFactor;

            // 没有新样本需要计算
            if (e.length == prev_length)
                break;

            // 线性子采样：从 el.samples[prev_length*ESF] 开始读 ESF 个样本，
            // 降采样为 1 个样本写入 e.samples[prev_length]
            const EnvelopeSample *src_ptr =
                el.samples + prev_length * EnvelopeScaleFactor;
            const EnvelopeSample *const end_dest_ptr = e.samples + e.length;

            for (dest_ptr = e.samples + prev_length;
                 dest_ptr < end_dest_ptr; dest_ptr++) {
                const EnvelopeSample *const end_src_ptr =
                    src_ptr + EnvelopeScaleFactor;

                EnvelopeSample sub_sample = *src_ptr++;
                while (src_ptr < end_src_ptr) {
                    sub_sample.min = min(sub_sample.min, src_ptr->min);
                    sub_sample.max = max(sub_sample.max, src_ptr->max);
                    src_ptr++;
                }

                *dest_ptr = sub_sample;
            }
        }
    }
}

int AnalogSnapshot::get_ch_order(int sig_index)
{
    uint16_t order = 0;
    for (auto& iter:_ch_index) {
        if (iter == sig_index)
            break;
        order++;
    }

    if (order >= _ch_index.size())
        return -1;
    else
        return order;
}

int AnalogSnapshot::get_scale_factor()
{
    return EnvelopeScaleFactor;
}

bool AnalogSnapshot::has_data(int index)
{
    for (int iter : _ch_index) {
        if (iter == index)
            return true;
    }
    return false;
}

int AnalogSnapshot::get_block_num()
{
    const uint64_t size = _sample_count * get_unit_bytes() * get_channel_num();
    return (size >> LeafBlockPower) +
           ((size & LeafMask) != 0);
}

uint64_t AnalogSnapshot::get_block_size(int block_index)
{
    assert(block_index < get_block_num());

    if (block_index < get_block_num() - 1) {
        return LeafBlockSamples;
    } else {
        const uint64_t size = _sample_count * get_unit_bytes() * get_channel_num();
        if (size % LeafBlockSamples == 0)
            return LeafBlockSamples;
        else
            return size % LeafBlockSamples;
    }
}

void* AnalogSnapshot::get_data()
{
    return _data;
}

bool AnalogSnapshot::has_enabled_channel(int index)
{

    for (int iter : _enabled_channel_indexs) {
        if (iter == index)
            return true;
    }
    return false; 
}

} // namespace data
} // namespace pv
