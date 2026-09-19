/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#include "pv/data/stack/spectrumstack.h"
#include <cmath>
#include <memory>
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/isignal_model_source.h"


#define PI 3.1415

using namespace std;

namespace pv {
namespace data {

SpectrumStack::SpectrumStack(pv::data::ISignalModelSource *source, int index) :
    _source(source),
    _index(index),
    _dc_ignore(true),
    _sample_interval(1),
    _spectrum_state(spectrum_state::Init),
    _fft_plan(nullptr)
{

}

SpectrumStack::~SpectrumStack()
{
    _xn.clear();
    _xk.clear();
    _power_spectrum.clear();
    if (_fft_plan)
        fftw_destroy_plan(_fft_plan);
}

void SpectrumStack::clear()
{
}

void SpectrumStack::init()
{
}

int SpectrumStack::get_index()
{
    return _index;
}

uint64_t SpectrumStack::get_sample_num()
{
    return _sample_num;
}

void SpectrumStack::set_sample_num(uint64_t num)
{
    _sample_num = num;
    _xn.resize(_sample_num);
    _xk.resize(_sample_num);
    _power_spectrum.resize(_sample_num/2+1);
    // FFTW's plan API takes an int length. _sample_num is bounded well below
    // INT_MAX by the capture size, so the narrowing is safe.
    _fft_plan = fftw_plan_r2r_1d(static_cast<int>(_sample_num), _xn.data(), _xk.data(),
                                 FFTW_R2HC, FFTW_ESTIMATE);
}

int SpectrumStack::get_windows_index()
{
    return _windows_index;
}

void SpectrumStack::set_windows_index(int index)
{
    _windows_index = index;
}

bool SpectrumStack::dc_ignored()
{
    return _dc_ignore;
}

void SpectrumStack::set_dc_ignore(bool ignore)
{
    _dc_ignore = ignore;
}

int SpectrumStack::get_sample_interval()
{
    return _sample_interval;
}

void SpectrumStack::set_sample_interval(int interval)
{
    _sample_interval = interval;
}

const std::vector<double> SpectrumStack::get_fft_spectrum()
{
    std::lock_guard<std::mutex> lk(_fft_mutex);
    if (_spectrum_state == spectrum_state::Stopped)
        return _power_spectrum;
    return {};
}

double SpectrumStack::get_fft_spectrum(uint64_t index)
{
    std::lock_guard<std::mutex> lk(_fft_mutex);
    double ret = -1;
    if (_spectrum_state == spectrum_state::Stopped && index < _power_spectrum.size())
        ret = _power_spectrum[index];

    return ret;
}

void SpectrumStack::calc_fft()
{
    _spectrum_state = spectrum_state::Running;
    // Get the dso data
    pv::data::DsoSnapshot *data = nullptr;
    std::shared_ptr<pv::data::DsoSnapshot> data_snap;
    std::shared_ptr<pv::data::SignalModel> model;

    for(auto m : _source->get_signal_models()) {
        if (m->type() == SR_CHANNEL_DSO) {
            if (m->index() == _index && m->enabled()) {
                data_snap = std::static_pointer_cast<pv::data::DsoSnapshot>(m->snapshot());
                data = data_snap.get();
                model = m;
                break;
            }
        }
    }

    if (data == nullptr || model == nullptr)
        return;

    if (data->empty())
        return;

    if (data->get_sample_count() < _sample_num * _sample_interval)
        return;

    // Get the samplerate
    _samplerate = data->samplerate();
    if (_samplerate == 0.0)
        _samplerate = 1.0;

    // prepare _xn data — read hardware offset / vdiv / vfactor from the
    // SignalModel (pure data layer, no view::DsoSignal dependency).
    const int offset = static_cast<int>(model->hw_offset());
    const double vscale = model->vdiv_mv() * model->vfactor() * DS_CONF_DSO_VDIVS / (1000*255.0);
    const uint16_t step = static_cast<uint16_t>(_sample_interval);
    // P1-c（统一读取抽象）：DSO 是通道平面布局，span.data 与旧
    // get_samples(0, N-1, _index) 返回的基指针等价。上方已保证
    // get_sample_count() >= _sample_num * _sample_interval，故 contiguous_samples
    // 覆盖所有被读取的下标（最大为 (_sample_num-1)*step）。
    const pv::data::SampleSpan sp =
        data->span(static_cast<uint32_t>(_index), 0, static_cast<uint64_t>(_sample_num) * _sample_interval);
    if (!sp.valid())
        return;
    const uint8_t *const samples = sp.data;
    double wsum = 0;
    
    for (unsigned int i = 0; i < _sample_num; i++) {
        double w = window(i, _windows_index);
        _xn[i] = (samples[i*step] - offset) * vscale * w;
        wsum += w;
    }

    // fft
    fftw_execute(_fft_plan);

    // calculate power spectrum (锁保护: GUI 线程可能并发读 get_fft_spectrum)
    {
      std::lock_guard<std::mutex> lk(_fft_mutex);
      _power_spectrum[0] = abs(_xk[0])/wsum;  /* DC component */
      for (unsigned int k = 1; k < (_sample_num + 1) / 2; ++k)  /* (k < N/2 rounded up) */
           _power_spectrum[k] = sqrt((_xk[k]*_xk[k] + _xk[_sample_num-k]*_xk[_sample_num-k]) * 2) / wsum;
      if (_sample_num % 2 == 0) /* N is even */
           _power_spectrum[_sample_num/2] = abs(_xk[_sample_num/2])/wsum;  /* Nyquist freq. */

      _spectrum_state = spectrum_state::Stopped;
    }
    // 计算完成 (在 worker 线程触发 → queued 到 GUI 线程刷新频谱 trace)
    emit fft_updated();
}

double SpectrumStack::window(uint64_t i, int type)
{
    const double n_m_1 = static_cast<double>(_sample_num - 1);
    // Every window coefficient below is a double, so hoist the sample index
    // once instead of letting each cos() argument convert implicitly.
    const double di = static_cast<double>(i);
    switch(type) {
    case 1: // Hann window
        return 0.5*(1-cos(2*PI*di/n_m_1));
    case 2: // Hamming window
        return 0.54-0.46*cos(2*PI*di/n_m_1);
    case 3: // Blackman window
        return 0.42659-0.49656*cos(2*PI*di/n_m_1) + 0.076849*cos(4*PI*di/n_m_1);
    case 4: // Flat_top window
        return 1-1.93*cos(2*PI*di/n_m_1)+1.29*cos(4*PI*di/n_m_1)-
                 0.388*cos(6*PI*di/n_m_1)+0.028*cos(8*PI*di/n_m_1);
    default:
        return 1;
    }
}

} // namespace data
} // namespace pv
