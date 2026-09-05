/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/data/decode/decoder_options.h"

#include <cstring>
#include <cassert>
#include <map>
#include <string>

#include "pv/config/appconfig.h"
#include "pv/data/decode/decoder.h"

using namespace std;

namespace pv {
namespace data {
namespace decode {

namespace {

} // namespace

AnalogTriggerFamily analog_trigger_family(const srd_decoder *d) {
  if (!d || !d->id) return AnalogTriggerFamily::None;
  if (std::strncmp(d->id, "tdm_audio", std::strlen("tdm_audio")) == 0)
    return AnalogTriggerFamily::Tdm;
  if (std::strncmp(d->id, "pwm_waveform", std::strlen("pwm_waveform")) == 0)
    return AnalogTriggerFamily::Pwm;
  return AnalogTriggerFamily::None;
}

bool is_analog_trigger_option(const char *id) {
  return id && (!std::strcmp(id, "display_trigger_enable") ||
                !std::strcmp(id, "display_trigger_mode") ||
                !std::strcmp(id, "display_trigger_channel") ||
                !std::strcmp(id, "display_trigger_edge") ||
                !std::strcmp(id, "display_trigger_level") ||
                !std::strcmp(id, "display_trigger_position"));
}

GVariant *remembered_analog_trigger_value(const srd_decoder *d, const char *id) {
  if (!is_analog_trigger_option(id)) return nullptr;
  const auto family = analog_trigger_family(d);
  const AppOptions &o = AppConfig::Instance().appOptions;
  const bool tdm = family == AnalogTriggerFamily::Tdm;
  const bool pwm = family == AnalogTriggerFamily::Pwm;
  if ((!tdm && !pwm) || !(tdm ? o.analogDisplayTriggerTdmValid
                              : o.analogDisplayTriggerPwmValid))
    return nullptr;
  if (!std::strcmp(id, "display_trigger_enable"))
    return g_variant_ref_sink(g_variant_new_boolean(
        tdm ? o.analogDisplayTriggerTdmEnable : o.analogDisplayTriggerPwmEnable));
  if (!std::strcmp(id, "display_trigger_mode")) {
    const QByteArray v = (tdm ? o.analogDisplayTriggerTdmMode
                              : o.analogDisplayTriggerPwmMode).toUtf8();
    return g_variant_ref_sink(g_variant_new_string(v.constData()));
  }
  if (!std::strcmp(id, "display_trigger_channel"))
    return g_variant_ref_sink(g_variant_new_int64(
        tdm ? o.analogDisplayTriggerTdmChannel : o.analogDisplayTriggerPwmChannel));
  if (!std::strcmp(id, "display_trigger_edge")) {
    const QByteArray v = (tdm ? o.analogDisplayTriggerTdmEdge
                              : o.analogDisplayTriggerPwmEdge).toUtf8();
    return g_variant_ref_sink(g_variant_new_string(v.constData()));
  }
  if (!std::strcmp(id, "display_trigger_level"))
    return g_variant_ref_sink(g_variant_new_double(
        tdm ? o.analogDisplayTriggerTdmLevel : o.analogDisplayTriggerPwmLevel));
  if (!std::strcmp(id, "display_trigger_position"))
    return g_variant_ref_sink(g_variant_new_int64(
        tdm ? o.analogDisplayTriggerTdmPosition : o.analogDisplayTriggerPwmPosition));
  return nullptr;
}

GVariant *get_decoder_option_value(Decoder *dec, const char *id)
{
	GVariant *val = nullptr;

	if (!dec || !id) {
		return nullptr;
	}

    const srd_decoder *const definition = dec->decoder();
    if (std::strcmp(id, "realtime_decode") == 0 && definition && definition->id &&
        std::strncmp(definition->id, "tdm_audio", std::strlen("tdm_audio")) == 0) {
        return g_variant_ref_sink(g_variant_new_boolean(
            AppConfig::Instance().appOptions.tdmRealtimeDecode));
    }
    if (GVariant *remembered = remembered_analog_trigger_value(definition, id)) {
        return remembered;
    }

	// Get the value from the hash table if it is already present
	const map<string, GVariant*>& options = dec->options();
	auto iter = options.find(id);

	if (iter != options.end())
		val = (*iter).second;
	else
	{
		assert(dec->decoder());

		// Get the default value if not
		for (GSList *l = dec->decoder()->options; l; l = l->next)
		{
			const srd_decoder_option *const opt =
				(srd_decoder_option*)l->data;
			if (strcmp(opt->id, id) == 0) {
				val = opt->def;
				break;
			}
		}
	}

	if (val)
		g_variant_ref(val);

	return val;
}

} // namespace decode
} // namespace data
} // namespace pv
