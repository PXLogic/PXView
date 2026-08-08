/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2018 DreamSourceLab <support@dreamsourcelab.com>
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

#include "pv/prop/binding/probeoptions.h"
#include <QObject>
#include <cstdint>
#include "pv/prop/bool.h"
#include "pv/prop/double.h"
#include "pv/prop/enum.h"
#include "pv/prop/int.h"
#include "pv/config/appconfig.h"
#include "pv/base/gvarptr.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"
#include "pv/ui/langresource.h"

using namespace std;

namespace pv {
namespace prop {
namespace binding {

ProbeOptions::ProbeOptions(SigSession *session, struct sr_channel *probe) :
    Binding(),
	_probe(probe)
{
    _device_agent = session->get_device();

    /* Pre-check: only query SR_CONF_PROBE_CONFIGS if the device actually
     * advertises it in SR_CONF_DEVICE_OPTIONS. PXLogic and most upstream
     * drivers don't support this key (only demo does); querying it directly
     * floods the log with "Option 'probe_configs' not available" per
     * channel. */
    GVariant *gvar_devopts = _device_agent->get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS);
    bool has_probe_configs = false;
    if (gvar_devopts) {
        gsize num_devopts;
        const uint32_t *devopts = (const uint32_t *)g_variant_get_fixed_array(
            gvar_devopts, &num_devopts, sizeof(uint32_t));
        for (gsize i = 0; i < num_devopts; i++) {
            if ((devopts[i] & 0x1fffffff) == SR_CONF_PROBE_CONFIGS) {
                has_probe_configs = true;
                break;
            }
        }
        g_variant_unref(gvar_devopts);
    }
    if (!has_probe_configs)
        return;

    GVariant *gvar_opts = _device_agent->get_config_list(nullptr, SR_CONF_PROBE_CONFIGS);
    if (gvar_opts == nullptr){
		/* Driver supports no device instance options. */
        return;
    }

	gsize num_opts;
	const int *const options = (const int32_t *)g_variant_get_fixed_array(
		gvar_opts, &num_opts, sizeof(int32_t));

