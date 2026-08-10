/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
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
 
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"

using namespace std;

namespace pv {
namespace data {

const int DsoSnapshot::EnvelopeScalePower = 8;
const int DsoSnapshot::EnvelopeScaleFactor = 1 << EnvelopeScalePower;
const float DsoSnapshot::LogEnvelopeScaleFactor =
	logf(EnvelopeScaleFactor);
const uint64_t DsoSnapshot::EnvelopeDataUnit = 4*1024;	// bytes

const int DsoSnapshot::VrmsScaleFactor = 1 << 8;

DsoSnapshot::DsoSnapshot() :
    Snapshot(sizeof(uint16_t), 1, 1)
{   
    _envelope_en = false;
    _envelope_done = false;
    _instant = false;
    _threshold = 0;
    _measure_voltage_factor1 = 0;
    _measure_voltage_factor2 = 0;
    _data_scale1 = 0;
    _data_scale2 = 0;  
    _is_file = false;
    _ref_min = 0;
    _ref_max = 0;
    _data_out_off_range = false;

	memset(_envelope_levels, 0, sizeof(_envelope_levels));
}

DsoSnapshot::~DsoSnapshot()
{
    // PulseView pattern: derived destructor explicitly frees its own data.
    // The base Snapshot::~Snapshot() also calls free_data(), but C++
    // [class.dtor]/12 causes virtual dispatch to resolve to the base
    // version during base destruction — DsoSnapshot::free_data()
    // (which frees _ch_data) would never be called. Calling it here in
    // the derived destructor body (where the vtable is still
    // DsoSnapshot's) ensures _ch_data elements are properly freed.
    free_data();
    free_envelop();
}

void DsoSnapshot::free_envelop()
{
    for (unsigned int i = 0; i < _channel_num; i++) {
        for(auto &e : _envelope_levels[i]) {
            if (e.samples)
                free(e.samples);
        }
    }
    memset(_envelope_levels, 0, sizeof(_envelope_levels));
}

void DsoSnapshot::init()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    init_all();    
}

void DsoSnapshot::init_all()
{
    _sample_count = 0;
    _ring_sample_count = 0;
    _memory_failed = false;
    _last_ended = true;
    _envelope_done = false;   
    _is_file = false; 

    for (unsigned int i = 0; i < _channel_num; i++) {
        for (unsigned int level = 0; level < ScaleStepCount; level++) {
            _envelope_levels[i][level].length = 0;
            _envelope_levels[i][level].data_length = 0;
        }
    }
}

void DsoSnapshot::clear()
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    free_data();
    free_envelop();
    init_all();
    _envelope_en = false;
}

void DsoSnapshot::copy_from(const DsoSnapshot &src)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

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
    _memory_failed = src._memory_failed.load();
    _last_ended = src._last_ended.load();
    _samplerate = src._samplerate.load();
    _ch_index = src._ch_index;

    for (size_t i = 0; i < src._ch_data.size(); i++) {
        uint8_t *chan_buffer = (uint8_t *)malloc(src._total_sample_count + 1);
        if (chan_buffer) {
            memcpy(chan_buffer, src._ch_data[i], src._total_sample_count + 1);
        } else {
            _memory_failed = true;
        }
        _ch_data.push_back(chan_buffer);
    }

    for (unsigned int i = 0; i < src._channel_num; i++) {
        uint64_t envelop_count = src._total_sample_count / EnvelopeScaleFactor;

        for (unsigned int level = 0; level < ScaleStepCount; level++) {
            const Envelope &src_env = src._envelope_levels[i][level];
            Envelope &dst_env = _envelope_levels[i][level];

            envelop_count = ((envelop_count + EnvelopeDataUnit - 1) / EnvelopeDataUnit) * EnvelopeDataUnit;
            uint64_t buffer_len = envelop_count * sizeof(EnvelopeSample);

            dst_env.length = src_env.length;
            dst_env.data_length = src_env.data_length;
            dst_env.samples = nullptr;

            if (buffer_len > 0) {
                dst_env.samples = (EnvelopeSample *)malloc(buffer_len);
                if (dst_env.samples && src_env.samples && src_env.data_length > 0) {
                    memcpy(dst_env.samples, src_env.samples, src_env.data_length * sizeof(EnvelopeSample));
                }
            }

            envelop_count = envelop_count / EnvelopeScaleFactor;
        }
    }

    _envelope_en = src._envelope_en;
    _envelope_done = src._envelope_done;
    _instant = src._instant;
    _threshold = src._threshold;
    _measure_voltage_factor1 = src._measure_voltage_factor1;
    _measure_voltage_factor2 = src._measure_voltage_factor2;
    _data_scale1 = src._data_scale1;
    _data_scale2 = src._data_scale2;
    _is_file = src._is_file;
    _ref_min = src._ref_min;
    _ref_max = src._ref_max;
    _data_out_off_range = src._data_out_off_range;
}

