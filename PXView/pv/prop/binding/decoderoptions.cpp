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

#include <libsigrokdecode.h>

#include "pv/prop/binding/decoderoptions.h"

#include "pv/data/stack/decoderstack.h"
#include "pv/data/decode/decoder.h"
#include "pv/base/gvarptr.h"
#include "pv/base/log.h"
#include "pv/prop/bool.h"
#include "pv/prop/double.h"
#include "pv/prop/enum.h"
#include "pv/prop/int.h"
#include "pv/prop/string.h"
#include "pv/ui/langresource.h"
#include "pv/config/appconfig.h"
#include <stdexcept>
#include <cstring>
#include <cstdio>

using namespace std;
 
namespace pv {
namespace prop {
namespace binding {

namespace {

enum class AnalogTriggerFamily { None, Tdm, Pwm };

AnalogTriggerFamily analog_trigger_family(const srd_decoder *d) {
  if (!d || !d->id) return AnalogTriggerFamily::None;
  if (std::strncmp(d->id, "tdm_audio", std::strlen("tdm_audio")) == 0)
    return AnalogTriggerFamily::Tdm;
  if (std::strncmp(d->id, "pwm_waveform", std::strlen("pwm_waveform")) == 0)
    return AnalogTriggerFamily::Pwm;
  return AnalogTriggerFamily::None;
}

int tdm_waveform_enable_channel(const srd_decoder *d, const char *id) {
  if (!d || !d->id || !id || std::strcmp(d->id, "tdm_audio_fast") != 0)
    return -1;
  int ch = -1;
  char tail = '\0';
  if (std::sscanf(id, "ch%d_enable%c", &ch, &tail) == 1 && ch >= 0 && ch < 8)
    return ch;
  return -1;
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

void seed_analog_trigger_memory(data::decode::Decoder *decoder, const srd_decoder *d) {
  if (!decoder) return;
  const auto family = analog_trigger_family(d);
  AppOptions &o = AppConfig::Instance().appOptions;
  const bool tdm = family == AnalogTriggerFamily::Tdm;
  const bool pwm = family == AnalogTriggerFamily::Pwm;
  if ((!tdm && !pwm) || !(tdm ? o.analogDisplayTriggerTdmValid
                               : o.analogDisplayTriggerPwmValid))
    return;
  auto set = [decoder](const char *id, GVariant *v) { decoder->set_option(id, v); };
  set("display_trigger_enable", g_variant_new_boolean(
      tdm ? o.analogDisplayTriggerTdmEnable : o.analogDisplayTriggerPwmEnable));
  set("display_trigger_mode", g_variant_new_string(
      (tdm ? o.analogDisplayTriggerTdmMode : o.analogDisplayTriggerPwmMode).toUtf8().constData()));
  set("display_trigger_channel", g_variant_new_int64(
      tdm ? o.analogDisplayTriggerTdmChannel : o.analogDisplayTriggerPwmChannel));
  set("display_trigger_edge", g_variant_new_string(
      (tdm ? o.analogDisplayTriggerTdmEdge : o.analogDisplayTriggerPwmEdge).toUtf8().constData()));
  set("display_trigger_level", g_variant_new_double(
      tdm ? o.analogDisplayTriggerTdmLevel : o.analogDisplayTriggerPwmLevel));
  set("display_trigger_position", g_variant_new_int64(
      tdm ? o.analogDisplayTriggerTdmPosition : o.analogDisplayTriggerPwmPosition));
}

void remember_analog_trigger_option(const srd_decoder *d, const char *id, GVariant *value) {
  if (!is_analog_trigger_option(id) || !value) return;
  const auto family = analog_trigger_family(d);
  if (family == AnalogTriggerFamily::None) return;
  AppConfig &app = AppConfig::Instance();
  AppOptions &o = app.appOptions;
  auto remember = [&](bool &valid, bool &enable, QString &mode, int &channel,
                      QString &edge, double &level, int &position) {
    valid = true;
    if (!std::strcmp(id, "display_trigger_enable") &&
        g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN))
      enable = g_variant_get_boolean(value);
    else if (!std::strcmp(id, "display_trigger_mode") &&
             g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
      mode = QString::fromUtf8(g_variant_get_string(value, nullptr));
    else if (!std::strcmp(id, "display_trigger_channel") &&
             g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
      channel = int(g_variant_get_int64(value));
    else if (!std::strcmp(id, "display_trigger_edge") &&
             g_variant_is_of_type(value, G_VARIANT_TYPE_STRING))
      edge = QString::fromUtf8(g_variant_get_string(value, nullptr));
    else if (!std::strcmp(id, "display_trigger_level") &&
             g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE))
      level = g_variant_get_double(value);
    else if (!std::strcmp(id, "display_trigger_position") &&
             g_variant_is_of_type(value, G_VARIANT_TYPE_INT64))
      position = int(g_variant_get_int64(value));
  };
  if (family == AnalogTriggerFamily::Tdm)
    remember(o.analogDisplayTriggerTdmValid, o.analogDisplayTriggerTdmEnable,
             o.analogDisplayTriggerTdmMode, o.analogDisplayTriggerTdmChannel,
             o.analogDisplayTriggerTdmEdge, o.analogDisplayTriggerTdmLevel,
             o.analogDisplayTriggerTdmPosition);
  else
    remember(o.analogDisplayTriggerPwmValid, o.analogDisplayTriggerPwmEnable,
             o.analogDisplayTriggerPwmMode, o.analogDisplayTriggerPwmChannel,
             o.analogDisplayTriggerPwmEdge, o.analogDisplayTriggerPwmLevel,
             o.analogDisplayTriggerPwmPosition);
  app.SaveApp();
}

} // namespace


DecoderOptions::DecoderOptions(std::shared_ptr<pv::data::DecoderStack> decoder_stack, data::decode::Decoder *decoder) :
	Binding(),
	_decoder(decoder),
	_decoder_stack(decoder_stack)
{
	if (!_decoder) {
		pxv_warn("%s", "DecoderOptions: _decoder is nullptr");
		throw std::invalid_argument("DecoderOptions: _decoder is nullptr");
	}
	assert(_decoder);

	const srd_decoder *const dec = _decoder->decoder();
	if (!dec) {
		pxv_warn("%s", "DecoderOptions: dec is nullptr");
		throw std::invalid_argument("DecoderOptions: dec is nullptr");
	}
	assert(dec);

	seed_analog_trigger_memory(_decoder, dec);

	bool bLang = AppConfig::Instance().appOptions.transDecoderDlg;

	if (LangResource::Instance()->is_lang_en()){
        bLang = false;
    }

	for (GSList *l = dec->options; l; l = l->next)
	{ 
		const srd_decoder_option *const opt =
			(srd_decoder_option*)l->data;

		const bool is_tdm_fast = dec->id &&
		    std::strcmp(dec->id, "tdm_audio_fast") == 0;
		if (is_tdm_fast && std::strcmp(opt->id, "output") == 0)
		    continue;

		const char *desc_str = nullptr;
		const char *lang_str = nullptr;

        if (opt->idn != nullptr && LangResource::Instance()->is_lang_en() == false){
            lang_str = LangResource::Instance()->get_lang_text(STR_PAGE_DECODER, opt->idn, opt->desc);
        }

		if (lang_str != nullptr && bLang){
            desc_str = lang_str;
        }
        else{
            desc_str = opt->desc;
        }

		const QString name = QString::fromUtf8(desc_str);

		const Property::Getter getter = [this, opt]() {
			return this->getter(opt->id);
		};
		const Property::Setter setter = [this, opt](GVariant *v) {
			this->setter(opt->id, v);
		};

		Property *prop = nullptr;

		const int waveform_ch = tdm_waveform_enable_channel(dec, opt->id);
		if (waveform_ch >= 0) {
		    const QString ch_name = QStringLiteral("CH%1").arg(waveform_ch);
		    prop = new Bool(ch_name, ch_name, getter, setter, true, true);
		} else if (opt->values)
            prop = bind_enum(name, opt, getter, setter);
		else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("d"))) {
      std::optional<std::pair<double, double>> range = std::nullopt;
      std::optional<double> step = std::nullopt;
      QString suffix;
      if (std::strstr(opt->id, "_vpos")) {
        range = std::make_pair(-800.0, 800.0);
      } else if (dec->id &&
                 std::strncmp(dec->id, "pwm_waveform",
                              std::strlen("pwm_waveform")) == 0 &&
                 std::strcmp(opt->id, "filter_cutoff_hz") == 0) {
        range = std::make_pair(0.01, 10000000.0);
        suffix = " Hz";
      }
      prop = new Double(name, name, 2, suffix, range, step, getter, setter);
    } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("b")))
            prop = new Bool(name, name, getter, setter);
		else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("x")))
			prop = new Int(name, name, "", std::nullopt, getter, setter);
		else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("s")))
			prop = new String(name, name, getter, setter);
		else
			continue;

		_properties.push_back(prop);
	}
}

