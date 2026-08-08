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

#include <QAbstractItemView>

#include "pv/prop/enum.h"
#include "pv/ui/dscombobox.h"
#include "pv/base/log.h"

using namespace std;

namespace pv {
namespace prop {

Enum::Enum(QString name, QString label,
    std::vector<pair<GVarPtr, QString> > values,
    Getter getter, Setter setter) :
    Property(name, label, getter, setter),
	_values(std::move(values)),
	_selector(nullptr)
{
}

Enum::~Enum()
{
	// Only delete _selector if it has no Qt parent. If it was parented
	// to a widget (e.g. a dock), Qt's parent-child mechanism will delete
	// it automatically. Deleting it here as well would cause a double-free.
	if (_selector != nullptr && _selector->parent() == nullptr){
		delete _selector;
	}
	_selector = nullptr;
}

QWidget* Enum::get_widget(QWidget *parent, bool auto_commit)
{
	if (_selector)
		return _selector;

	GVariant *const value = _getter ? _getter() : nullptr;
    if (!value) {
        pxv_warn("Enum::get_widget: _getter() returned nullptr for property '%s' "
                 "(name='%s'), widget will not be created",
                 label().toUtf8().data(), name().toUtf8().data());
        return nullptr;
    }

	_selector = new DsComboBox(parent);
	_selector->setObjectName("dock_content");

	for (unsigned int i = 0; i < _values.size(); i++) {
		const pair<GVarPtr, QString> &v = _values[i];
		_selector->addItem(v.second, QVariant::fromValue(v.first));
		
		if (value && g_variant_compare(v.first.get(), value) == 0)
			_selector->setCurrentIndex(i);
	}

	g_variant_unref(value);

    if (auto_commit) {
        connect(_selector, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &Enum::on_current_item_changed);
    }

	return _selector;
}

void Enum::commit()
{
	if (!_setter)
		return;
	assert(_setter);

	if (!_selector)
		return;

	const int index = _selector->currentIndex();
	if (index < 0)
		return;

	_setter(qvariant_cast<GVarPtr>(_selector->itemData(index)).get());
	emit committed();
}

GVariant* Enum::get_value()
{
	return _getter ? _getter() : nullptr;
}

void Enum::on_current_item_changed(int)
{
    commit();
}

void Enum::select_value(const QString &val_str)
{
    if (!_selector)
        return;

    for (unsigned int i = 0; i < _values.size(); i++) {
        GVariant *gvar = _values[i].first.get();
        if (!gvar)
            continue;
        gchar *text = g_variant_print(gvar, FALSE);
        QString printed = QString::fromUtf8(text);
        g_free(text);
        if (printed == val_str) {
            _selector->setCurrentIndex(i);
            commit();
            return;
        }
    }
    pxv_warn("Enum::select_value: no match for '%s' in property '%s'",
             val_str.toUtf8().constData(), name().toUtf8().constData());
}

} // prop
} // pv
