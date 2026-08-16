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

#ifndef PXVIEW_PV_DATA_ISIGNAL_MODEL_SOURCE_H
#define PXVIEW_PV_DATA_ISIGNAL_MODEL_SOURCE_H

#include <memory>
#include <vector>

namespace pv {
namespace data {

class SignalModel;

/**
 * ISignalModelSource — 极窄的信号模型访问源（依赖倒置）。
 *
 * SpectrumStack / MathStack（pxview-data）此前持有 SigSession*（具体类），
 * 仅为了 get_signal_models() 反向依赖 pxview-session，导致隔离单测链接墙。
 * 本接口只暴露这一个方法，由 SigSession 实现并注入。Fake 实现仅数行即可
 * 驱动隔离单测。
 */
class ISignalModelSource {
public:
    virtual ~ISignalModelSource() = default;

    /// Live signal models vector. Callers MUST hold signal_models_mutex()
    /// when accessing from non-UI threads (see ISessionHost).
    virtual std::vector<std::shared_ptr<SignalModel>>& get_signal_models() = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_ISIGNAL_MODEL_SOURCE_H
