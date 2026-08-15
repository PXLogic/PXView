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

#ifndef PXVIEW_PV_DATA_SPECTRUMSTACK_H
#define PXVIEW_PV_DATA_SPECTRUMSTACK_H

#include "pv/data/model/signaldata.h"

#include <list>

#include <optional> 
  
#include <fftw3.h>

#include <mutex>

#include <QObject>
#include <QString>

namespace pv {

class SigSession;

namespace view {
class DsoSignal;
}

namespace data {

class DsoSnapshot;

class SpectrumStack : public QObject, public SignalData
{
    Q_OBJECT

public:
    enum spectrum_state {
        Init,
        Stopped,
        Running
    };

public:
    SpectrumStack(pv::SigSession *_session, int index);
    virtual ~SpectrumStack();
    void clear();
    void init();

    int get_index();

    uint64_t get_sample_num();
    void set_sample_num(uint64_t num);

    int get_windows_index();
    void set_windows_index(int index); 

    bool dc_ignored();
    void set_dc_ignore(bool ignore);

    int get_sample_interval();
    void set_sample_interval(int interval);

    const std::vector<double> get_fft_spectrum();
    double get_fft_spectrum(uint64_t index);

    void calc_fft();

    double window(uint64_t i, int type); 

signals:
    // #4 FFT 计算完成 (在 worker 线程触发, queued 到 GUI 线程刷新频谱)
    void fft_updated();

private:
    pv::SigSession *_session;

    int _index;
    uint64_t _sample_num;
    int _windows_index;
    bool _dc_ignore;
    int _sample_interval;
    spectrum_state _spectrum_state;

    fftw_plan _fft_plan;
    std::vector<double> _xn;
    std::vector<double> _xk;
    std::vector<double> _power_spectrum;

    // #4 保护 _power_spectrum / _spectrum_state 的并发读写 (calc_fft 在
    // worker 线程写, get_fft_spectrum 在 GUI 线程读).
    mutable std::mutex _fft_mutex;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SPECTRUMSTACK_H
