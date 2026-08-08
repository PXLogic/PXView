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

#ifndef PXVIEW_PV_VIEW_DSO_MEASURE_H
#define PXVIEW_PV_VIEW_DSO_MEASURE_H

#include <QPainter>
#include <QPointF>
#include <QString>
#include <cstdint>

namespace pv {
namespace view {

class DsoSignal;

/**
 * DsoMeasure — delegate for DsoSignal measurement / auto-set / hover.
 *
 * Extracted from DsoSignal (Phase G3 of modernize-view-layer-v2). Owns the
 * method bodies for get_measure / measure / hover / voltage / time / auto_set
 * / autoV_end / autoH_end / auto_end / auto_start, plus the per-frame measure
 * status update (extracted from paint_mid). Holds a non-owning pointer back to
 * the parent DsoSignal and accesses its private state via friendship.
 *
 * The DsoSignal public API is preserved: DsoSignal keeps thin facade methods
 * that forward to this delegate.
 */
class DsoMeasure
{
public:
    explicit DsoMeasure(DsoSignal *signal);
    ~DsoMeasure();

    // -- measurement readout --
    // Parameter is int rather than enum DSO_MEASURE_TYPE so this header
    // does not need to include <libsigrok.h>. Callers (e.g. DsoSignal,
    // ViewStatus) still pass DSO_MS_* constants — enum values implicitly
    // convert to int.
    QString get_measure(int type);

    // -- hover / point pick --
    bool measure(const QPointF &p);
    bool get_hover(uint64_t &index, QPointF &p, double &value);
    QPointF get_point(uint64_t index, float &value);

    // -- voltage / time formatting --
    double get_voltage(uint64_t index);
    QString get_voltage(double v, int p, bool scaled = false);
    QString get_time(double t);

    // -- auto-set (vertical + horizontal autoscale) --
    void auto_set();
    void autoV_end();
    void autoH_end();
    void auto_end();
    void auto_start();

    /**
     * Paints the hover-point voltage label and the per-cursor voltage labels.
     * Extracted from DsoSignal::paint_hover_measure (Phase G4) so the DsoSignal
     * method stays a thin entry point. DsoSignal::paint_hover_measure forwards
     * here.
     */
    void paint_hover_measure(QPainter &p, QColor fore, QColor back);

private:
    void call_auto_end();

    DsoSignal *_signal;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_DSO_MEASURE_H