Property* DecoderOptions::bind_enum(
	const QString &name, const srd_decoder_option *option,
	Property::Getter getter, Property::Setter setter)
{
    std::vector<std::pair<GVarPtr, QString> > values;
	for (GSList *l = option->values; l; l = l->next) {
		GVariant *const var = (GVariant*)l->data;
		if (!var) {
			pxv_warn("%s", "DecoderOptions::bind_enum: var is nullptr, skipping");
			continue;
		}
		assert(var);
		values.push_back(make_pair(GVarPtr(var), print_gvariant(var)));
	}

    return new Enum(name, name, values, getter, setter);
}

GVariant* DecoderOptions::getter(const char *id)
{
	GVariant *val = nullptr;

	if (!_decoder) {
		pxv_warn("%s", "DecoderOptions::getter: _decoder is nullptr");
		return nullptr;
	}
	assert(_decoder);

    const srd_decoder *const definition = _decoder->decoder();
    if (std::strcmp(id, "realtime_decode") == 0 && definition && definition->id &&
        std::strncmp(definition->id, "tdm_audio", std::strlen("tdm_audio")) == 0) {
        return g_variant_ref_sink(g_variant_new_boolean(
            AppConfig::Instance().appOptions.tdmRealtimeDecode));
    }
    if (GVariant *remembered = remembered_analog_trigger_value(definition, id))
        return remembered;

	// Get the value from the hash table if it is already present
	const map<string, GVariant*>& options = _decoder->options();
	auto iter = options.find(id);

	if (iter != options.end())
		val = (*iter).second;
	else
	{
		assert(_decoder->decoder());

		// Get the default value if not
		for (GSList *l = _decoder->decoder()->options; l; l = l->next)
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

void DecoderOptions::setter(const char *id, GVariant *value)
{
	if (!_decoder) {
		pxv_warn("%s", "DecoderOptions::setter: _decoder is nullptr");
		return;
	}
	assert(_decoder);

    const srd_decoder *const definition = _decoder->decoder();
    if (std::strcmp(id, "realtime_decode") == 0 && value && definition &&
        definition->id &&
        std::strncmp(definition->id, "tdm_audio", std::strlen("tdm_audio")) == 0 &&
        g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        AppConfig &app = AppConfig::Instance();
        app.appOptions.tdmRealtimeDecode = g_variant_get_boolean(value);
        app.SaveApp();
    }
    remember_analog_trigger_option(definition, id, value);

    const int waveform_ch = tdm_waveform_enable_channel(definition, id);
    if (waveform_ch >= 0 && value &&
        g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) {
      if (auto stack = _decoder_stack.lock()) {
        const bool visible = g_variant_get_int64(value) != 0;
        for (const auto &ch_data : stack->analog_data_copy()) {
          if (ch_data && ch_data->channel() == waveform_ch) {
            ch_data->set_visible(visible);
            break;
          }
        }
      }
    }

	_decoder->set_option(id, value);
}

} // binding
} // prop
} // pv
