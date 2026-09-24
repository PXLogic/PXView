/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2015 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include "pv/prop/binding/inputoutput.h"

#include <optional>
#include <utility>
#include <vector>

#include "pv/base/gvarptr.h"
#include "pv/base/log.h"
#include "pv/data/sr_options.h"
#include "pv/prop/bool.h"
#include "pv/prop/double.h"
#include "pv/prop/enum.h"
#include "pv/prop/int.h"
#include "pv/prop/string.h"

namespace pv {
namespace prop {
namespace binding {

InputOutput::InputOutput(const struct sr_option **options, const QVariantMap &prefill)
{
    if (!options)
        return;

    for (int i = 0; options[i]; i++) {
        const struct sr_option *const option = options[i];
        if (!option->id)
            continue;

        // Initial value: the caller's prefill when it fits the declared type,
        // else the module's own default. An option without any default value
        // cannot be represented by any widget, so it is skipped.
        GVariant *created = nullptr;
        const QString option_id = QString::fromUtf8(option->id);
        if (prefill.contains(option_id)) {
            created = data::sr_options::make_variant(option, prefill.value(option_id));
            if (!created) {
                pxv_warn("InputOutput: ignoring prefill for option '%s' "
                         "(does not fit the declared type)",
                         option->id);
            }
        }

        GVariant *const initial = created ? created : option->def;
        if (!initial) {
            pxv_warn("InputOutput: option '%s' has no default value, skipped",
                     option->id);
            continue;
        }

        const std::string id(option->id);
        // The module owns its `def` reference; our map keeps its own.
        _values[id] = g_variant_ref(initial);
        if (created)
            g_variant_unref(created);

        const Property::Getter getter = [this, id]() -> GVariant * {
            const auto it = _values.find(id);
            if (it == _values.end() || !it->second)
                return nullptr;
            // Property::Getter must return a new reference: Int/String/Bool
            // get_widget() unrefs the value after reading it.
            return g_variant_ref(it->second);
        };

        const Property::Setter setter = [this, id](GVariant *value) {
            if (!value)
                return;
            // Widgets hand over either a floating variant (g_variant_new_*)
            // or an already sunk one (Enum's GVarPtr); normalize to "we own
            // exactly one reference" in both cases.
            GVariant *const owned = g_variant_is_floating(value)
                ? g_variant_ref_sink(value)
                : g_variant_ref(value);

            const auto it = _values.find(id);
            if (it != _values.end()) {
                if (it->second)
                    g_variant_unref(it->second);
                it->second = owned;
            } else {
                _values.emplace(id, owned);
            }
        };

        // Widgets show label(); name() is only used for widget-specific
        // behaviour (e.g. String's path/dir detection), so feed the raw id.
        const QString name = QString::fromUtf8(option->id);
        const QString label = QString::fromUtf8(option->name ? option->name
                                                             : option->id);

        const GVariantType *const type = g_variant_get_type(initial);
        Property *prop = nullptr;

        if (option->values) {
            prop = bind_enum(label, option, getter, setter);
        } else if (type && g_variant_type_equal(type, G_VARIANT_TYPE_BOOLEAN)) {
            prop = new Bool(name, label, getter, setter);
        } else if (type && g_variant_type_equal(type, G_VARIANT_TYPE_DOUBLE)) {
            prop = new Double(name, label, 2, QString(), std::nullopt,
                              std::nullopt, getter, setter);
        } else if (data::sr_options::is_integer_type(type)) {
            prop = new Int(name, label, QString(), std::nullopt, getter, setter);
        } else if (type && g_variant_type_equal(type, G_VARIANT_TYPE_STRING)) {
            prop = new String(name, label, getter, setter);
        } else if (type) {
            const char *type_str = g_variant_type_peek_string(type);
            pxv_warn("InputOutput: option '%s' has unsupported type '%s', "
                     "skipped", option->id, type_str ? type_str : "?");
        }

        if (prop)
            _properties.push_back(prop);
    }
}

InputOutput::~InputOutput()
{
    for (auto &entry : _values) {
        if (entry.second)
            g_variant_unref(entry.second);
    }
    _values.clear();
}

GHashTable *InputOutput::make_options_table() const
{
    return data::sr_options::make_option_table(_values);
}

Property *InputOutput::bind_enum(const QString &label,
                                const struct sr_option *option,
                                Property::Getter getter, Property::Setter setter)
{
    std::vector<std::pair<GVarPtr, QString>> values;

    for (GSList *l = option->values; l; l = l->next) {
        GVariant *const var = reinterpret_cast<GVariant *>(l->data);
        if (!var) {
            pxv_warn("%s", "InputOutput::bind_enum: skipping NULL value");
            continue;
        }
        values.emplace_back(GVarPtr(var), Binding::print_gvariant(var));
    }

    return new Enum(label, label, values, getter, setter);
}

} // binding
} // prop
} // pv
