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

#include "datasource.h"

#include "../pxvdef.h"
#include <libsigrok/libsigrok.h>

namespace pv {
namespace data {

// Out-of-line default implementations for the session-facade hooks declared
// in datasource.h. Moving these here lets datasource.h drop its
// `#include <libsigrok/libsigrok.h>` (Track A pollution regression) so the
// libsigrok header no longer leaks into every translation unit that includes
// datasource.h.
//
// Only SigSession overrides these with real behaviour; SessionDocument and
// SessionSnapshot inherit the no-op defaults defined below.

// ---- Data access defaults (return 0/false so stubs inherit safely) ----
uint64_t DataSource::cur_snap_samplerate() { return 0; }
uint64_t DataSource::cur_samplelimits() { return 0; }
double DataSource::cur_sampletime() { return 0; }
uint64_t DataSource::get_trigger_pos() { return 0; }
double DataSource::cur_view_time() { return 0; }
bool DataSource::is_running_status() { return false; }

bool DataSource::is_stopped_status() { return false; }
void DataSource::refresh(int holdtime) { (void)holdtime; }
bool DataSource::stop_capture() { return false; }
bool DataSource::start_capture(bool instant, data::SessionDocument *owner) {
    (void)instant; (void)owner; return false;
}
// Task D6.1: DevMode signal forwards — default no-ops so SessionDocument /
// SessionSnapshot stubs inherit safely. SigSession overrides with real work.
bool DataSource::switch_work_mode(int mode) { (void)mode; return false; }
void DataSource::session_save() {}
void DataSource::close_file(unsigned long long dev_handle) { (void)dev_handle; }
// Task D6.2: repeat-hold percentage — default 0; SigSession overrides.
int DataSource::get_repeat_hold() { return 0; }
bool DataSource::trigd() { return false; }
uint8_t DataSource::trigd_ch() { return 0; }
bool DataSource::get_data_auto_lock() { return false; }
void DataSource::data_auto_lock(int lock) { (void)lock; }
void DataSource::auto_end() {}
SessionDocument *DataSource::get_active_document() { return nullptr; }
void DataSource::decode_done() {}

// Task C1: default returns empty — only SigSession overrides with real
// computation via core::MeasureCalculator. SessionDocument/SessionSnapshot
// stubs inherit this no-op (they have no live DSO data to measure).
// Non-const to match the header declaration (other data accessors such as
// get_signal_models / get_decoder_stacks / get_dso_snapshot are also
// non-const because they read non-const SessionStateContext state).
std::vector<api::MeasurementValue> DataSource::get_measurements(
    int channel_index,
    int view_rect_height) {
    (void)channel_index;
    (void)view_rect_height;
    return {};
}

// Task C2: cursor state defaults — only SigSession overrides with real
// state via SessionStateContext::cursor_registry(). SessionDocument /
// SessionSnapshot stubs inherit these no-ops (cursor state lives in the
// live session only, not in saved documents/snapshots).
std::vector<core::CursorEntry> DataSource::get_cursors() const { return {}; }
int  DataSource::add_cursor(uint64_t sample_position) { (void)sample_position; return -1; }
bool DataSource::remove_cursor(int index) { (void)index; return false; }
bool DataSource::set_cursor_position(int index, uint64_t sample_position) {
    (void)index; (void)sample_position; return false;
}
void DataSource::clear_cursors() {}

} // namespace data
} // namespace pv
