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

#include "deviceoptions.h"

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/bind.hpp>
#include <QObject>
#include <stdint.h>
#include "../bool.h"
#include "../string.h"
#include "../double.h"
#include "../enum.h"
#include "../int.h"
#include "../../config/appconfig.h"
#include "../../log.h"
#include "../../sigsession.h"
#include "../../deviceagent.h"
#include "../../ui/langresource.h"
 
using namespace std;

namespace pv {
namespace prop {
namespace binding {

DeviceAgent* DeviceOptions::_static_device_agent = nullptr;

DeviceOptions::DeviceOptions(SigSession *session)
{
	GVariant *gvar_opts, *gvar_list;
	gsize num_opts;

	_device_agent = session->get_device();
	_static_device_agent = _device_agent;

    pxv_info("DeviceOptions binding: driver=%s, is_hardware=%d, is_dsl=%d, is_stream=%d",
             _device_agent->driver_name().toUtf8().constData(),
             _device_agent->is_hardware(),
             _device_agent->is_dsl_device(),
             _device_agent->is_stream_mode());

	gvar_opts = _device_agent->get_config_list(NULL, SR_CONF_DEVICE_OPTIONS);

    if (gvar_opts == NULL) {
        pxv_warn("DeviceOptions binding: get_config_list(SR_CONF_DEVICE_OPTIONS) returned NULL");
		/* Driver supports no device instance options. */
		return;
    }

	const uint32_t *const options = (const uint32_t *)g_variant_get_fixed_array(
		gvar_opts, &num_opts, sizeof(uint32_t));

    pxv_info("DeviceOptions binding: num_opts=%zu", num_opts);

	for (unsigned int i = 0; i < num_opts; i++) {
		/* Mask off capability bits (SR_CONF_GET/SET/LIST, top 3 bits) to
		 * get the bare config key. pxlogic.c now routes DEVICE_OPTIONS through
		 * STD_CONFIG_LIST which returns uint32 entries with cap bits
		 * (e.g. SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST).
		 * sr_key_info_get only recognizes bare keys.
		 * SR_CONF_MASK = 0x1fffffff (libsigrok-internal.h, not public). */
		const int key = (int)(options[i] & 0x1fffffff);

		const struct sr_config_info *const info =
			_device_agent->get_config_info(key);

		if (!info)
			continue;

		/* 只在驱动声明了 SR_CONF_LIST 能力位时才查询可选值列表。
		 * 否则 hwdriver.c check_key() 会因 pub_opt & SR_CONF_LIST == 0
		 * 打印 "Option 'xxx' not available to list" 错误日志。 */
		const bool can_list = (options[i] & SR_CONF_LIST) != 0;
		gvar_list = can_list ? _device_agent->get_config_list(NULL, key) : NULL;

        const QString name(info->name);
        const char *label_char = info->name;
        QString label(label_char);

		switch(key)
		{
		case SR_CONF_SAMPLERATE:
            /* Skip: SamplingBar already provides the sample-rate dropdown in
             * the device-options dock. Creating a duplicate here would show
             * two sample-rate controls. The key stays in devopts[] so
             * hwdriver.c check_key() permits get/set/list from SamplingBar. */
            continue;

		case SR_CONF_CAPTURE_RATIO:
            bind_int(name, label, key, "%", pair<int64_t, int64_t>(0, 100));
			break;

		case SR_CONF_MAX_HEIGHT:
		case SR_CONF_MAX_HEIGHT_VALUE:
		case SR_CONF_INSTANT:
            /* Skip: DeviceOptionsDock 不显示这三个 key。
             * MAX_HEIGHT/MAX_HEIGHT_VALUE 由 SamplingBar 的下拉框控制，
             * INSTANT 由 sidebar 的 SIDEBAR_INSTANT 按钮控制。
             * 驱动 devopts[] 保留声明，其它路径仍可 get/set/list。 */
            continue;

		case SR_CONF_PATTERN_MODE:
		case SR_CONF_BUFFERSIZE:
		case SR_CONF_TRIGGER_SOURCE:
        case SR_CONF_PROBE_EN:
        case SR_CONF_STREAM:
        case SR_CONF_TEST:
        case SR_CONF_PROBE_FACTOR:
            bind_enum(name, label, key, gvar_list);
			break;

		case SR_CONF_OPERATION_MODE:
        case SR_CONF_BUFFER_OPTIONS:
        case SR_CONF_THRESHOLD:
		case SR_CONF_FILTER: 
		case SR_CONF_EX_TRIGGER_MATCH:
			bind_list(name, label, key, gvar_list);
			break;

        case SR_CONF_VTH:
            bind_double(name, label, key, "V", pair<double, double>(0.0, 6.0), 1, 0.1);
            break;

        case SR_CONF_PWM0_FREQ:
            bind_double(name, "PWM0 Freq", key, "Hz", pair<double, double>(0, 1000000), 1, 1);
            break;
        case SR_CONF_PWM1_FREQ:
            bind_double(name, "PWM1 Freq", key, "Hz", pair<double, double>(0, 1000000), 1, 1);
            break;

        case SR_CONF_PWM0_DUTY:
            bind_double(name, "PWM0 Duty", key, "%", pair<double, double>(0, 100), 1, 1);
            break;
        case SR_CONF_PWM1_DUTY:
            bind_double(name, "PWM1 Duty", key, "%", pair<double, double>(0, 100), 1, 1);
            break;

		case SR_CONF_RLE:
        case SR_CONF_CLOCK_TYPE:
        case SR_CONF_CLOCK_EDGE:
		case SR_CONF_TRIGGER_OUT:
            bind_bool(name, label, key);
            break;
        case SR_CONF_PWM0_EN:
            bind_bool(name, "PWM0 EN", key);
            break;
        case SR_CONF_PWM1_EN:
            bind_bool(name, "PWM1 EN", key);
            break;

		case SR_CONF_TIMEBASE:
            /* Skip: SamplingBar 已经提供时基下拉框（DSO 模式下由
             * SR_CONF_MAX/MIN_TIMEBASE 驱动生成 500ms/div ... 10ns/div
             * 列表）。此处再次绑定会出现重复控件，且 print_timebase 期望
             * GVariant 类型为 "(tt)" 元组，但 demo 等驱动 config_list
             * 返回 uint64 数组（std_gvar_array_u64），类型不匹配导致
             * g_variant_get 取到未初始化值，下拉框显示成错误的"8s"。
             * 驱动 devopts[] 保留 SR_CONF_TIMEBASE 声明，hwdriver.c
             * check_key() 仍允许 SamplingBar 路径 get/set/list。 */
            continue;

        case SR_CONF_BANDWIDTH_LIMIT:
            bind_bandwidths(name, label, key, gvar_list);
            break;

        default:
            gvar_list = NULL;
		}

		if (gvar_list)
			g_variant_unref(gvar_list);
	}
    if (gvar_opts)
        g_variant_unref(gvar_opts);

    // App-layer C-class controls (DISK_CACHE_ENABLE/PATH, STREAM_BUFF,
    // STREAM_MEM_BUFF): served by DeviceAgent's app-layer config state for
    // ALL devices (including PXLogic — these are not driver-backed anymore).
    // Show them for any hardware device so users can configure disk cache
    // regardless of whether the driver is PXLogic or fx2lafw.
    // Labels match dsl_label.json ids so LangResource translates them.
    if (_device_agent->is_hardware()) {
        pxv_info("DeviceOptions binding: adding app-layer stream/disk-cache controls");
        // Non-DSL hardware devices (fx2lafw etc.): add a Buffer/Stream run-mode
        // dropdown via SR_CONF_OPERATION_MODE. DeviceAgent serves the string
        // list/get/set from app-layer state so the driver doesn't need to
        // implement these. DSL/PXLogic devices already declare
        // SR_CONF_OPERATION_MODE in their devopts and handled by the switch
        // above (bind_list), so we only bind here for non-DSL devices.
        if (!_device_agent->is_dsl_device()) {
            GVariant *opmode_list = _device_agent->get_config_list(
                NULL, SR_CONF_OPERATION_MODE);
            bind_list("operation_mode", "Operation mode",
                      SR_CONF_OPERATION_MODE, opmode_list);
            if (opmode_list)
                g_variant_unref(opmode_list);
        }
        bind_bool("disk_cache_enable", "Disk Cache Enable",
                  SR_CONF_DISK_CACHE_ENABLE);
        bind_string("disk_cache_path", "Disk Cache Path",
                    SR_CONF_DISK_CACHE_PATH);
        bind_double("stream_buff", "Disk Buff Size (with cache)",
                    SR_CONF_STREAM_BUFF,
                    "GB", pair<double, double>(1, 1024), 0, 1);
        bind_double("stream_mem_buff", "Mem Buff Size (no cache)",
                    SR_CONF_STREAM_MEM_BUFF,
                    "GB", pair<double, double>(1, 64), 0, 1);
    } else {
        pxv_info("DeviceOptions binding: skipping app-layer controls "
                 "(is_hardware=%d)",
                 _device_agent->is_hardware());
    }
}

GVariant* DeviceOptions::config_getter(int key)
{ 
	return _static_device_agent->get_config(key);
}

void DeviceOptions::config_setter(int key, GVariant* value)
{
    _static_device_agent->set_config(key, value);
}

void DeviceOptions::bind_bool(const QString &name, const QString label, int key)
{
	// Bool::labeled_widget() returns true, so get_property_form() skips its
	// LangResource translation and the label is shown verbatim on the QCheckBox.
	// Translate here so Bool labels get i18n too.
	QString text = QString::fromUtf8(
		LangResource::Instance()->get_lang_text(STR_PAGE_DSL,
			label.toLocal8Bit().data(), label.toLocal8Bit().data()));
	_properties.push_back(
        new Bool(name, text, bind(config_getter, key),
			bind(config_setter, key, _1)));
}

void DeviceOptions::bind_string(const QString &name, const QString label, int key)
{
	_properties.push_back(
        new String(name, label, bind(config_getter, key),
			bind(config_setter, key, _1)));
}

void DeviceOptions::bind_enum(const QString &name, const QString label, int key,
    GVariant *const gvar_list, boost::function<QString (GVariant*)> printer)
{
	GVariant *gvar;
	GVariantIter iter;
	std::vector< pair<GVariant*, QString> > values;

	if (!gvar_list) {
		pxv_warn("%s", "DeviceOptions::bind_enum: gvar_list is NULL");
		return;
	}
	assert(gvar_list);

	g_variant_iter_init (&iter, gvar_list);

	while ((gvar = g_variant_iter_next_value (&iter)))
	{
		QString v = printer(gvar);
		values.push_back(make_pair(gvar, v));
	}

	_properties.push_back(
        new Enum(name, label, values,
			bind(config_getter, key),
			bind(config_setter, key, _1)));
}

void DeviceOptions::bind_int(const QString &name, const QString label, int key, QString suffix,
    boost::optional< std::pair<int64_t, int64_t> > range)
{
	_properties.push_back(
        new Int(name, label, suffix, range,
			bind(config_getter, key),
			bind(config_setter, key, _1)));
}

void DeviceOptions::bind_double(const QString &name, const QString label, int key, QString suffix,
    boost::optional< std::pair<double, double> > range,
    int decimals, boost::optional<double> step)
{
    _properties.push_back(
        new Double(name, label, decimals, suffix, range, step,
            bind(config_getter, key),
            bind(config_setter, key, _1)));
}

QString DeviceOptions::print_gvariant(GVariant *const gvar)
{
	QString s;

	if (g_variant_is_of_type(gvar, G_VARIANT_TYPE("s"))){
        s = QString::fromUtf8(g_variant_get_string(gvar, NULL));
	}
	else
	{
		gchar *const text = g_variant_print(gvar, FALSE);
        s = QString::fromUtf8(text);
		g_free(text);
	}

	return s;
}

void DeviceOptions::bind_samplerate(const QString &name, const QString label,
    GVariant *const gvar_list)
{
	GVariant *gvar_list_samplerates;

	if (!gvar_list) {
		pxv_warn("%s", "DeviceOptions::bind_samplerate: gvar_list is NULL");
		return;
	}
	assert(gvar_list);

	if ((gvar_list_samplerates = g_variant_lookup_value(gvar_list,
			"samplerate-steps", G_VARIANT_TYPE("at"))))
	{
		gsize num_elements;
		const uint64_t *const elements =
			(const uint64_t *)g_variant_get_fixed_array(
				gvar_list_samplerates, &num_elements, sizeof(uint64_t));

		assert(num_elements == 3);

		_properties.push_back(
			//tr
            new Double(name, label, 0, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_HZ), "Hz"),
				make_pair((double)elements[0], (double)elements[1]),
						(double)elements[2],
				bind(samplerate_double_getter),
				bind(samplerate_double_setter, _1)));

