/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 * 
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#ifndef PXVIEW_PV_PROP_STRING_H
#define PXVIEW_PV_PROP_STRING_H

#include "property.h"

class QLineEdit;
class QToolButton;

namespace pv {
namespace prop {

class String : public Property
{
	Q_OBJECT;

public:
    String(QString name, QString label, Getter getter, Setter setter);

	QWidget* get_widget(QWidget *parent, bool auto_commit);

	void commit();

	bool is_path_or_dir();
	QLineEdit* get_line_edit() const { return _line_edit; }
	QToolButton* get_browse_btn() const { return _browse_btn; }

private slots:
	void on_text_edited(const QString&);

private:
	QLineEdit *_line_edit;
	QToolButton *_browse_btn;
	QWidget *_container;
};

} // prop
} // pv

#endif // PXVIEW_PV_PROP_STRING_H
