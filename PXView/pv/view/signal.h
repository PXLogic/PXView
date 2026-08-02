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

#ifndef PXVIEW_PV_SIGNAL_H
#define PXVIEW_PV_SIGNAL_H

#include <QPainter>
#include <QPen>
#include <QRect>
#include <QString>

#include <cstdint>
#include <list>
#include <memory>

#include "trace.h"

namespace pv {

namespace data {
class SignalData;
class SignalModel;
class DataSource;
}

class SigSession;

namespace view {

/**
 * draw signal trace base class
 * Signal is the View layer representation of a Core-layer SignalModel.
 * It bridges Qt Widget rendering with the data model.
 */
class Signal : public Trace
{
    Q_OBJECT

signals:
    void sig_released(void *o);

public slots:
    void on_appearance_changed();
    void on_visibility_changed();

protected:
    /**
     * Constructor accepting a SignalModel and DataSource.
     * @param model The Core-layer SignalModel that backs this view::Signal.
     * @param data_source The DataSource for data/snapshot access (typically
     *                    the SigSession, which implements DataSource).
     */
    Signal(std::shared_ptr<data::SignalModel> model, data::DataSource *data_source);

    /**
     * Copy constructor for cloning in new views.
     * @param s The Signal to copy from.
     * @param model The SignalModel for the new Signal (shared with the original).
     * @param data_source The DataSource for the new view.
     */
    Signal(const Signal &s, std::shared_ptr<data::SignalModel> model, data::DataSource *data_source);

public:
    virtual ~Signal() {}

    /**
     * Clone this Signal for a new view.
     * The cloned Signal shares the same SignalModel as the original.
     */
    virtual Signal* clone() const = 0;

    /**
     * Returns whether the signal is enabled.
     */
    bool enabled();

    /**
     * Sets the signal name.
     * Override from Trace to also update the Core-layer SignalModel.
     */
    void set_name(QString name) override;

    /**
     * Sets whether the signal is enabled.
     * Also updates the Core-layer SignalModel.
     */
    virtual void set_enabled(bool en);

    /**
     * Sets the colour of the signal.
     * Override from Trace to also update the Core-layer SignalModel.
     */
    void set_colour(QColor colour) override;

    /**
     * Accessor for the Core-layer SignalModel that backs this view::Signal.
     * External consumers (DecoderOptionsDlg, etc.) should use this instead of
     * the deprecated probe() accessor.
     */
    inline std::shared_ptr<data::SignalModel> model() {
        return _model;
    }

protected:
    std::shared_ptr<data::SignalModel> _model;
    data::DataSource *_data_source = nullptr;
    bool _local_enabled = true;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_SIGNAL_H