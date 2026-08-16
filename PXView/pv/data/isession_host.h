/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
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

#ifndef PXVIEW_PV_DATA_ISESSION_HOST_H
#define PXVIEW_PV_DATA_ISESSION_HOST_H

#include <functional>
#include <memory>
#include <shared_mutex>
#include <vector>

namespace pv {
namespace data {

class SignalModel;

/**
 * ISessionHost — 解码栈所需的会话宿主能力（依赖倒置）。
 *
 * DecoderStack（pxview-data）此前持有 SigSession*（具体类），通过
 * event_bus_post / signal_models_mutex / get_signal_models /
 * is_realtime_refresh / is_closed / get_ring_sample_count 反向依赖
 * pxview-session，导致隔离单测链接墙。本接口只暴露 DecoderStack 实际
 * 需要的那一小片会话能力，由 SigSession（session 层）实现并注入。
 * 窄接口原则：仅包含被调用的方法，避免把整个 SigSession 虚化。
 */
class ISessionHost {
public:
    virtual ~ISessionHost() = default;

    /// Post a callable to the main thread (thread-safe; used to emit Qt
    /// signals from the decode worker thread).
    virtual void event_bus_post(std::function<void()> fn) = 0;
    /// Mutex protecting the signal models vector (shared_lock for readers).
    virtual std::shared_mutex &signal_models_mutex() = 0;
    /// Live signal models vector. Callers MUST hold signal_models_mutex().
    virtual std::vector<std::shared_ptr<SignalModel>>& get_signal_models() = 0;
    /// True when the capture is in real-time refresh (streaming) mode.
    virtual bool is_realtime_refresh() = 0;
    /// True when the session has been closed.
    virtual bool is_closed() = 0;
    /// Ring-buffer sample limit for repeating captures.
    virtual int64_t get_ring_sample_count() = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_ISESSION_HOST_H
