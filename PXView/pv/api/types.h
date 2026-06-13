#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pv {
namespace api {

// ---- Enums ----

enum class WorkMode : int8_t {
    Logic   = 0,
    Analog  = 1,
    Dso     = 2,
    Unknown = -1
};

enum class CaptureState : int8_t {
    Empty    = 0,
    Starting = 1,
    Recording = 2,
    Stopping = 3,
    Stopped  = 4,
    Paused   = 5,
    Error    = 6
};

enum class CollectMode : int8_t {
    Single = 0,
    Repeat = 1,
    Loop   = 2
};

enum class ChannelType : int8_t {
    Logic  = 0,
    Analog = 1,
    Dso    = 2
};

enum class TriggerSlope : int8_t {
    Rising  = 0,
    Falling = 1
};

enum class TriggerSource : int8_t {
    Auto      = 0,
    Channel0  = 1,
    Channel1  = 2,
    Ch0AndCh1 = 3,
    Ch0OrCh1  = 4
};

enum class Coupling : int8_t {
    AC = 0,
    DC = 1
};

enum class GlitchFilterMode : int8_t {
    Both = 0,
    High = 1,
    Low  = 2
};

enum class ErrorCode : int8_t {
    Ok                 = 0,
    InvalidRequest     = 1,
    MissingDevice      = 2,
    DeviceError        = 3,
    DeviceDisconnected = 4,
    CaptureInProgress  = 5,
    CaptureNotStarted  = 6,
    NoData             = 7,
    DecoderNotFound    = 8,
    DecoderError       = 9,
    ChannelNotFound    = 10,
    ConfigNotSupported = 11,
    ConfigInvalid      = 12,
    ExportFailed       = 13,
    SaveFailed         = 14,
    LoadFailed         = 15,
    OutOfMemory        = 16,
    InternalError      = 17,
    SessionBusy        = 18,
    InvalidState       = 19
};

enum class ServiceEvent : int32_t {
    CaptureStateChanged    = 100,
    DataUpdated            = 101,
    TriggerReceived        = 102,
    FrameBegan             = 103,
    FrameEnded             = 104,
    CaptureProgress        = 105,

    DeviceListUpdated      = 200,
    DeviceModeChanged      = 201,
    DeviceConfigChanged    = 202,
    DeviceDetached         = 203,
    NewUsbDevice           = 204,

    GlitchFilterStarted    = 300,
    GlitchFilterProgress   = 301,
    GlitchFilterCompleted  = 302,
    GlitchFilterCleared    = 303,
    SignalInvertStarted    = 304,
    SignalInvertCompleted  = 305,
    SignalInvertCleared    = 306,

    DecodeDone             = 400,
    DecoderAdded           = 401,
    DecoderRemoved         = 402,
    DecodeProgress         = 403,

    SaveComplete           = 500,
    LoadComplete           = 501,
    ExportComplete         = 502,

    SignalsChanged         = 600,

    ErrorOccurred          = 900
};

// ---- Error ----

struct Error {
    ErrorCode   code;
    std::string message;

    explicit Error(ErrorCode c, std::string msg = "")
        : code(c), message(std::move(msg)) {}
};

// ---- Result<T> ----

template<typename T>
class Result {
public:
    static Result Success(T val) {
        Result r;
        r.ok_     = true;
        r.value_  = std::move(val);
        return r;
    }

    static Result Fail(ErrorCode code, std::string msg = "") {
        Result r;
        r.ok_    = false;
        r.error_ = Error(code, std::move(msg));
        return r;
    }

    static Result Fail(Error err) {
        Result r;
        r.ok_    = false;
        r.error_ = std::move(err);
        return r;
    }

    explicit operator bool() const noexcept { return ok_; }

    bool ok() const noexcept { return ok_; }

    const T& value() const { return value_; }
    T&       value()       { return value_; }

    const Error& error() const { return error_; }
    Error&       error()       { return error_; }

private:
    Result() = default;
    bool   ok_ = false;
    T      value_;
    Error  error_{ErrorCode::Ok};
};

template<>
class Result<void> {
public:
    static Result Success() {
        Result r;
        r.ok_ = true;
        return r;
    }

    static Result Fail(ErrorCode code, std::string msg = "") {
        Result r;
        r.ok_    = false;
        r.error_ = Error(code, std::move(msg));
        return r;
    }

    static Result Fail(Error err) {
        Result r;
        r.ok_    = false;
        r.error_ = std::move(err);
        return r;
    }

    explicit operator bool() const noexcept { return ok_; }

    bool ok() const noexcept { return ok_; }

