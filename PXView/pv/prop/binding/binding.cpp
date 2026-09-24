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
 

#include <QFormLayout>
#include <QLabel>

#include "pv/prop/property.h"
#include "pv/prop/binding/binding.h"
 

namespace pv {
namespace prop {
namespace binding {

const std::vector<Property*>& Binding::properties()
{
	return _properties;
}

Binding::Binding(){
    _row_num = 0;    
}

Binding::~Binding(){
    for (auto p : _properties)
        delete p;
    _properties.clear();
}

void Binding::commit()
{
    for(auto p : _properties) {
        p->commit();
    }
}

void Binding::add_properties_to_form(QFormLayout *layout, bool auto_commit, QFont font)
{
    if (!layout)
        return;
    assert(layout);

    for(auto p : _properties)
    {
        QWidget *const widget = auto_commit
            ? p->get_widget_live(layout->parentWidget())
            : p->get_widget_deferred(layout->parentWidget());

        // A property whose widget could not be built (Int/String return
        // nullptr when their getter yields no value) must be skipped: the
        // code below dereferences the widget unconditionally, so adding the
        // row would crash. Options that carry no default value are skipped by
        // the bindings themselves, this is the safety net.
        if (!widget)
            continue;

        // P2-B: Connect each property's committed() signal to the
        // aggregated config-changed callback so consumers only need
        // to register one callback instead of per-property connections.
        if (_config_changed_cb) {
            QObject::connect(p, &Property::committed,
                             [this]() { if (_config_changed_cb) _config_changed_cb(); });
        }

        if (p->labeled_widget()){
            layout->addRow(widget);
            widget->setFont(font);
            _row_num++;
        }
        else{
            const QString &lbstr = p->label();
            //remove data format options
            // Legacy: only the device-options panel wanted this row hidden.
            // The import/export option dialog turns it off (a libsigrok module
            // option is legitimately named "Data format", e.g. raw_analog), and
            // the check compares untranslated text, so it must never apply to
            // translated labels either.
            if (_skip_data_format && lbstr == "Data format"){
                continue;                
            }   
            QLabel *lb = new QLabel(p->label());
            lb->setFont(font);
            widget->setFont(font);
            // Module options carry a one-sentence explanation; too long for a
            // label, so it becomes the tooltip of the whole row. The editor may
            // already carry one of its own (e.g. Int reports the full value when
            // the spin box had to clamp it), so keep both.
            if (!p->description().isEmpty()) {
                const QString existing = widget->toolTip();
                const QString combined = existing.isEmpty()
                    ? p->description()
                    : existing + QLatin1Char('\n') + p->description();
                lb->setToolTip(combined);
                widget->setToolTip(combined);
            }
            layout->addRow(lb, widget);
            _row_num++;
        } 
    }
}

std::map<Property*,GVariant*> Binding::get_property_value()
{
    std::map <Property*,GVariant*> pvalue;
            
    for(auto p : _properties)
    {
        pvalue[p] = p->get_value();
    }

    return pvalue;
}

QString Binding::print_gvariant(GVariant *const gvar)
{
    QString s;

    if (g_variant_is_of_type(gvar, G_VARIANT_TYPE("s")))
        s = QString::fromUtf8(g_variant_get_string(gvar, nullptr));
    else
    {
        gchar *const text = g_variant_print(gvar, FALSE);
        s = QString::fromUtf8(text);
        g_free(text);
    }

    return s;
}

} // binding
} // prop
} // pv
