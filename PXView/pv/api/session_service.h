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

#include "isession_service.h"
#include <QtGlobal>
#include <QString>
#include "../interface/icallbacks.h"

#include <mutex>
#include <vector>

namespace pv {

class SigSession;
namespace view {
class View;
}

} // namespace pv

class DeviceAgent;

namespace pv {
namespace api {

class SessionService : public ISessionService,
                       public ISessionCallback,
                       public IMessageListener {
public:
    explicit SessionService(SigSession *session, DeviceAgent *device);
    ~SessionService() override;

    // Disable copy
    SessionService(const SessionService &) = delete;
    SessionService &operator=(const SessionService &) = delete;

    // View binding for cursor/measure/zoom operations
    void set_view(view::View *view);

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
        int capture_ratio = -1,
        double repeat_interval_seconds = 0.0,
        uint64_t sample_count = 0) override;
    int get_current_capture_id() const override;
    Result<void> close_capture() override;

    // ---- ISessionService: 2. Capture state ----
    CaptureState get_capture_state() const override;
    CaptureStatus get_capture_status() const override;
    bool can_start_capture() const override;
    bool can_stop_capture() const override;

    // ---- ISessionService: 3. Device info ----
    DeviceInfo get_device_info() const override;
    WorkMode get_work_mode() const override;
    Result<std::vector<WorkMode>> get_supported_work_modes() const override;

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

    // ---- ISessionService: 13. Decoder results ----
    Result<std::vector<DecoderAnnotation>> get_decoder_annotations(
        const std::string &instance_id,
        uint64_t start_sample = 0,
        uint64_t end_sample = UINT64_MAX,
        int max_count = 1000) override;

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

    // ---- ISessionService: 21. Event subscription ----
    void add_event_listener(IServiceEventListener *listener) override;
    void remove_event_listener(IServiceEventListener *listener) override;

    // ---- ISessionCallback ----
    void session_error() override;
    void session_save() override;
    void data_updated() override;
    void update_capture() override;
    void cur_snap_samplerate_changed() override;
    void signals_changed() override;
    void receive_trigger(quint64 trigger_pos) override;
    void frame_ended() override;
    void frame_began() override;
    void show_region(uint64_t start, uint64_t end, bool keep) override;
    void show_wait_trigger() override;
    void repeat_hold(int percent) override;
    void decode_done() override;
    void receive_data_len(quint64 len) override;
    void receive_header() override;
    void trigger_message(int msg) override;
    void delay_prop_msg(QString strMsg) override;

    // ---- IMessageListener ----
    void OnMessage(int msg) override;

private:
    void broadcast_event(ServiceEvent event,
                         const std::map<std::string, std::string> &params = {});
    ChannelType sr_channel_type_to_api(int sr_type) const;

private:
    SigSession *_session;
    DeviceAgent *_device;
    view::View *_view;
    std::vector<IServiceEventListener *> _listeners;
    mutable std::mutex _listeners_mutex;
    int _capture_id;
    bool _wait_capture_stop_flag;
};

} // namespace api
} // namespace pv
