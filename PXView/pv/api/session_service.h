/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
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

#pragma once

#include "pv/api/isession_service.h"
#include <QtGlobal>
#include <QString>
#include <cstddef>
#include <cstdint>
#include "pv/interface/icallbacks.h"
#include "pv/interface/events.h"

#include <condition_variable>
#include <mutex>
#include <vector>

namespace pv {

class SigSession;

namespace data {
class SessionDocument;
} // namespace data

} // namespace pv

class DeviceAgent;

namespace pv {
namespace api {

class SessionService : public ISessionService {
public:
explicit SessionService(SigSession *session, DeviceAgent *device);
    ~SessionService() override;

    // Disable copy
    SessionService(const SessionService &) = delete;
    SessionService &operator=(const SessionService &) = delete;

    // ---- MCP document injection ----
    // Injects the dedicated document (by owning index in DocumentRegistry)
    // used as the stable target container for MCP operations. Decouples MCP
    // from the UI's _active_document cursor so the same target is available
    // in both headless and GUI modes.
    // phase 2: the document is owned by DocumentRegistry; SessionService holds
    // only the index. set_api_document() releases the previously-injected
    // index (if any) before storing the new one.
    void set_api_document(size_t doc_index);

    // ---- ISessionService: 1. Capture control ----
    Result<void> start_capture(bool instant = false) override;
    Result<void> stop_capture() override;
    Result<void> switch_work_mode(WorkMode mode) override;
    Result<void> restart_capture() override;
    Result<void> wait_capture_complete(uint64_t timeout_ms = 300000) override;
    Result<int> configure_and_start(
        const std::vector<int16_t>& digital_channels = {},
        const std::vector<int16_t>& analog_channels = {},
        uint64_t digital_sample_rate = 0,
        uint64_t analog_sample_rate = 0,
        double digital_threshold_volts = 0.0,
        const std::vector<std::pair<int16_t, double>>& glitch_filters = {},
        const std::string& capture_mode = "manual",
        double duration_seconds = 0.0,
        bool instant = false,
        int trigger_channel_index = -1,
        const std::string& trigger_type = "",
        double after_trigger_seconds = 0.0,
        double min_pulse_width_seconds = 0.0,
        double max_pulse_width_seconds = 0.0,
        const std::vector<std::pair<int16_t, std::string>>& linked_channels = {},
        const std::string& channel_mode = "",
        bool rle_enabled = false,
        double stream_buffer_size_gb = 0.0,
        double stream_mem_buffer_size_gb = 0.0,
        bool disk_cache_enabled = false,
        const std::string& disk_cache_path = "",
        const std::string& threshold_preset = "",
        const std::string& operation_mode = "",
        const std::string& buffer_options = "",
        const std::string& digital_filter = "",
        const std::string& pattern = "",
        int capture_ratio = -1,
        double repeat_interval_seconds = 0.0,
        uint64_t sample_count = 0) override;
    int get_current_capture_id() const override;
    Result<void> close_capture() override;
    Result<void> wait_for_decode_complete(uint64_t timeout_ms = 300000) override;

    // ---- ISessionService: 2. Capture state ----
    CaptureState get_capture_state() const override;
    CaptureStatus get_capture_status() const override;
    bool can_start_capture() const override;
    bool can_stop_capture() const override;

    // ---- ISessionService: 3. Device info ----
    DeviceInfo get_device_info() const override;
    WorkMode get_work_mode() const override;
    Result<std::vector<WorkMode>> get_supported_work_modes() const override;
    Result<std::vector<DeviceInfo>> refresh_device_list() override;

    // ---- ISessionService: 4. Channel management ----
    std::vector<ChannelInfo> get_channels() const override;
    Result<void> set_channel_enabled(int16_t index, bool enabled) override;
    Result<void> set_channel_name(int16_t index, const std::string &name) override;
    uint16_t get_channel_count(ChannelType type) const override;