void DsoSnapshot::free_data()
{
    Snapshot::free_data();

    for (int i=0; i<(int)_ch_data.size(); i++)
    {
        void *p = _ch_data[i];
        free(p);
    }

    _ch_data.clear();
}

void DsoSnapshot::first_payload(const sr_datafeed_dso &dso, uint64_t total_sample_count,
                                GSList *channels, bool instant, bool isFile)
{
    if (!channels) {
        pxv_warn("%s", "DsoSnapshot::first_payload: channels is nullptr");
        return;
    }
    assert(channels);

    bool channel_changed = false;
    uint16_t channel_num = 0;
    _is_file = isFile;

    for (const GSList *l = channels; l; l = l->next) {
        sr_channel *const probe = (sr_channel*)l->data;

        if (probe->type == SR_CHANNEL_DSO) {
            if (probe->enabled || isFile){
                channel_num++;
                if (!channel_changed){
                    channel_changed = !has_data(probe->index);
                }
            } 
        }
    }

    assert(channel_num != 0);

    _instant = instant;
    bool isOk = true;

    if (total_sample_count != _total_sample_count
        || channel_num != _channel_num
        || channel_changed
        || isFile){
        
        std::lock_guard<std::recursive_mutex> lock(_mutex);

        free_data();

        _ch_index.clear();
        _total_sample_count = total_sample_count;
        _channel_num = channel_num; 

         for (const GSList *l = channels; l; l = l->next) {
            sr_channel *const probe = (sr_channel*)l->data;

            if (probe->type == SR_CHANNEL_DSO && (probe->enabled || isFile)) {
                
                uint8_t *chan_buffer = (uint8_t*)malloc(total_sample_count + 1);
                if (chan_buffer == nullptr){
                    isOk = false;
                    pxv_err("DsoSnapshot::first_payload, Malloc memory failed!");
                    break;
                }
                _ch_data.push_back(chan_buffer);
                _ch_index.push_back(probe->index);
            }
        }
        
        if (isOk) {
            free_envelop();

            for (unsigned int i = 0; i < _channel_num; i++) {
                uint64_t envelop_count = _total_sample_count / EnvelopeScaleFactor;

                for (unsigned int level = 0; level < ScaleStepCount; level++) {
                    
                    envelop_count = ((envelop_count + EnvelopeDataUnit - 1) / EnvelopeDataUnit) 
                                        * EnvelopeDataUnit;

                    uint64_t buffer_len = envelop_count * sizeof(EnvelopeSample);
                    _envelope_levels[i][level].samples = (EnvelopeSample*)malloc(buffer_len);
                    
                    if (_envelope_levels[i][level].samples == nullptr) {
                        pxv_err("DsoSnapshot::first_payload, malloc failed!");
                        isOk = false;
                        break;
                    }
                    
                    envelop_count = envelop_count / EnvelopeScaleFactor;
                }
                if (!isOk)
                    break;
            }
        }
    }

    if (isOk) {
        _memory_failed = false;
        append_payload(dso);
        _last_ended = false;
    }
    else {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        free_data();
        free_envelop();
        _memory_failed = true;
    }
}

void DsoSnapshot::append_payload(const sr_datafeed_dso &dso)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (_channel_num > 0 && dso.num_samples > 0) {       
        append_data(dso.data, dso.num_samples, _instant);

        // Generate the first mip-map from the data
        if (_envelope_en)
            append_payload_to_envelope_levels(dso.samplerate_tog);
    }
}

void DsoSnapshot::append_data(void *data, uint64_t samples, bool instant)
{
    uint64_t old_sample_count = _sample_count;

    _data_out_off_range = false;

    if (instant) { 
        if(_sample_count + samples > _total_sample_count)
            samples = _total_sample_count - _sample_count;
        _sample_count += samples;
    }
    else {
        _sample_count = samples;
    }

    assert(_sample_count <= _total_sample_count);
     
    for (unsigned int ch = 0; ch < _channel_num; ch++)
    {
        uint8_t *src = (uint8_t*)data + ch;
        uint8_t *dest = _ch_data[ch];

        if (instant){
            dest += old_sample_count;
        }

        for (uint64_t i = 0; i < samples; i++)
        {
            *dest++ = *src;

            if (*src > _ref_max || *src < _ref_min){
                _data_out_off_range = true;
            }
            
            src += _channel_num;
        }  
    }
}

void DsoSnapshot::enable_envelope(bool enable)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (!_envelope_done && enable)
        append_payload_to_envelope_levels(true);
    _envelope_en = enable;
}

