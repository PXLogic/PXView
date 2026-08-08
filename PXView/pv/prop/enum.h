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


#ifndef PXVIEW_PV_PROP_ENUM_H
#define PXVIEW_PV_PROP_ENUM_H

#include <utility>
#include <vector>

#include "pv/prop/property.h"
#include "pv/base/gvarptr.h"

class DsComboBox;

namespace pv {
namespace prop {

class Enum : public Property
{
    Q_OBJECT;

public:
	Enum(QString name, QString label, std::vector<std::pair<GVarPtr, QString> > values,
		Getter getter, Setter setter);

	virtual ~Enum();

protected:
    QWidget* get_widget(QWidget *parent, bool auto_commit) override;
public:

	GVariant* get_value();

	void commit();

	/* Select the combo box item whose GVariant prints to val_str.
	 * Used by DeviceOptionsDock::set_session() to restore Mode section
	 * properties from saved JSON state. Calls commit() to write the value
	 * to the driver. */
	void select_value(const QString &val_str);

private slots:
    void on_current_item_changed(int);

private:
	const std::vector< std::pair<GVarPtr, QString> > _values;

	DsComboBox *_selector;
};

} // prop
} // pv

#endif // PXVIEW_PV_PROP_ENUM_H