    // ---- ISessionService: 5. Sample config ----
    SampleConfig get_sample_config() const override;
    Result<void> set_sample_rate(uint64_t rate) override;
    Result<void> set_sample_limit(uint64_t limit) override;
    Result<void> set_time_base(uint64_t tb) override;
    Result<void> set_collect_mode(CollectMode mode) override;
    Result<void> set_repeat_interval(double seconds) override;
    Result<uint64_t> get_actual_sample_rate() const override;
    Result<uint64_t> get_actual_sample_count() const override;
    Result<void> set_save_range(uint64_t start_sample,
                                uint64_t end_sample) override;

    // ---- ISessionService: 6. Trigger config ----
    LogicTriggerConfig get_logic_trigger_config() const override;
    Result<void> set_logic_trigger_config(const LogicTriggerConfig &config) override;
    DsoTriggerConfig get_dso_trigger_config() const override;
    Result<void> set_dso_trigger_config(const DsoTriggerConfig &config) override;

    // ---- ISessionService: 7. Probe config ----
    ProbeConfig get_probe_config(int16_t channel) const override;
    Result<void> set_probe_config(int16_t channel, const ProbeConfig &config) override;

    // ---- ISessionService: 8. Generic device config ----
    Result<std::string> get_config_string(int key) override;
    Result<bool> set_config_string(int key, const std::string &value) override;
    Result<bool> get_config_bool(int key) override;
    Result<bool> set_config_bool(int key, bool value) override;
    Result<uint64_t> get_config_uint64(int key) override;
    Result<bool> set_config_uint64(int key, uint64_t value) override;
    Result<int32_t> get_config_int32(int key) override;
    Result<bool> set_config_int32(int key, int32_t value) override;
    Result<double> get_config_double(int key) override;
    Result<bool> set_config_double(int key, double value) override;
    Result<uint8_t> get_config_byte(int key) override;
    Result<bool> set_config_byte(int key, uint8_t value) override;
    bool has_config(int key) const override;

    // ---- ISessionService: 9. Time & trigger ----
    TimeInfo get_time_info() const override;
    uint64_t get_samplerate() const override;
    uint64_t get_sample_count() const override;
    double get_sample_time() const override;
    uint64_t get_trigger_pos() const override;

    // ---- ISessionService: 10. Signal list ----
    std::vector<SignalInfo> get_signal_list() const override;

    // ---- ISessionService: 11. Waveform data reading ----
    Result<uint64_t> get_logic_samples(
        uint64_t start_sample, uint64_t end_sample,
        const std::vector<int16_t> &channel_indices,
        std::vector<uint8_t> &out_data) override;
    Result<uint64_t> get_analog_samples(
        uint64_t start_sample, uint64_t end_sample,
        int16_t channel_index,
        std::vector<float> &out_data) override;
    Result<uint64_t> get_dso_samples(
        uint64_t start_sample, uint64_t end_sample,
        int16_t channel_index,
        std::vector<float> &out_data) override;
    Result<uint64_t> find_next_edge(
        uint64_t from_sample, int16_t channel_index, bool rising_edge) override;
    Result<uint64_t> find_pattern(
        uint64_t from_sample, int16_t channel_index, const std::string &pattern) override;

    // ---- ISessionService: 12. Decoder management ----
    std::vector<DecoderDescriptor> get_available_decoders() const override;
    std::vector<DecoderInstance> get_active_decoders() const override;
    Result<json> get_decoder_options(const std::string& decoder_id) override;
    Result<std::string> add_decoder(
        const std::string &decoder_id,
        const std::map<std::string, std::string> &options = {},
        const std::map<std::string, int16_t> &channel_map = {},
        const std::string &label = "",
        bool wait_for_completion = true,
        const std::string &stack_on_analyzer_id = "") override;
    Result<void> remove_decoder(const std::string &instance_id) override;
    Result<void> clear_all_decoders() override;
    Result<void> reconfigure_decoder(
        const std::string &instance_id,
        const std::map<std::string, std::string> &options,
        const std::map<std::string, int> &channel_map) override;
    Result<std::vector<DecoderClassInfo>> get_decoder_class_names(
        const std::string &decoder_id) override;

