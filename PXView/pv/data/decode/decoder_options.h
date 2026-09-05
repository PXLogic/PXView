/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <glib.h>

struct srd_decoder;

namespace pv {
namespace data {
namespace decode {

class Decoder;

/// Analog-trigger option family a decoder belongs to (drives which persisted
/// app options the remembered values come from).
enum class AnalogTriggerFamily { None, Tdm, Pwm };

AnalogTriggerFamily analog_trigger_family(const srd_decoder *d);

bool is_analog_trigger_option(const char *id);

/// Current value of a remembered analog-trigger option (TDM/PWM family), or
/// nullptr when the option is not an analog-trigger one or nothing was
/// persisted. Returns a newly referenced GVariant (caller unrefs).
GVariant *remembered_analog_trigger_value(const srd_decoder *d, const char *id);

/// Read a decoder option's effective value, in priority order:
///   1. tdm_audio "realtime_decode"  -> persisted application option
///   2. remembered analog-trigger value (TDM/PWM)
///   3. the decoder's option hash table (a value the user/decoder set)
///   4. the srd decoder's declared default
///
/// This is pure data access -- it used to live in the View-layer
/// pv::prop::binding::DecoderOptions, which made the headless (pxviewd) link
/// fail and blocked session save/export from ever running without Widgets.
///
/// @return a referenced GVariant the caller must g_variant_unref(), or
///         nullptr when the option id is unknown.
GVariant *get_decoder_option_value(Decoder *dec, const char *id);

} // namespace decode
} // namespace data
} // namespace pv