	for (unsigned int i = 0; i < num_opts; i++)
    {
		const struct sr_config_info *const info =
			_device_agent->get_config_info(options[i]);

		if (!info)
			continue;

		const int key = info->key;

        GVariant *gvar_list = _device_agent->get_config_list(nullptr, key);

        const QString name(info->name);
        const char *label_char =  LangResource::Instance()->get_lang_text(STR_PAGE_DSL, info->name, info->name);
        const QString label(label_char);

		switch(key)
		{
        case SR_CONF_PROBE_VDIV:
            bind_vdiv(name, label, gvar_list);
            break;

        case SR_CONF_PROBE_COUPLING:
            bind_coupling(name, label, gvar_list);
            break;

        case SR_CONF_PROBE_MAP_MIN:
        case SR_CONF_PROBE_MAP_MAX:
            bind_double(name, label, key, "",
                        pair<double, double>(-999999.99, 999999.99), 2, 0.01);
            break;

        case SR_CONF_PROBE_MAP_UNIT:
            bind_enum(name, label, key, gvar_list);
			break;

        case SR_CONF_PROBE_MAP_DEFAULT:
            bind_bool(name, label, key);
            break;

case SR_CONF_PATTERN_MODE:
bind_enum(name, label, key, gvar_list, print_pattern);
break;
		}

		if (gvar_list)
			g_variant_unref(gvar_list);
	}
    g_variant_unref(gvar_opts);
}

GVariant* ProbeOptions::config_getter(const struct sr_channel *probe, int key)
{
    if (!_device_agent)
        return nullptr;
    return _device_agent->get_config(key, probe, nullptr);
}

void ProbeOptions::config_setter(struct sr_channel *probe, int key, GVariant* value)
{
    _device_agent->set_config(key, value, probe, nullptr);
}

void ProbeOptions::bind_bool(const QString &name, const QString label, int key)
{
	_properties.push_back(
        new Bool(name, label,
            [this, key]() { return config_getter(_probe, key); },
            [this, key](GVariant* v) { config_setter(_probe, key, v); }));
}

void ProbeOptions::bind_enum(const QString &name, const QString label, int key,
    GVariant *const gvar_list, std::function<QString (GVariant*)> printer)
{
	GVariant *gvar;
	GVariantIter iter;
	std::vector< pair<GVarPtr, QString> > values;

	if (!gvar_list) {
		pxv_warn("%s", "ProbeOptions::bind_enum: gvar_list is nullptr");
		return;
	}
	assert(gvar_list);

	g_variant_iter_init (&iter, gvar_list);
	while ((gvar = g_variant_iter_next_value (&iter)))
		values.push_back(make_pair(GVarPtr(gvar, true), printer(gvar)));

	_properties.push_back(
        new Enum(name, label, values,
            [this, key]() { return config_getter(_probe, key); },
            [this, key](GVariant* v) { config_setter(_probe, key, v); }));
}

void ProbeOptions::bind_int(const QString &name, const QString label, int key, QString suffix,
    std::optional< std::pair<int64_t, int64_t> > range)
{
	_properties.push_back(
        new Int(name, label, suffix, range,
            [this, key]() { return config_getter(_probe, key); },
            [this, key](GVariant* v) { config_setter(_probe, key, v); }));
}

void ProbeOptions::bind_double(const QString &name, const QString label, int key, QString suffix,
    std::optional< std::pair<double, double> > range,
    int decimals, std::optional<double> step)
{
    _properties.push_back(
        new Double(name, label, decimals, suffix, range, step,
            [this, key]() { return config_getter(_probe, key); },
            [this, key](GVariant* v) { config_setter(_probe, key, v); }));
}

void ProbeOptions::bind_vdiv(const QString &name, const QString label,
    GVariant *const gvar_list)
{
    GVariant *gvar_list_vdivs;

    if (!gvar_list) {
        pxv_warn("%s", "ProbeOptions::bind_vdiv: gvar_list is nullptr");
        return;
    }

    /* Driver returns a dict {"vdivs": [uint64...]} (a{sv}). */
    if ((gvar_list_vdivs = g_variant_lookup_value(gvar_list,
            "vdivs", G_VARIANT_TYPE("at"))))
    {
        bind_enum(name, label, SR_CONF_PROBE_VDIV,
            gvar_list_vdivs, print_vdiv);
        g_variant_unref(gvar_list_vdivs);
    } else {
        /* g_variant_lookup_value returned nullptr — either the dict is missing
         * the "vdivs" key or its value type is not "at". Without this branch
         * the vdiv control would silently disappear from the DeviceOptions
         * dialog in ANALOG mode, leaving the user with no way to change
         * probe attenuation. Log enough context to diagnose the driver. */
        pxv_warn("ProbeOptions::bind_vdiv: key 'vdivs' not found in "
                 "gvar_list (probe index=%d name='%s') — vdiv control "
                 "will not be created",
                 _probe ? _probe->index : -1,
                 (_probe && _probe->name) ? _probe->name : "(nullptr)");
    }
}

void ProbeOptions::bind_coupling(const QString &name, const QString label,
    GVariant *const gvar_list)
{
    GVariant *gvar_list_coupling;

    if (!gvar_list) {
        pxv_warn("%s", "ProbeOptions::bind_coupling: gvar_list is nullptr");
        return;
    }

    /* Driver returns a dict {"coupling": [int32...]} (a{sv}). int32 matches
     * sr_key_info_config SR_T_INT32 so SET passes sr_variant_type_check. */
    if ((gvar_list_coupling = g_variant_lookup_value(gvar_list,
            "coupling", G_VARIANT_TYPE("ai"))))
    {
        bind_enum(name, label, SR_CONF_PROBE_COUPLING,
            gvar_list_coupling, print_coupling);
        g_variant_unref(gvar_list_coupling);
    } else {
        /* g_variant_lookup_value returned nullptr — either the dict is missing
         * the "coupling" key or its value type is not "ai". Without this
         * branch the coupling control would silently disappear from the
         * DeviceOptions dialog in ANALOG mode. Log enough context to
         * diagnose the driver. */
        pxv_warn("ProbeOptions::bind_coupling: key 'coupling' not found in "
                 "gvar_list (probe index=%d name='%s') — coupling control "
                 "will not be created",
                 _probe ? _probe->index : -1,
                 (_probe && _probe->name) ? _probe->name : "(nullptr)");
    }
}

QString ProbeOptions::print_gvariant(GVariant *const gvar)
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

QString ProbeOptions::print_vdiv(GVariant *const gvar)
{
    uint64_t p, q;
    g_variant_get(gvar, "t", &p);
    if (p < 1000ULL) {
        q = 1000;
    } else {
        q = 1;
        p /= 1000;
    }
	return QString(sr_voltage_string(p, q));
}

QString ProbeOptions::print_pattern(GVariant *const gvar)
{
QString s = print_gvariant(gvar);
/* Translate driver pattern strings to localized display text.
 * The driver returns lowercase English strings ("random", "sine",
 * "square", "sawtooth", "triangle"); the Enum property stores the
 * GVariant (raw driver value) for SET, but the display string comes
 * from this printer, so we can freely translate it. */
if (s == "random")
return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PATTERN_RANDOM), "Random");
if (s == "sine")
return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PATTERN_SINE), "Sine");
if (s == "square")
return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PATTERN_SQUARE), "Square");
if (s == "sawtooth")
return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PATTERN_SAWTOOTH), "Sawtooth");
if (s == "triangle")
return L_S(STR_PAGE_DLG, S_ID(IDS_DLG_PATTERN_TRIANGLE), "Triangle");
return s;
}

QString ProbeOptions::print_coupling(GVariant *const gvar)
{
    /* Driver LIST now returns int32 ("i") to match sr_key_info_config
     * SR_T_INT32. Old code used "y" (byte) which caused SET to be rejected
     * by sr_variant_type_check. */
    int32_t coupling;
    g_variant_get(gvar, "i", &coupling);
    if (coupling == SR_DC_COUPLING) {
        return QString("DC");
    } else if (coupling == SR_AC_COUPLING) {
        return QString("AC");
    } else if (coupling == SR_GND_COUPLING) {
        return QString("GND");
    } else {
        return QString("Undefined");
    }
}

} // binding
} // prop
} // pv