    // ---- ISessionService: 13. Decoder results ----
    Result<std::vector<DecoderAnnotation>> get_decoder_annotations(
        const std::string &instance_id,
        uint64_t start_sample = 0,
        uint64_t end_sample = UINT64_MAX,
        int max_count = 1000) override;
    Result<std::vector<uint8_t>> get_decoder_binary_output(
        const std::string &instance_id, int output_id) override;

    // ---- ISessionService: 14. Measurements ----
    std::vector<MeasurementValue> get_measurements() const override;

    // ---- ISessionService: 15. Cursors ----
    std::vector<CursorInfo> get_cursors() const override;
    Result<void> add_cursor(uint64_t sample_pos) override;
    Result<void> remove_cursor(int index) override;
    Result<void> clear_cursors() override;

    // ---- ISessionService: 16. Signal processing ----
    Result<void> set_glitch_filter(const GlitchFilterConfig &config) override;
    Result<void> clear_glitch_filter() override;
    GlitchFilterConfig get_glitch_filter_config() const override;
    Result<void> set_signal_invert(const SignalInvertConfig &config) override;
    Result<void> clear_signal_invert() override;
    SignalInvertConfig get_signal_invert_config() const override;

    // ---- ISessionService: 17. Disk cache ----
    DiskCacheInfo get_disk_cache_info() const override;

    // ---- ISessionService: 18. File operations ----
    Result<void> load_file(const std::string &path) override;
    Result<void> save_file(const std::string &path) override;
    Result<void> export_data(const ExportConfig &config) override;
    Result<void> export_binary(const ExportConfig &config) override;
    Result<void> export_decoder_table(
        const std::string &filepath,
        const std::vector<AnalyzerExportConfig> &analyzers = {},
        bool iso8601_timestamp = false) override;

    // ---- ISessionService: 18b. MCP-specific file operations ----
    Result<void> export_raw_data_csv(
        const std::string &directory,
        const std::vector<int32_t> &digital_channels,
        const std::vector<int32_t> &analog_channels,
        int analog_downsample_ratio = 1,
        bool iso8601_timestamp = false) override;
    Result<void> export_raw_data_binary(
        const std::string &directory,
        const std::vector<int32_t> &digital_channels,
        const std::vector<int32_t> &analog_channels,
        int analog_downsample_ratio = 1) override;
    Result<void> export_data_table_csv(
        const std::string &filepath,
        const std::string &analyzer_id,
        int radix_type = 0,
        bool iso8601_timestamp = false) override;

    // Unified raw-data export. `format` maps to an sr_output module id
    // (csv/binary/vcd/hex/bits); the file suffix is derived from it so the
    // existing StoreSession export path picks the right module.
    Result<void> export_raw_data(
        const std::string &format,
        const std::string &directory,
        const std::vector<int32_t> &digital_channels,
        const std::vector<int32_t> &analog_channels,
        int analog_downsample_ratio = 1,
        bool iso8601_timestamp = false) override;

    // ---- ISessionService: 19. View control ----
    Result<void> show_region(uint64_t start_sample, uint64_t end_sample) override;
    Result<void> zoom_fit() override;
    Result<void> zoom_in() override;
    Result<void> zoom_out() override;

    // ---- ISessionService: 20. Spectrum/Lissajous/Math ----
    Result<void> enable_spectrum(int16_t channel_index, bool enable) override;
    Result<void> enable_lissajous(int16_t x_channel, int16_t y_channel,
                                  double percent) override;
    Result<void> disable_lissajous() override;
    Result<void> enable_math(int16_t ch1, int16_t ch2, int math_type) override;
    Result<MathResult> get_math_results() override;
    Result<SpectrumResult> get_spectrum_results() override;
    Result<LissajousResult> get_lissajous_results() override;