    const Error& error() const { return error_; }
    Error&       error()       { return error_; }

private:
    Result() = default;
    bool  ok_ = false;
    Error error_{ErrorCode::Ok};
};

// ---- Data Structs ----

struct DeviceInfo {
    std::string id                = "";
    std::string driver_name       = "";
    std::string display_name      = "";
    std::string path              = "";
    bool        is_hardware       = false;
    bool        is_demo           = false;
    bool        is_file           = false;
    bool        is_virtual        = false;
    bool        is_hardware_logic = false;
    bool        is_hardware_dso   = false;
    bool        is_dsl_device     = false;
    bool        is_compat_device  = false;
    int         usb_speed         = 0;
};

struct ChannelInfo {
    int32_t     index          = 0;
    std::string name           = "";
    ChannelType type           = ChannelType::Logic;
    bool        enabled        = false;
    bool        enabled_default = false;
};

struct SampleConfig {
    uint64_t    sample_rate         = 0;
    uint64_t    sample_limit        = 0;
    double      time_base           = 0.0;
    CollectMode collect_mode        = CollectMode::Single;
    bool        stream_mode         = false;
    bool        rle_enabled         = false;
    double      repeat_interval     = 0.0;
    double      repeat_hold_percent = 0.0;
};

struct LogicTriggerConfig {
    int32_t     stage_count = 0;
    std::string config_json = "";
};

struct DsoTriggerConfig {
    TriggerSource source    = TriggerSource::Auto;
    TriggerSlope  slope     = TriggerSlope::Rising;
    double        horiz_pos = 0.0;
    double        holdoff   = 0.0;
    double        margin    = 0.0;
    int32_t       channel   = 0;
};

struct ProbeConfig {
    double   vdiv        = 0.0;
    Coupling coupling    = Coupling::DC;
    double   vfactor     = 1.0;
    bool     map_default = true;
};

struct SignalInfo {
    int32_t     index   = 0;
    std::string name    = "";
    ChannelType type    = ChannelType::Logic;
    bool        enabled = false;
    std::string color   = "";
    ProbeConfig probe   = {};
};

struct CaptureStatus {
    CaptureState state                  = CaptureState::Empty;
    bool         is_instant             = false;
    bool         is_saving              = false;
    bool         have_view_data         = false;
    bool         have_hardware_data     = false;
    bool         have_decoded_result    = false;
    bool         is_copy_in_progress    = false;
    bool         is_glitch_filter_active = false;
    bool         is_signal_invert_active = false;
    double       progress               = 0.0;
    bool         triggered              = false;
};

struct TimeInfo {
    int64_t session_start_ms   = 0;
    int64_t trigger_pos        = 0;
    int64_t trigger_time_ms    = 0;
    bool    is_triggered       = false;
    double  session_duration_sec = 0.0;
    double  view_time_sec      = 0.0;
    double  sample_time_sec    = 0.0;
};

struct DiskCacheInfo {
    bool   enabled          = false;
    double write_speed_mbps = 0.0;
    int32_t write_queue_depth = 0;
    bool   is_disk_full     = false;
};

struct DecoderChannelInfo {
    std::string id          = "";   // Channel ID (e.g. "data", "clk", "sda")
    std::string name        = "";   // Display name (e.g. "Data", "CLK", "SDA")
    std::string desc        = "";   // Description (e.g. "Data line")
    int32_t     order       = 0;
    bool        is_optional = false;
};

struct DecoderDescriptor {
    std::string                        id                 = "";
    std::string                        name               = "";
    std::string                        long_name          = "";
    int32_t                            channels           = 0;
    int32_t                            optional_channels  = 0;
    std::vector<DecoderChannelInfo>    channel_info;
};

struct DecoderInstance {
    std::string instance_id  = "";
    std::string decoder_id   = "";
    std::string display_name = "";
    int32_t     row_index    = 0;
    bool        is_running   = false;
    double      progress     = 0.0;
};

struct DecoderAnnotation {
    uint64_t                   start_sample = 0;
    uint64_t                   end_sample   = 0;
    int32_t                    ann_class    = 0;
    std::vector<std::string>   texts;
};

struct MeasurementValue {
    int32_t     type  = 0;
    double      value = 0.0;
    std::string unit  = "";
    bool        valid = false;
};

struct CursorInfo {
    int32_t index       = 0;
    int64_t sample_pos  = 0;
    double  time_sec    = 0.0;
};

struct GlitchFilterConfig {
    std::vector<int32_t>          channels;
    std::vector<int32_t>          thresholds;
    std::vector<GlitchFilterMode> modes;
};

struct SignalInvertConfig {
    std::vector<int32_t> channels;
    std::vector<bool>    invert_states;
};

struct ExportConfig {
    std::string          output_path   = "";
    std::vector<int32_t> channels;
    uint64_t             start_sample  = 0;
    uint64_t             end_sample    = 0;
    bool                 is_logic      = true;
    bool                 include_headers = true;
    uint64_t             analog_downsample_ratio = 1;
    bool                 iso8601_timestamp = false;
};

struct AnalyzerExportConfig {
    std::string analyzer_id;
    int radix_type = 4;  // 1=Binary, 2=Decimal, 3=Hex, 4=Ascii
};

struct ServiceEventData {
    ServiceEvent                          event = ServiceEvent::ErrorOccurred;
    std::map<std::string, std::string>    params;
};

// ---- Interface ----

class IServiceEventListener {
public:
    virtual ~IServiceEventListener() = default;
    virtual void on_service_event(const ServiceEventData& data) = 0;
};

} // namespace api
} // namespace pv