const uint8_t *DsoSnapshot::get_samples(int64_t start_sample, int64_t end_sample, uint16_t ch_index)
{
    (void)end_sample;
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (start_sample < 0 || start_sample >= (int64_t)_sample_count) {
        pxv_warn("DsoSnapshot::get_samples: start_sample %lld out of range (count=%llu)",
                 (long long)start_sample, (unsigned long long)_sample_count);
        return nullptr;
    }
    if (end_sample < 0 || end_sample >= (int64_t)_sample_count) {
        pxv_warn("DsoSnapshot::get_samples: end_sample %lld out of range (count=%llu)",
                 (long long)end_sample, (unsigned long long)_sample_count);
        return nullptr;
    }
    if (start_sample > end_sample) {
        pxv_warn("DsoSnapshot::get_samples: start %lld > end %lld",
                 (long long)start_sample, (long long)end_sample);
        return nullptr;
    }

    int order = get_ch_order(ch_index);

    if (order == -1){
        pxv_err("The channel index is not exist:%d", ch_index);
        return nullptr;
    }

    /* AGENTS.md: assert() is a no-op in Release. If _ch_data[order] has
     * not been allocated yet (channel data not populated), return nullptr
     * so callers can check for nullptr instead of dereferencing a wild pointer
     * (nullptr + start_sample). */
    if (!_ch_data[order]) {
        pxv_warn("DsoSnapshot::get_samples: _ch_data[%d] is nullptr", order);
        return nullptr;
    }

    return (uint8_t*)_ch_data[order] + start_sample;
}

void DsoSnapshot::get_envelope_section(EnvelopeSection &s,
    uint64_t start, uint64_t end, float min_length, int probe_index)
{
	if (end > get_sample_count()) {
		pxv_warn("DsoSnapshot::get_envelope_section: end %llu > sample_count %llu, clamping",
		         (unsigned long long)end, (unsigned long long)get_sample_count());
		end = get_sample_count();
	}
	if (start > end) {
		pxv_warn("DsoSnapshot::get_envelope_section: start %llu > end %llu, clamping",
		         (unsigned long long)start, (unsigned long long)end);
		start = end;
	}
	assert(min_length > 0);

    const int order = get_ch_order(probe_index);
    if (order == -1) {
        s.length = 0;
        return;
    }

    if (!_envelope_done) {
        s.length = 0;
        return;
    }

	const unsigned int min_level = max((int)floorf(logf(min_length) /
		LogEnvelopeScaleFactor) - 1, 0);
	const unsigned int scale_power = (min_level + 1) *
		EnvelopeScalePower;
	start >>= scale_power;
	end >>= scale_power;

	s.start = start << scale_power;
	s.scale = 1 << scale_power;
    if (_envelope_levels[order][min_level].length == 0)
        s.length = 0;
    else
        s.length = end - start;

    s.samples = _envelope_levels[order][min_level].samples + start;
}

void DsoSnapshot::reallocate_envelope(Envelope &e)
{
	const uint64_t new_data_length = ((e.length + EnvelopeDataUnit - 1) /
		EnvelopeDataUnit) * EnvelopeDataUnit;
    if (new_data_length > e.data_length)
	{
		e.data_length = new_data_length;
	}
}

void DsoSnapshot::append_payload_to_envelope_levels(bool header)
{
    if (_ch_data.empty()) return;

    for (unsigned int i = 0; i < _channel_num; i++) {
        if (i >= _ch_data.size()) break;

        Envelope &e0 = _envelope_levels[i][0];
        uint64_t prev_length;
        EnvelopeSample *dest_ptr;

        if (header)
            prev_length = 0;
        else
            prev_length = e0.length;
        e0.length = _sample_count / EnvelopeScaleFactor;

        if (e0.length == 0)
            return;
        if (e0.length == prev_length)
            prev_length = 0;

        // Expand the data buffer to fit the new samples
        reallocate_envelope(e0);

        assert(e0.samples);

        dest_ptr = e0.samples + prev_length;

        // Iterate through the samples to populate the first level mipmap
        const uint8_t *const stop_src_ptr = (uint8_t*)_ch_data[i] + e0.length * EnvelopeScaleFactor;
        const uint8_t *src_ptr = (uint8_t*)_ch_data[i] + prev_length * EnvelopeScaleFactor;

        for (; src_ptr < stop_src_ptr; src_ptr += EnvelopeScaleFactor)
        {
            const uint8_t *begin_src_ptr = src_ptr;
            const uint8_t *const end_src_ptr = src_ptr + EnvelopeScaleFactor;

            EnvelopeSample sub_sample;
            sub_sample.min = *begin_src_ptr;
            sub_sample.max = *begin_src_ptr;

            while (begin_src_ptr < end_src_ptr)
            {
                sub_sample.min = ds_min(sub_sample.min, *begin_src_ptr);
                sub_sample.max = ds_max(sub_sample.max, *begin_src_ptr);
                begin_src_ptr++;
            }
            
            *dest_ptr++ = sub_sample;
        }

        // Compute higher level mipmaps
        for (unsigned int level = 1; level < ScaleStepCount; level++)
        {
            Envelope &e = _envelope_levels[i][level];
            const Envelope &el = _envelope_levels[i][level-1];

            // Expand the data buffer to fit the new samples
            if (header)
                prev_length = 0;
            else
                prev_length = e.length;
            e.length = el.length / EnvelopeScaleFactor;

            // Break off if there are no more samples to computed
            if (e.length == 0)
                break;
            if (e.length == prev_length)
                prev_length = 0;

            reallocate_envelope(e);

            // Subsample the level lower level
            const EnvelopeSample *src_ptr =
                el.samples + prev_length * EnvelopeScaleFactor;
            const EnvelopeSample *const end_dest_ptr = e.samples + e.length;

            for (dest_ptr = e.samples + prev_length;
                dest_ptr < end_dest_ptr; dest_ptr++)
            {
                const EnvelopeSample *const end_src_ptr =
                    src_ptr + EnvelopeScaleFactor;

                EnvelopeSample sub_sample = *src_ptr++;
                while (src_ptr < end_src_ptr)
                {
                    sub_sample.min = min(sub_sample.min, src_ptr->min);
                    sub_sample.max = max(sub_sample.max, src_ptr->max);
                    src_ptr++;
                }

                *dest_ptr = sub_sample;
            }
        }
    }
    _envelope_done = true;
}

