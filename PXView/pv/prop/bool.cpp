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


#include <cassert>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "pv/prop/bool.h"
#include "pv/config/appconfig.h"
#include "pv/ui/iconcache.h"

using namespace std;

namespace pv {
namespace prop {

Bool::Bool(QString name, QString label, Getter getter, Setter setter,
           bool eye_button, bool int64_storage) :
    Property(name, label, getter, setter),
    _check_box(nullptr),
    _eye_button(nullptr),
    _eye_widget(nullptr),
    _eye_button_mode(eye_button),
    _int64_storage(int64_storage)
{
}

Bool::~Bool()
{
}

QWidget* Bool::get_widget(QWidget *parent, bool auto_commit)
{
    if (_eye_button_mode) {
        if (_eye_widget)
            return _eye_widget;

        _eye_widget = new QWidget(parent);
        auto *layout = new QHBoxLayout(_eye_widget);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto *text = new QLabel(label(), _eye_widget);
        _eye_button = new QPushButton(_eye_widget);
        _eye_button->setCheckable(true);
        _eye_button->setFlat(true);
        _eye_button->setFixedSize(26, 24);
        _eye_button->setToolTip(label());

        GVariant *const value = _getter ? _getter() : nullptr;
        if (value) {
            bool checked = false;
            if (_int64_storage &&
                g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
                checked = g_variant_get_int64(value) != 0;
            else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
                checked = g_variant_get_boolean(value);
            _eye_button->setChecked(checked);
            g_variant_unref(value);
        }
        update_eye_icon();

        connect(_eye_button, &QPushButton::toggled, this, [this, auto_commit](bool) {
            update_eye_icon();
            if (auto_commit)
                commit();
        });

        layout->addWidget(text);
        layout->addStretch(1);
        layout->addWidget(_eye_button);
        return _eye_widget;
    }

    if (_check_box)
        return _check_box;

    _check_box = new QCheckBox(label(), parent);

    GVariant *const value = _getter ? _getter() : nullptr;

    if (value) {
        bool checked = false;
        if (_int64_storage &&
            g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
            checked = g_variant_get_int64(value) != 0;
        else if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
            checked = g_variant_get_boolean(value);
        _check_box->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        g_variant_unref(value);
    }

    if (auto_commit) {
        connect(_check_box, &QCheckBox::checkStateChanged,
            this, &Bool::on_state_changed);
    }

    return _check_box;
}

void Bool::update_eye_icon()
{
    if (!_eye_button)
        return;
    const QString iconPath = GetIconPath();
    _eye_button->setIcon(IconCache::Instance().icon(
        _eye_button->isChecked() ? iconPath + "/shown.svg"
                                 : iconPath + "/hidden.svg"));
}

bool Bool::labeled_widget()
{
	return true;
}

GVariant* Bool::get_value()
{
    GVariant *const value = _getter ? _getter() : nullptr;

    return value;
}

void Bool::commit()
{
    if (!_setter)
        return;
    assert(_setter);

    bool checked = false;
    if (_eye_button_mode) {
        if (!_eye_button)
            return;
        checked = _eye_button->isChecked();
    } else {
        if (!_check_box)
            return;
        checked = _check_box->checkState() == Qt::Checked;
    }

    if (_int64_storage)
        _setter(g_variant_new_int64(checked ? 1 : 0));
    else
        _setter(g_variant_new_boolean(checked));
    emit committed();
}

void Bool::on_state_changed(int)
{
    commit();
}

} // prop
} // pv