		g_variant_unref(gvar_list_samplerates);
	}
	else if ((gvar_list_samplerates = g_variant_lookup_value(gvar_list,
			"samplerates", G_VARIANT_TYPE("at"))))
	{
        bind_enum(name, label, SR_CONF_SAMPLERATE,
			gvar_list_samplerates, print_samplerate);
		g_variant_unref(gvar_list_samplerates);
	}
}

QString DeviceOptions::print_samplerate(GVariant *const gvar)
{
	char *const s = sr_samplerate_string(
		g_variant_get_uint64(gvar));
	const QString qstring(s);
	g_free(s);
	return qstring;
}

GVariant* DeviceOptions::samplerate_double_getter()
{
    GVariant *const gvar = config_getter(SR_CONF_SAMPLERATE);

	if(!gvar)
		return NULL;

	GVariant *const gvar_double = g_variant_new_double(
		g_variant_get_uint64(gvar));

	g_variant_unref(gvar);

	return gvar_double;
}

void DeviceOptions::samplerate_double_setter(GVariant *value)
{
	GVariant *const gvar = g_variant_new_uint64(
		g_variant_get_double(value));
	config_setter(SR_CONF_SAMPLERATE, gvar);
}

QString DeviceOptions::print_timebase(GVariant *const gvar)
{
	uint64_t p, q;
	g_variant_get(gvar, "(tt)", &p, &q);
	return QString(sr_period_string(p, q));
}

QString DeviceOptions::print_vdiv(GVariant *const gvar)
{
	uint64_t p, q;
	g_variant_get(gvar, "(tt)", &p, &q);
	return QString(sr_voltage_string(p, q));
}

void DeviceOptions::bind_bandwidths(const QString &name, const QString label, int key,
    GVariant *const gvar_list, boost::function<QString (GVariant*)> printer)
{
	(void)printer;

	bool bw_limit = false;
	GVariant *gvar;
	std::vector< pair<GVariant*, QString> > values;

	if (!gvar_list) {
		pxv_warn("%s", "DeviceOptions::bind_bandwidths: gvar_list is NULL");
		return;
	}
	assert(gvar_list);

	/* Task 10.5: config_list now returns a GVariant string array
	 * (g_variant_new_strv) instead of a uint64 bare-pointer cast.
	 * Note: g_variant_get_strv returns const gchar** (the strings are
	 * owned by the variant); only the array itself is freed via g_free. */
	gsize n_items;
	const gchar **strs = g_variant_get_strv(gvar_list, &n_items);
	if (!strs) {
		pxv_warn("%s", "DeviceOptions::bind_bandwidths: strs is NULL");
		return;
	}

	_device_agent->get_config_bool(SR_CONF_BANDWIDTH, bw_limit);

    if (bw_limit == false){
        g_free((gpointer)strs);
        return;
	}

	for (gsize i = 0; i < n_items; i++)
	{
		QString v = QString::fromUtf8(
			LangResource::Instance()->get_lang_text(STR_PAGE_DSL, strs[i], strs[i]));
		gvar = g_variant_new_string(strs[i]);
		values.push_back(make_pair(gvar, v));
	}

	g_free((gpointer)strs);

	_properties.push_back(
        new Enum(name, label, values,
			bind(config_getter, key),
			bind(config_setter, key, _1)));
}

void DeviceOptions::bind_list(const QString &name, const QString label, int key, GVariant *const gvar_list)
{
	GVariant *gvar;
	std::vector< pair<GVariant*, QString> > values;

	if (!gvar_list) {
		pxv_warn("%s", "DeviceOptions::bind_list: gvar_list is NULL");
		return;
	}
	assert(gvar_list);

	/* Task 10.5: config_list now returns a GVariant string array
	 * (g_variant_new_strv) instead of a uint64 bare-pointer cast. The
	 * selected string is passed to config_setter as a string GVariant,
	 * which the driver validates via std_str_idx. */
	gsize n_items;
	const gchar **strs = g_variant_get_strv(gvar_list, &n_items);
	if (!strs) {
		pxv_warn("%s", "DeviceOptions::bind_list: strs is NULL");
		return;
	}

	for (gsize i = 0; i < n_items; i++)
	{
		QString v = QString::fromUtf8(
			LangResource::Instance()->get_lang_text(STR_PAGE_DSL, strs[i], strs[i]));
		gvar = g_variant_new_string(strs[i]);
		values.push_back(make_pair(gvar, v));
	}

	g_free((gpointer)strs);

	_properties.push_back(
        new Enum(name, label, values,
			bind(config_getter, key),
			bind(config_setter, key, _1)));
}

} // binding
} // prop
} // pv

