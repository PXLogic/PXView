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

#ifndef PXVIEW_PV_DATA_DATASOURCE_H
#define PXVIEW_PV_DATA_DATASOURCE_H

#include <cstdint>
#include <vector>
#include <memory>
#include <list>

#include "pv/api/types.h"  // api::MeasurementValue (for get_measurements return type)
#include "pv/core/cursorregistry.h"  // core::CursorEntry (for get_cursors return type)

// Spec v2 Task 8: DataSource now inherits from 5 fine-grained interfaces
// (IDataSource, ISignalSource, IDecoderHost, ICaptureControl, IMeasureSource).
// This enables View-layer classes to depend on only the interface they need
// (ISP), while SigSession (which inherits DataSource) automatically satisfies
// all 5 interfaces.  DataSource itself remains as the composite fat-interface
// for backward compatibility; new code should depend on the specific interface.
#include "pv/data/idata_source.h"
#include "pv/data/isignal_source.h"
#include "pv/data/idecoder_host.h"
#include "pv/data/icapture_control.h"
#include "pv/data/imeasure_source.h"

struct srd_decoder;
class DecoderStatus;
class DeviceAgent;  // forward declaration (global namespace); defined in deviceagent.h

namespace pv {

namespace data {

class LogicSnapshot;
class AnalogSnapshot;
class DsoSnapshot;
class Snapshot;
class SignalModel;
class LissajousModel;
class DecoderStack;
class SpectrumStack;
class MathStack;
class SessionDocument;
class TriggerConfig;
namespace decode { class Decoder; }

// Spec v2 Task 8: DataSource inherits all 5 fine-grained interfaces.
// Existing methods (some pure, some with default impls) override the
// pure virtuals from each interface.  SigSession inherits DataSource and
// therefore satisfies all 5 interfaces — a SigSession* is implicitly
// convertible to any of IDataSource*/ISignalSource*/IDecoderHost*/
// ICaptureControl*/IMeasureSource*.
class DataSource
    : public IDataSource,
      public ISignalSource,
      public IDecoderHost,
      public ICaptureControl,
      public IMeasureSource
{
public:
    // virtual ~DataSource() = default; — inherited from interfaces
    virtual ~DataSource() {}

    // ---- Device access (Task D4: capability queries / probe config reads
    //      with no SignalModel getter). Returns the underlying DeviceAgent so
    //      the View layer can issue get_config_* queries for device-level
    //      constants (SR_CONF_UNIT_BITS, SR_CONF_REF_MIN/MAX) and probe
    //      fields not mirrored in SignalModel (SR_CONF_PROBE_MAP_UNIT/MIN/MAX,
    //      SR_CONF_PROBE_HW_OFFSET). Write paths MUST go through SignalModel
    //      setters instead. Default returns nullptr; only SigSession overrides
    //      to return the real device. SessionDocument/SessionSnapshot stubs
    //      inherit the nullptr default (analog signals are only created from
    //      the live session, so this is never null in practice for View signals).
    virtual DeviceAgent* device() override { return nullptr; }

    // ---- New v2 pure-data interface (no view::* types) ----
    virtual std::vector<std::shared_ptr<SignalModel>>& get_signal_models() = 0;
    virtual std::vector<std::shared_ptr<DecoderStack>>& get_decoder_stacks(
        SessionDocument *doc = nullptr) = 0;
    virtual std::vector<std::shared_ptr<SpectrumStack>>& get_spectrum_stacks() = 0;
    virtual std::shared_ptr<MathStack> get_math_stack() = 0;
    virtual LissajousModel* get_lissajous_model() override = 0;

    // ---- Data access ----
    // cur_snap_samplerate/cur_samplelimits/cur_sampletime/get_trigger_pos
    // have out-of-line default implementations (return 0) in datasource.cpp
    // so SessionDocument/SessionSnapshot stubs may inherit the defaults if
    // they do not need real values. SigSession overrides with real data.
    virtual uint64_t cur_snap_samplerate();
    virtual uint64_t cur_samplelimits();
    virtual double cur_sampletime();
    virtual double cur_snap_sampletime() = 0;
    virtual data::LogicSnapshot* get_logic_snapshot() = 0;
    // Shared-pointer variant for lifetime safety: callers that store a raw
    // pointer (e.g. LogicSignal::_data) can also hold the shared_ptr to
    // prevent use-after-free when the document releases its reference.
    virtual std::shared_ptr<data::LogicSnapshot> get_logic_snapshot_shared() { return nullptr; }
    virtual data::AnalogSnapshot* get_analog_snapshot() = 0;
    // Shared-pointer variant for lifetime safety: callers that store a raw
    // pointer (e.g. AnalogSignal::_data) can also hold the shared_ptr to
    // prevent use-after-free when the document releases its reference.
    virtual std::shared_ptr<data::AnalogSnapshot> get_analog_snapshot_shared() { return nullptr; }
    virtual data::DsoSnapshot* get_dso_snapshot() = 0;
    // Shared-pointer variant for lifetime safety: callers that store a raw
    // pointer (e.g. DsoSignal::_data) can also hold the shared_ptr to
    // prevent use-after-free when the document releases its reference.
    virtual std::shared_ptr<data::DsoSnapshot> get_dso_snapshot_shared() { return nullptr; }
    virtual data::Snapshot* get_snapshot(int type) = 0;
    virtual uint64_t get_trigger_pos();

    // ---- Facade data/status queries (Task D6: route View away from
    //      SigSession facade via DataSource). These let the View layer
    //      query session-level data and capture state without holding a
    //      SigSession* for anything other than broadcast/facade. Only
    //      SigSession provides meaningful implementations; SessionDocument
    //      and SessionSnapshot provide no-op/default stubs.
    virtual double cur_view_time();
    virtual int get_map_zoom() = 0;
    virtual double get_logic_data_view_time() = 0;
    virtual const TriggerConfig& trigger_config() const = 0;
    virtual bool is_repeating() = 0;
    virtual bool is_running_status();
    virtual bool is_instant() = 0;
    virtual bool have_view_data() = 0;
    virtual bool is_working() = 0;
    // Capture mode and refresh flags. Default: false (SessionDocument stubs).
    // SigSession overrides to forward to CaptureManager.
    virtual bool is_repeat_mode() override { return false; }
    virtual bool is_realtime_refresh() override { return false; }
    // Repeat-mode hold percentage (0..100). Default 0; SigSession overrides
    // to forward to CaptureManager. Used by ViewStatus to draw the repeat
    // progress bar.
    virtual int get_repeat_hold();

    // ---- Session facade operations (route View's session-level operations
    //      through DataSource so view::Signal subclasses do not hold a
    //      SigSession* directly). Only SigSession performs real work;
    //      SessionDocument / SessionSnapshot stubs inherit the no-op
    //      defaults defined out-of-line in datasource.cpp (kept out of the
    //      header so `<libsigrok.h>` does not leak into every includer).
    virtual bool is_stopped_status();
    virtual void refresh(int holdtime);
    // capture 生命周期控制（默认 no-op，仅 SigSession 真正实现）
    virtual bool stop_capture();
    virtual bool start_capture(bool instant = false, data::SessionDocument *owner = nullptr);
    // Work-mode / session-save / file-close hooks invoked by the DevMode
    // toolbar signals (Task D6.1: route View's DevMode signal forwards
    // through DataSource instead of holding a SigSession*). Default no-op;
    // only SigSession overrides with real behaviour. close_file takes the
    // raw `unsigned long long` handle (libsigrok's `ds_device_handle` is a
    // typedef of this) so datasource.h does not need to include libsigrok.h.
    virtual bool switch_work_mode(int mode);
    virtual void session_save() override;
    virtual void close_file(unsigned long long dev_handle);
    virtual bool trigd();
    virtual uint8_t trigd_ch();
    virtual bool get_data_auto_lock();
    virtual void data_auto_lock(int lock);
    virtual void auto_end();
    virtual data::SessionDocument* get_active_document();

    // ---- Decode task completion notification (Task L5: route View's
    //      DecodeTrace::on_decode_done away from holding a SigSession*).
    //      Default no-op; SigSession overrides to dispatch to listeners.
    virtual void decode_done();

    // ---- Measurements (Task C1: route DSO measurement computation through
    //      DataSource so headless mode can read real values without a View).
    //      Returns a flat list of api::MeasurementValue (one per DSO_MS_*
    //      measurement type per DSO channel). channel_index == -1 returns
    //      measurements for all enabled DSO channels; a specific channel
    //      index returns only that channel's measurements. Default returns
    //      an empty vector (SessionDocument/SessionSnapshot stubs inherit
    //      this); only SigSession overrides with real computation via
    //      core::MeasureCalculator::compute(view_data()).
    //      view_rect_height is the pixel height of the DSO trace, used by
    //      the voltage conversion formula (raw_adc * data_scale *
    //      measure_vf * vfactor * DS_CONF_DSO_VDIVS / view_rect_height).
    //      Pass 0 (or omit) to use the headless default (256 = 8 divs *
    //      32 px/div); the View layer passes its actual get_view_rect().
    //      height() so GUI-displayed voltages match the original DsoMeasure
    //      computation exactly.
    //      Non-const to match the pattern of other data accessors
    //      (get_signal_models / get_decoder_stacks / get_dso_snapshot)
    //      which read non-const SessionStateContext state.
    virtual std::vector<api::MeasurementValue> get_measurements(
        int channel_index = -1,
        int view_rect_height = 0);

    // ---- Cursors (Task C2: cursor position state moved to Core so
    //      headless MCP clients can enumerate/mutate cursors without a
    //      View). Returns the Core-layer CursorEntry list (positional
    //      index + sample_position + visible). Default returns an empty
    //      vector / -1 / false (SessionDocument/SessionSnapshot stubs
    //      inherit these); only SigSession overrides with real state via
    //      SessionStateContext::cursor_registry(). The View layer's
    //      view::Cursor objects are pure rendering objects that read
    //      their position through this interface, and write back via
    //      set_cursor_position() when the user drags. add_cursor returns
    //      the positional index of the new entry, or -1 on failure. ---
    //      Non-const mutators match the pattern of start_capture /
    //      switch_work_mode / etc. get_cursors is const (reads only).
    virtual std::vector<core::CursorEntry> get_cursors() const;
    virtual int add_cursor(uint64_t sample_position);
    virtual bool remove_cursor(int index);
    virtual bool set_cursor_position(int index, uint64_t sample_position);
    virtual void clear_cursors();

    // ---- Decoder business calls (Task D6: route View's decoder mutation
    //      calls through DataSource so the View layer does not reach into
    //      the SigSession facade for business operations). Only SigSession
    //      performs real work; SessionDocument/SessionSnapshot stubs are
    //      no-ops because decoder lifecycle lives in the live session.
    virtual bool add_decoder(srd_decoder *const dec, bool silent,
                             DecoderStatus *dstatus,
                             std::list<decode::Decoder *> &sub_decoders,
                             std::shared_ptr<DecoderStack> &out_stack,
                             SessionDocument *doc = nullptr) = 0;
    virtual void remove_decoder_by_key_handel(void *handel,
                                              SessionDocument *doc = nullptr) = 0;
    virtual void rst_decoder_by_key_handel(void *handel,
                                           SessionDocument *doc = nullptr) = 0;
    virtual void clear_all_decoder(bool bUpdateView = true) = 0;
    virtual void start_all_decode_tasks() = 0;
    virtual void update_dso_data_scale() = 0;
};

} // namespace data
} // namespace pv

#endif
