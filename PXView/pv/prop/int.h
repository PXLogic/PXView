/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
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


#ifndef PXVIEW_PV_PROP_INT_H
#define PXVIEW_PV_PROP_INT_H

#include <utility>

#include <optional>

#include "pv/prop/property.h"

namespace pv {
namespace ui {
class DsSpinBox;
class IntSpinBox;
}

namespace prop {

class Int : public Property
{
    Q_OBJECT;

public:
    Int(QString name, QString label, QString suffix,
		std::optional< std::pair<int64_t, int64_t> > range,
		Getter getter, Setter setter);

	virtual ~Int();

protected:
    QWidget* get_widget(QWidget *parent, bool auto_commit) override;
public:

	void commit();

private slots:
    void on_value_changed(int);
    /* The wide editor reports a settled value (editing finished / step). */
    void on_wide_value_changed();

private:
    /* Whether the editor must be the 64-bit spin box: true for the integer types
     * a QSpinBox cannot express (uint32/int64/uint64), unless an explicit
     * int-sized range proves the value stays inside the QSpinBox. */
    bool needs_wide_spin_box(const GVariantType *type) const;

    QWidget *create_spin_box(QWidget *parent, bool auto_commit);
    QWidget *create_wide_spin_box(QWidget *parent, bool auto_commit);

	const QString _suffix;
	const std::optional< std::pair<int64_t, int64_t> > _range;

    GVariant *_value;
	pv::ui::DsSpinBox *_spin_box;

    /* Wide types (uint32/int64/uint64) get pv::ui::IntSpinBox: a full 64-bit spin
     * box, because QSpinBox tops out at INT_MAX and cannot express a 4 GHz
     * samplerate or VCD's "skip" sentinel (UINT64_MAX = "start at the first
     * timestamp") — the same custom widget the @todo in this file asked for. */
    pv::ui::IntSpinBox *_int_spin_box = nullptr;

    /* The widget handed to the form. */
    QWidget *_editor = nullptr;

    /* What the spin box shows right now. commit() compares the field content
     * against it: an untouched spin box must submit the module's declared value
     * verbatim, otherwise a value outside its int range would be replaced by the
     * clamp merely because the dialog was accepted. The wide editor keeps the
     * value in 64 bits, so it needs no such rescue. */
    int _displayed_value = 0;
};

} // prop
} // pv

#endif // PXVIEW_PV_PROP_INT_H
