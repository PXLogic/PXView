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


#ifndef PXVIEW_PV_DATA_ANALOGSNAPSHOT_H
#define PXVIEW_PV_DATA_ANALOGSNAPSHOT_H

#include <libsigrok/libsigrok.h>
#include "../pxvdef.h"
#include "snapshot.h"

#include <utility>
#include <vector>

namespace AnalogSnapshotTest {
class Basic;
}

namespace pv {
namespace data {

class AnalogSnapshot : public Snapshot
{
public:
	struct EnvelopeSample
	{
        // 改为 float 以支持上游 libsigrok 的 float analog 数据。
        // 旧 fork 仅支持 ADC 整数（0-255），用 uint8_t 足够；
        // 上游 demo 驱动发送 float 电压值（-10 到 +10），uint8_t 会截断为 0-255。
        float min;
        float max;
	};

	struct EnvelopeSection
	{
		uint64_t start;
		unsigned int scale;
		uint64_t length;
        uint64_t samples_num;
		EnvelopeSample *samples;
	};

private:
	struct Envelope
	{
		uint64_t length;
        uint64_t ring_length;
        uint64_t count;
		uint64_t data_length;
		EnvelopeSample *samples;
        uint8_t *max;
        uint8_t *min;
	};

private:
	static const unsigned int ScaleStepCount = 10;
	static const int EnvelopeScalePower;
	static const int EnvelopeScaleFactor;
	static const float LogEnvelopeScaleFactor;
	static const uint64_t EnvelopeDataUnit;

    static const uint64_t LeafBlockPower = 21;
    static const uint64_t LeafBlockSamples = 1 << LeafBlockPower;
    static const uint64_t LeafMask = ~(~0ULL << LeafBlockPower);

private:
    void init_all();

public:
    AnalogSnapshot();

	virtual ~AnalogSnapshot();

    void clear();
    void init();

    void copy_from(const AnalogSnapshot &src);

    void first_payload(const sr_datafeed_analog &analog,
                       uint64_t total_sample_count, GSList *channels);

	void append_payload(const sr_datafeed_analog &analog);

    const uint8_t *get_samples(int64_t start_sample);

    void get_envelope_section(EnvelopeSection &s,
        uint64_t start, int64_t count, float min_length, int probe_index);

    int get_ch_order(int sig_index);

    int get_scale_factor();

    bool has_data(int index);
    int get_block_num();
    uint64_t get_block_size(int block_index);

    void* get_data();

    bool has_enabled_channel(int index);

    // 上游 libsigrok analog 数据为 float 电压值时（encoding->is_float），
    // 跟踪实际数据范围供 AnalogSignal 计算 float_scale。
    // 参考 PulseView AnalogSegment::min_value_/max_value_ + perform_autoranging。
    void get_float_min_max(float &min_v, float &max_v) const {
        min_v = _float_min; max_v = _float_max;
    }
    bool has_float_range() const { return _float_range_valid; }

private:
    void append_data(void *data, uint64_t samples, uint16_t pitch);
    // 上游 libsigrok 部分通道数据写入（如 demo 单通道发送）
    void append_data_partial(const sr_datafeed_analog &analog, uint32_t pkt_channels);
    void free_envelop();
	void reallocate_envelope(Envelope &l);
	void append_payload_to_envelope_levels();
    void free_data();
    // 扫描 float 样本更新 _float_min/_float_max（仅 is_float 时调用）
    void update_float_range(const float *data, uint64_t count);

private:
    void *_data;
    struct Envelope _envelope_levels[DS_MAX_ANALOG_PROBES_NUM][ScaleStepCount];
	friend class AnalogSnapshotTest::Basic;
    friend class SessionSnapshot;
    friend class SessionDocument;
    std::vector<int>        _enabled_channel_indexs;
    // 每通道独立写入偏移（demo 逐通道发送时各通道从 0 开始写）
    std::vector<uint64_t>  _per_ch_ring_offset;
    // 每通道 envelope 已处理的样本数（避免对未到达的通道扫描到未初始化的 0）
    std::vector<uint64_t>  _per_ch_env_prev;
    // float 电压数据的实际范围（参考 PulseView AnalogSegment::min_value_/max_value_）
    float   _float_min = 0.0f;
    float   _float_max = 0.0f;
    bool    _float_range_valid = false;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_ANALOGSNAPSHOT_H
