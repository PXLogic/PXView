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


#ifndef PXVIEW_PV_DATA_SIGNALDATA_H
#define PXVIEW_PV_DATA_SIGNALDATA_H

#include <cstdint>
#include <mutex>
#include <atomic>

namespace pv {
namespace data {

class SignalData
{
public:
    SignalData();
    virtual ~SignalData() = 0;

public:
    // Thread-safety P1: _samplerate is now std::atomic<double>.
    // Reads (samplerate()) and writes (set_samplerate()) use
    // atomic load/store, making them safe from any thread.
    inline double samplerate() const
        {return _samplerate.load(std::memory_order_acquire); }

    void set_samplerate(double samplerate);
    virtual void clear() = 0;
    virtual void init() = 0;

protected:
    mutable std::mutex _mutex;
    std::atomic<double> _samplerate;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SIGNALDATA_H