double DsoSnapshot::cal_vrms(double zero_off, int index)
{
    assert(index >= 0);

    // root-meam-squart value
    double vrms_pre = 0;
    double vrms = 0;
    double tmp;

    // Iterate through the samples to populate the first level mipmap
    const uint8_t *const stop_src_ptr = (uint8_t*)_ch_data[index] + _sample_count;
    const uint8_t *src_ptr = (uint8_t*)_ch_data[index];

    for (;src_ptr < stop_src_ptr; src_ptr += VrmsScaleFactor)
    {
        const uint8_t * begin_src_ptr = src_ptr;
        const uint8_t *const end_src_ptr = src_ptr + VrmsScaleFactor;

        while (begin_src_ptr < end_src_ptr)
        {
            tmp = (zero_off - *begin_src_ptr);
            vrms += tmp * tmp;
            begin_src_ptr++;
        }
        vrms = vrms_pre + vrms / _sample_count;
        vrms_pre = vrms;
    }
    vrms = pow(vrms, 0.5);

    return vrms;
}

double DsoSnapshot::cal_vmean(int index)
{
    assert(index >= 0);

    // mean value
    double vmean_pre = 0;
    double vmean = 0;

    // Iterate through the samples to populate the first level mipmap
    const uint8_t *const stop_src_ptr = (uint8_t*)_ch_data[index] + _sample_count;
    const uint8_t *src_ptr = (uint8_t*)_ch_data[index];

    for (; src_ptr < stop_src_ptr; src_ptr += VrmsScaleFactor)
    {
        const uint8_t * begin_src_ptr = src_ptr;
        const uint8_t *const end_src_ptr = src_ptr + VrmsScaleFactor;

        while (begin_src_ptr < end_src_ptr)
        {
            vmean += *begin_src_ptr;
            begin_src_ptr += _channel_num;
        }
        vmean = vmean_pre + vmean / _sample_count;
        vmean_pre = vmean;
    }

    return vmean;
}

int DsoSnapshot::get_block_num()
{
    const uint64_t size = _sample_count * get_unit_bytes() * get_channel_num();
    return (size >> LeafBlockPower) +
           ((size & LeafMask) != 0);
}

uint64_t DsoSnapshot::get_block_size(int block_index)
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

bool DsoSnapshot::get_max_min_value(uint8_t &maxv, uint8_t &minv, int chan_index)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (_sample_count == 0){
        return false;
    }

    if (chan_index < 0 || chan_index >= (int)_ch_data.size()){
        pxv_err("DsoSnapshot::get_data_range: chan_index %d out of range (size=%zu)", chan_index, _ch_data.size());
        return false;
    }

    uint8_t *p = _ch_data[chan_index];
    maxv = *p;
    minv = *p;

    for (uint64_t i=1; i<_sample_count; i++){
        p++;
        if (*p > maxv)
            maxv = *p;
        if (*p < minv)
            minv = *p;
    }
    
    return true;
}

bool DsoSnapshot::has_data(int sig_index)
{
    return get_ch_order(sig_index) != -1;
}

int DsoSnapshot::get_ch_order(int sig_index)
{
    uint16_t order = 0;

    for (uint16_t i : _ch_index) {
        if (i == sig_index)
            return order;
        else
            order++;
    }

    return -1;
}

} // namespace data
} // namespace pv