    // ---- ISessionService: 21. Event subscription ----
    void add_event_listener(IServiceEventListener *listener) override;
    void remove_event_listener(IServiceEventListener *listener) override;

    // ---- ISessionService: 22. Error state (Batch B) ----
    Result<ErrorState> get_error_state() override;
    Result<void> clear_error_state() override;

    // ---- ISessionCallback ----
// Spec v2 Task 7: was ISessionCallback overrides — now regular methods
void session_error();
void session_save();
void data_updated();
void update_capture();
void cur_snap_samplerate_changed();
void signals_changed();
void receive_trigger(quint64 trigger_pos);
void frame_ended();
void frame_began();
void show_region(uint64_t start, uint64_t end, bool keep);
void show_wait_trigger();
void repeat_hold(int percent);
void decode_done();
void receive_data_len(quint64 len);
void receive_header();
void delay_prop_msg(QString strMsg);

    // ---- IEventListener ----
    // Full migration: all notification events are wired to on_event handlers
    // that re-broadcast as ServiceEvent for MCP/WS clients. The legacy
    // int-message dispatch path is removed.
private:
void broadcast_event(ServiceEvent event,
                         const std::map<std::string, std::string> &params = {}) const;
    ChannelType sr_channel_type_to_api(int sr_type) const;

    // ---- configure_and_start helper methods (split from 470-line function) ----
    // Step 0: Ensure device is in LOGIC mode when digital channels are requested.
    void ensure_logic_mode_for_digital(const std::vector<int16_t>& digital_channels);
    // Step 1: Enable/disable channels based on caller specification.
    void configure_capture_channels(const std::vector<int16_t>& digital_channels,
                                    const std::vector<int16_t>& analog_channels);
    // Step 2: Set up logic trigger on SignalModel (drives UI rendering).
    void apply_trigger_to_signal_models(
        int trigger_channel_index,
        const std::string& trigger_type,
        const std::vector<std::pair<int16_t, std::string>>& linked_channels);
    // Steps 5a-5g: Set device-level options (mode, threshold, filters, etc.).
    void configure_device_options(
        const std::string& channel_mode,
        bool rle_enabled,
        double stream_buffer_size_gb,
        double stream_mem_buffer_size_gb,
        bool disk_cache_enabled,
        const std::string& disk_cache_path,
        const std::string& threshold_preset,
        const std::string& operation_mode,
        const std::string& buffer_options,
        const std::string& digital_filter,
        const std::string& pattern);
    // Steps 6-7: Set collect mode, repeat interval, capture ratio, duration.
    void configure_capture_timing(
        const std::string& capture_mode,
        double repeat_interval_seconds,
        int capture_ratio,
        double duration_seconds,
        uint64_t sample_count);
// Returns true when running inside the GUI (QApplication), false when
// running headless (QCoreApplication only).
    static bool is_gui_mode();
    // phase 2: resolve the MCP-dedicated document weak pointer from the owning
    // index in DocumentRegistry. Returns nullptr if no document is injected
    // or the slot has been released.
    pv::data::SessionDocument *api_document() const;

private:
    SigSession *_session;
    DeviceAgent *_device;
    std::vector<IServiceEventListener *> _listeners;
    mutable std::mutex _listeners_mutex;
int _capture_id;

// RAII event subscriptions
std::vector<core::Subscription> _event_subscriptions;

// P0-2: Global state version counter for versioned notifications
    mutable uint64_t _state_version_counter = 0;

    // MCP-dedicated document. phase 2: ownership is held by DocumentRegistry;
    // SessionService stores only the owning index (SIZE_MAX == none). Created
    // via DocumentRegistry::create_api_document() (called from AppService) and
    // released in the destructor via release_document().
    size_t _api_doc_index = SIZE_MAX;
};

} // namespace api
} // namespace pv
