#pragma once
#include "types.h"
#include <nlohmann/json.hpp>

namespace pv::api {

using json = nlohmann::json;

class ISessionService {
public:
    virtual ~ISessionService() = default;

    // 1. Capture control
    virtual Result<void> start_capture(bool instant = false) = 0;
    virtual Result<void> stop_capture() = 0;
    virtual Result<void> switch_work_mode(WorkMode mode) = 0;
    virtual Result<void> restart_capture() = 0;
    virtual Result<void> wait_capture_complete(uint64_t timeout_ms = 300000) = 0;
    virtual Result<int> configure_and_start(
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
        uint64_t sample_count = 0) = 0;
    virtual int get_current_capture_id() const = 0;
    virtual Result<void> close_capture() = 0;

    // 2. Capture state (state machine)
    virtual CaptureState get_capture_state() const = 0;
    virtual CaptureStatus get_capture_status() const = 0;
    virtual bool can_start_capture() const = 0;
    virtual bool can_stop_capture() const = 0;

    // 3. Device info
    virtual DeviceInfo get_device_info() const = 0;
    virtual WorkMode get_work_mode() const = 0;
    virtual Result<std::vector<WorkMode>> get_supported_work_modes() const = 0;

    // 4. Channel management
    virtual std::vector<ChannelInfo> get_channels() const = 0;
    virtual Result<void> set_channel_enabled(int16_t index, bool enabled) = 0;
    virtual Result<void> set_channel_name(int16_t index, const std::string& name) = 0;
    virtual uint16_t get_channel_count(ChannelType type) const = 0;

    // 5. Sample config
    virtual SampleConfig get_sample_config() const = 0;
    virtual Result<void> set_sample_rate(uint64_t rate) = 0;
    virtual Result<void> set_sample_limit(uint64_t limit) = 0;
    virtual Result<void> set_time_base(uint64_t tb) = 0;
    virtual Result<void> set_collect_mode(CollectMode mode) = 0;
    virtual Result<void> set_repeat_interval(double seconds) = 0;
    virtual Result<uint64_t> get_actual_sample_rate() const = 0;
    virtual Result<uint64_t> get_actual_sample_count() const = 0;

    // 6. Trigger config
    virtual LogicTriggerConfig get_logic_trigger_config() const = 0;
    virtual Result<void> set_logic_trigger_config(const LogicTriggerConfig& config) = 0;
    virtual DsoTriggerConfig get_dso_trigger_config() const = 0;
    virtual Result<void> set_dso_trigger_config(const DsoTriggerConfig& config) = 0;

    // 7. Probe config
    virtual ProbeConfig get_probe_config(int16_t channel) const = 0;
    virtual Result<void> set_probe_config(int16_t channel, const ProbeConfig& config) = 0;

    // 8. Generic device config (covers all SR_CONF_* keys)
    virtual Result<std::string> get_config_string(int key) = 0;
    virtual Result<bool> set_config_string(int key, const std::string& value) = 0;
    virtual Result<bool> get_config_bool(int key) = 0;
    virtual Result<bool> set_config_bool(int key, bool value) = 0;
    virtual Result<uint64_t> get_config_uint64(int key) = 0;
    virtual Result<bool> set_config_uint64(int key, uint64_t value) = 0;
    virtual Result<int32_t> get_config_int32(int key) = 0;
    virtual Result<bool> set_config_int32(int key, int32_t value) = 0;
    virtual Result<double> get_config_double(int key) = 0;
    virtual Result<bool> set_config_double(int key, double value) = 0;
    virtual Result<uint8_t> get_config_byte(int key) = 0;
    virtual Result<bool> set_config_byte(int key, uint8_t value) = 0;
    virtual bool has_config(int key) const = 0;

    // 9. Time & trigger
    virtual TimeInfo get_time_info() const = 0;
    virtual uint64_t get_samplerate() const = 0;
    virtual uint64_t get_sample_count() const = 0;
    virtual double get_sample_time() const = 0;
    virtual uint64_t get_trigger_pos() const = 0;

    // 10. Signal list
    virtual std::vector<SignalInfo> get_signal_list() const = 0;

