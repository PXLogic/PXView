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

#ifndef PXVIEW_PV_VIEW_DSO_TRIGGER_CONFIG_H
#define PXVIEW_PV_VIEW_DSO_TRIGGER_CONFIG_H

namespace pv {
namespace view {

class DsoSignal;

/**
 * DsoTriggerConfig — delegate for DsoSignal trigger voltage configuration.
 *
 * Extracted from DsoSignal (Phase G2 of modernize-view-layer-v2). Owns the
 * method bodies for trigger value/ratio/position handling. Holds a non-owning
 * pointer back to the parent DsoSignal and accesses its private state via
 * friendship.
 *
 * The DsoSignal public API is preserved: DsoSignal keeps thin facade methods
 * that forward to this delegate.
 */
class DsoTriggerConfig
{
public:
    explicit DsoTriggerConfig(DsoSignal *signal);
    ~DsoTriggerConfig();

    double get_trig_vrate();
    void set_trig_vpos(int pos, bool delta_change = true);
    void set_trig_ratio(double ratio, bool delta_change = true);

private:
    DsoSignal *_signal;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_DSO_TRIGGER_CONFIG_H