    // 11. Waveform data reading
    virtual Result<uint64_t> get_logic_samples(
        uint64_t start_sample, uint64_t end_sample,
        const std::vector<int16_t>& channel_indices,
        std::vector<uint8_t>& out_data) = 0;
    virtual Result<uint64_t> get_analog_samples(
        uint64_t start_sample, uint64_t end_sample,
        int16_t channel_index,
        std::vector<float>& out_data) = 0;
    virtual Result<uint64_t> get_dso_samples(
        uint64_t start_sample, uint64_t end_sample,
        int16_t channel_index,
        std::vector<float>& out_data) = 0;
    virtual Result<uint64_t> find_next_edge(
        uint64_t from_sample, int16_t channel_index, bool rising_edge) = 0;
    virtual Result<uint64_t> find_pattern(
        uint64_t from_sample, int16_t channel_index, const std::string& pattern) = 0;

    // 12. Decoder management
    virtual std::vector<DecoderDescriptor> get_available_decoders() const = 0;
    virtual std::vector<DecoderInstance> get_active_decoders() const = 0;
    virtual Result<json> get_decoder_options(
        const std::string& decoder_id) = 0;
    virtual Result<std::string> add_decoder(
        const std::string& decoder_id,
        const std::map<std::string, std::string>& options = {},
        const std::map<std::string, int16_t>& channel_map = {},
        const std::string& label = "",
        bool wait_for_completion = true,
        const std::string& stack_on_analyzer_id = "") = 0;
    virtual Result<void> remove_decoder(const std::string& instance_id) = 0;
    virtual Result<void> clear_all_decoders() = 0;

    // 13. Decoder results
    virtual Result<std::vector<DecoderAnnotation>> get_decoder_annotations(
        const std::string& instance_id,
        uint64_t start_sample = 0,
        uint64_t end_sample = UINT64_MAX,
        int max_count = 1000) = 0;

    // 14. Measurements
    virtual std::vector<MeasurementValue> get_measurements() const = 0;

    // 15. Cursors
    virtual std::vector<CursorInfo> get_cursors() const = 0;
    virtual Result<void> add_cursor(uint64_t sample_pos) = 0;
    virtual Result<void> remove_cursor(int index) = 0;
    virtual Result<void> clear_cursors() = 0;

    // 16. Signal processing
    virtual Result<void> set_glitch_filter(const GlitchFilterConfig& config) = 0;
    virtual Result<void> clear_glitch_filter() = 0;
    virtual GlitchFilterConfig get_glitch_filter_config() const = 0;
    virtual Result<void> set_signal_invert(const SignalInvertConfig& config) = 0;
    virtual Result<void> clear_signal_invert() = 0;
    virtual SignalInvertConfig get_signal_invert_config() const = 0;

    // 17. Disk cache
    virtual DiskCacheInfo get_disk_cache_info() const = 0;

  // 18. File operations
    virtual Result<void> load_file(const std::string& path) = 0;
    virtual Result<void> save_file(const std::string& path) = 0;
    virtual Result<void> export_data(const ExportConfig& config) = 0;
    virtual Result<void> export_binary(const ExportConfig& config) = 0;
    virtual Result<void> export_decoder_table(
        const std::string& filepath,
        const std::vector<AnalyzerExportConfig>& analyzers = {},
        bool iso8601_timestamp = false) = 0;

    // 18b. MCP-specific file operations
    virtual Result<void> export_raw_data_csv(
        const std::string& directory,
        const std::vector<int32_t>& digital_channels,
        const std::vector<int32_t>& analog_channels,
        int analog_downsample_ratio = 1,
        bool iso8601_timestamp = false) = 0;
    virtual Result<void> export_raw_data_binary(
        const std::string& directory,
        const std::vector<int32_t>& digital_channels,
        const std::vector<int32_t>& analog_channels,
        int analog_downsample_ratio = 1) = 0;
    virtual Result<void> export_data_table_csv(
        const std::string& filepath,
        const std::string& analyzer_id,
        int radix_type = 0,
        bool iso8601_timestamp = false) = 0;

    // 19. View control
    virtual Result<void> show_region(uint64_t start_sample, uint64_t end_sample) = 0;
    virtual Result<void> zoom_fit() = 0;
    virtual Result<void> zoom_in() = 0;
    virtual Result<void> zoom_out() = 0;

    // 20. Spectrum/Lissajous/Math
    virtual Result<void> enable_spectrum(int16_t channel_index, bool enable) = 0;
    virtual Result<void> enable_lissajous(int16_t x_channel, int16_t y_channel, double percent) = 0;
    virtual Result<void> disable_lissajous() = 0;
    virtual Result<void> enable_math(int16_t ch1, int16_t ch2, int math_type) = 0;

    // 21. Event subscription
    virtual void add_event_listener(IServiceEventListener* listener) = 0;
    virtual void remove_event_listener(IServiceEventListener* listener) = 0;
};

} // namespace pv::api
