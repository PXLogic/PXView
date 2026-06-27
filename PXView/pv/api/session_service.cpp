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

#include "session_service.h"

#include "../sigsession.h"
#include "../deviceagent.h"
#include "../view/view.h"
#include "../view/signal.h"
#include "../view/logicsignal.h"
#include "../view/analogsignal.h"
#include "../view/dsosignal.h"
#include "../view/decodetrace.h"
#include "../view/spectrumtrace.h"
#include "../view/lissajoustrace.h"
#include "../view/mathtrace.h"
#include "../view/cursor.h"
#include "../data/logicsnapshot.h"
#include "../data/analogsnapshot.h"
#include "../data/dsosnapshot.h"
#include "../data/decoderstack.h"
#include "../data/decode/decoder.h"
#include "../data/decode/annotation.h"
#include "../data/decode/row.h"
#include "../storesession.h"
#include "../log.h"

#include <libsigrok.h>
#include <libsigrokdecode/libsigrokdecode.h>

#include <QApplication>
#include <QDebug>
#include <QColor>
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QString>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <condition_variable>

#ifdef WIN32
#include <windows.h>
#endif

namespace pv {
namespace api {

// Convert a potentially GBK-encoded C string to UTF-8.
// On Windows, C decoder DLLs may store Chinese text in the system default
// codepage (GBK on Chinese systems). nlohmann::json requires UTF-8.
static std::string ensure_utf8(const char *str) {
    if (!str || !*str)
        return "";

    // First check if the string is already valid UTF-8.
    // MinGW compiles C decoders with -fexec-charset=UTF-8 by default,
    // so most strings are already UTF-8.
    bool is_valid_utf8 = true;
    for (const unsigned char *p = (const unsigned char *)str; *p; ) {
        if (*p <= 0x7f) {
            p++;
        } else if ((*p & 0xe0) == 0xc0) {
            if ((p[1] & 0xc0) != 0x80) { is_valid_utf8 = false; break; }
            p += 2;
        } else if ((*p & 0xf0) == 0xe0) {
            if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) { is_valid_utf8 = false; break; }
            p += 3;
        } else if ((*p & 0xf8) == 0xf0) {
            if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80) { is_valid_utf8 = false; break; }
            p += 4;
        } else {
            is_valid_utf8 = false;
            break;
        }
    }
    if (is_valid_utf8)
        return str;

#ifdef WIN32
    // Not valid UTF-8 — assume it's GBK (system ANSI codepage on Chinese Windows)
    int wlen = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0);
    if (wlen <= 0)
        return str;  // fallback
    std::wstring wstr(wlen - 1, 0);
    MultiByteToWideChar(CP_ACP, 0, str, -1, &wstr[0], wlen);

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0)
        return str;  // fallback
    std::string utf8(utf8_len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8[0], utf8_len, nullptr, nullptr);
    return utf8;
#else
    return str;
#endif
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SessionService::SessionService(SigSession *session, DeviceAgent *device)
    : _session(session), _device(device), _view(nullptr),
      _capture_id(0), _wait_capture_stop_flag(false) {
    if (_session) {
        _session->add_callback(this);
        _session->add_msg_listener(this);
    }
}

SessionService::~SessionService() {
    if (_session) {
        _session->remove_callback(this);
    }
}

void SessionService::set_view(view::View *view) {
    _view = view;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SessionService::broadcast_event(
    ServiceEvent event, const std::map<std::string, std::string> &params) {
    std::lock_guard<std::mutex> lock(_listeners_mutex);
    ServiceEventData data;
    data.event = event;
    data.params = params;
    for (auto *listener : _listeners) {
        listener->on_service_event(data);
    }
}

ChannelType SessionService::sr_channel_type_to_api(int sr_type) const {
    switch (sr_type) {
    case SR_CHANNEL_LOGIC:
        return ChannelType::Logic;
    case SR_CHANNEL_ANALOG:
        return ChannelType::Analog;
    case SR_CHANNEL_DSO:
        return ChannelType::Dso;
    default:
        return ChannelType::Logic;
    }
}

// ===========================================================================
// 1. Capture control
// ===========================================================================

Result<void> SessionService::start_capture(bool instant) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is null");
    if (_session->is_working())
        return Result<void>::Fail(ErrorCode::CaptureInProgress,
                                 "Capture already in progress");

    bool ok = _session->start_capture(instant);
    if (!ok)
        return Result<void>::Fail(ErrorCode::DeviceError,
                                 "Failed to start capture");
    return Result<void>::Success();
}

Result<void> SessionService::stop_capture() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is null");
    if (!_session->is_working())
        return Result<void>::Fail(ErrorCode::CaptureNotStarted,
                                 "No capture in progress");

    bool ok = _session->stop_capture();
    if (!ok)
        return Result<void>::Fail(ErrorCode::DeviceError,
                                 "Failed to stop capture");

    // Wait for capture to actually stop (max 3 seconds)
    for (int i = 0; i < 30; i++) {
        if (!_session->is_working() && !_session->is_running_status())
            break;
        QThread::msleep(100);
    }

    return Result<void>::Success();
}

Result<void> SessionService::switch_work_mode(WorkMode mode) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is null");

    int sr_mode = 0;
    switch (mode) {
    case WorkMode::Logic:
        sr_mode = LOGIC;
        break;
    case WorkMode::Analog:
        sr_mode = ANALOG;
        break;
    case WorkMode::Dso:
        sr_mode = DSO;
        break;
    default:
        return Result<void>::Fail(ErrorCode::InvalidRequest,
                                 "Unknown work mode");
    }

    bool ok = _session->switch_work_mode(sr_mode);
    if (!ok)
        return Result<void>::Fail(ErrorCode::DeviceError,
                                 "Failed to switch work mode");
    return Result<void>::Success();
}

Result<void> SessionService::restart_capture() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is null");

    bool ok = _session->re_start();
    if (!ok)
        return Result<void>::Fail(ErrorCode::DeviceError,
                                 "Failed to restart capture");
    return Result<void>::Success();
}

Result<void> SessionService::wait_capture_complete(uint64_t timeout_ms) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is null");

    CaptureState state = get_capture_state();

    // Already stopped — return success immediately
    if (state == CaptureState::Stopped)
        return Result<void>::Success();

    // No capture started — error
    if (state == CaptureState::Empty)
        return Result<void>::Fail(ErrorCode::CaptureNotStarted,
                                 "No capture has been started");

    // If not recording/starting, nothing to wait for
    if (state != CaptureState::Recording && state != CaptureState::Starting)
        return Result<void>::Fail(ErrorCode::InvalidState,
                                 "Unexpected capture state");

    // Block using QEventLoop until capture stops or timeout
    QEventLoop loop;
    QTimer timeout_timer;
    timeout_timer.setSingleShot(true);

    _wait_capture_stop_flag = false;

    // Connect timeout
    QObject::connect(&timeout_timer, &QTimer::timeout, &loop, [&]() {
        _wait_capture_stop_flag = false;
        loop.quit();
    });

    // Check capture state periodically via a repeating timer.
    // The ISessionCallback / IMessageListener callbacks arrive on the
    // main thread, so a QEventLoop here will process them. We use a
    // short-interval check timer to poll the session state.
    QTimer check_timer;
    QObject::connect(&check_timer, &QTimer::timeout, &loop, [&]() {
        if (!_session->is_working() && !_session->is_running_status()) {
            _wait_capture_stop_flag = true;
            loop.quit();
        }
    });

    timeout_timer.start(static_cast<int>(timeout_ms));
    check_timer.start(100); // check every 100 ms

    loop.exec();

    check_timer.stop();
    timeout_timer.stop();

    if (_wait_capture_stop_flag)
        return Result<void>::Success();

    return Result<void>::Fail(ErrorCode::SessionBusy,
                             "Capture wait timed out");
}

Result<int> SessionService::configure_and_start(
    const std::vector<int16_t>& digital_channels,
    const std::vector<int16_t>& analog_channels,
    uint64_t digital_sample_rate,
    uint64_t analog_sample_rate,
    double digital_threshold_volts,
    const std::vector<std::pair<int16_t, double>>& glitch_filters,
    const std::string& capture_mode,
    double duration_seconds,
    bool instant,
    int trigger_channel_index,
    const std::string& trigger_type,
    double after_trigger_seconds,
    double min_pulse_width_seconds,
    double max_pulse_width_seconds,
    const std::vector<std::pair<int16_t, std::string>>& linked_channels,
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
    int capture_ratio,
    double repeat_interval_seconds,
    uint64_t sample_count) {
    (void)analog_sample_rate;
    if (!_session)
        return Result<int>::Fail(ErrorCode::InternalError,
                                 "Session is null");
    if (!_device || !_device->have_instance())
        return Result<int>::Fail(ErrorCode::MissingDevice,
                                 "No device connected");
    if (_session->is_working())
        return Result<int>::Fail(ErrorCode::CaptureInProgress,
                                 "Capture already in progress");

    // Write debug log to file (GUI app stderr is not captured reliably)
    auto dbg_log = [](const char* msg) {
        static QFile dbg_file;
        if (!dbg_file.isOpen()) {
            dbg_file.setFileName(QDir::tempPath() + "/pxview_mcp_debug.log");
            (void)dbg_file.open(QIODevice::WriteOnly | QIODevice::Append);
        }
        if (dbg_file.isOpen()) {
            dbg_file.write(msg);
            dbg_file.write("\n");
            dbg_file.flush();
        }
    };

    dbg_log("configure_and_start: step 0 - ensure logic mode");

    // 0. Ensure device is in Logic mode if digital channels are requested.
    // If the device is in DSO or Analog mode, it won't have the expected
    // logic channels, and enable_probe() may hang or crash.
    {
        int cur_mode = _device->get_work_mode();
        const char* mode_names[] = {"LOGIC", "DSO", "ANALOG", "UNKNOWN"};
        int mode_idx = (cur_mode >= 0 && cur_mode <= 2) ? cur_mode : 3;
        dbg_log(QString("  current work mode: %1 (%2)").arg(cur_mode).arg(mode_names[mode_idx]).toUtf8().constData());

        GSList *channels = _device->get_channels();
        int ch_count = 0;
        for (GSList *l = channels; l; l = l->next) ch_count++;
        dbg_log(QString("  channel count: %1").arg(ch_count).toUtf8().constData());

        // If we need digital channels but have too few, force switch to LOGIC mode
        if (!digital_channels.empty() && (cur_mode != LOGIC || ch_count < 16)) {
            if (cur_mode != LOGIC) {
                dbg_log("  switching to LOGIC mode");
                _session->switch_work_mode(LOGIC);
            } else {
                // Device reports LOGIC mode but has too few channels.
                // This can happen after set_device() when the device was
                // previously in DSO mode. Force a mode cycle to reinitialize.
                dbg_log("  forcing mode cycle: LOGIC -> DSO -> LOGIC");
                _device->set_config_int16(SR_CONF_DEVICE_MODE, DSO);
                QCoreApplication::processEvents();
                _device->set_config_int16(SR_CONF_DEVICE_MODE, LOGIC);
                // Re-initialize signals after mode change
                _session->init_signals();
            }
            // Let UI process the mode change events
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();

            // Re-check channel count after mode switch
            channels = _device->get_channels();
            ch_count = 0;
            for (GSList *l = channels; l; l = l->next) ch_count++;
            dbg_log(QString("  after switch: channel count=%1, mode=%2").arg(ch_count).arg(_device->get_work_mode()).toUtf8().constData());
        }
    }

    dbg_log("configure_and_start: step 1 - configure channels");

    // 1. Configure channels directly using libsigrok API (bypassing
    // config_changed() callback) to avoid cascading UI updates while
    // we're in the middle of reconfiguring. We'll rebuild signals
    // once at the end via init_signals().
    {
        // Disable all channels first
        GSList *channels = _device->get_channels();
        int ch_count = 0;
        for (GSList *l = channels; l; l = l->next) ch_count++;
        dbg_log(QString("  total channels: %1").arg(ch_count).toUtf8().constData());

        for (GSList *l = channels; l; l = l->next) {
            auto *ch = static_cast<sr_channel *>(l->data);
            if (ch && ch->enabled) {
                dbg_log(QString("  disabling channel %1 via ds_enable").arg(ch->index).toUtf8().constData());
                ds_enable_device_channel_index(ch->index, false);
            }
        }

        // Enable specified digital channels
        for (int16_t idx : digital_channels) {
            dbg_log(QString("  enabling digital channel %1 via ds_enable").arg(idx).toUtf8().constData());
            ds_enable_device_channel_index(idx, true);
        }

        // Enable specified analog channels
        for (int16_t idx : analog_channels) {
            dbg_log(QString("  enabling analog channel %1 via ds_enable").arg(idx).toUtf8().constData());
            ds_enable_device_channel_index(idx, true);
        }
    }

    dbg_log("configure_and_start: step 2 - rebuild signals");

    // Note: We do NOT clear existing decoders here anymore.
    // If the user added decoders before starting capture (the recommended
    // workflow for MCP), those decoders should be preserved so that
    // DSV_MSG_COPY_TO_DOC_DONE can automatically start decoding for them
    // when the capture completes.
    //
    // If the user wants to clear decoders, they can call
    // clear_decoders() explicitly before start_capture().

    // 2a. Set channel mode if specified (e.g. "Buffer", "Stream")
    if (!channel_mode.empty()) {
        _device->set_config_string(SR_CONF_CHANNEL_MODE, channel_mode.c_str());
    }

    // 2b. Configure logic trigger if specified
    ds_trigger_reset();

    if (trigger_channel_index >= 0) {
        ds_trigger_set_en(1);
        ds_trigger_set_mode(SIMPLE_TRIGGER);

        // Mark trigger as preconfigured so TriggerDock::try_commit_trigger()
        // won't overwrite our settings with ds_trigger_reset() at capture start.
        _session->set_trigger_preconfigured(true);

        // Map trigger_type to ds_trigger_probe_set values
        // (following LogicSignal::commit_trig() pattern)
        if (trigger_type == "rising") {
            ds_trigger_probe_set(static_cast<uint16_t>(trigger_channel_index), 'R', 'X');
        } else if (trigger_type == "falling") {
            ds_trigger_probe_set(static_cast<uint16_t>(trigger_channel_index), 'F', 'X');
        } else if (trigger_type == "pulse_high") {
            ds_trigger_probe_set(static_cast<uint16_t>(trigger_channel_index), '1', 'X');
        } else if (trigger_type == "pulse_low") {
            ds_trigger_probe_set(static_cast<uint16_t>(trigger_channel_index), '0', 'X');
        } else {
            // Default to edge trigger
            ds_trigger_probe_set(static_cast<uint16_t>(trigger_channel_index), 'C', 'X');
        }

        // Set linked channel conditions (additional channels with required state)
        for (const auto &lc : linked_channels) {
            if (lc.second == "high") {
                ds_trigger_probe_set(static_cast<uint16_t>(lc.first), '1', 'X');
            } else if (lc.second == "low") {
                ds_trigger_probe_set(static_cast<uint16_t>(lc.first), '0', 'X');
            }
        }

        // Set trigger position based on afterTriggerSeconds
        if (after_trigger_seconds > 0.0) {
            uint64_t rate = (digital_sample_rate > 0) ? digital_sample_rate : _device->get_sample_rate();
            uint64_t sample_limit = _device->get_sample_limit();
            if (rate > 0 && sample_limit > 0) {
                uint64_t after_samples = static_cast<uint64_t>(
                    after_trigger_seconds * static_cast<double>(rate));
                uint16_t pos = static_cast<uint16_t>(
                    (after_samples * 100) / sample_limit);
                if (pos > 100) pos = 100;
                ds_trigger_set_pos(pos);
            }
        }

        // Configure pulse width trigger counts if specified
        if ((trigger_type == "pulse_high" || trigger_type == "pulse_low") &&
            (min_pulse_width_seconds > 0.0 || max_pulse_width_seconds > 0.0)) {
            uint64_t rate = (digital_sample_rate > 0) ? digital_sample_rate : _device->get_sample_rate();
            if (rate > 0) {
                uint32_t min_count = static_cast<uint32_t>(
                    min_pulse_width_seconds * static_cast<double>(rate));
                uint32_t max_count = static_cast<uint32_t>(
                    max_pulse_width_seconds * static_cast<double>(rate));
                ds_trigger_stage_set_count(0, 1, min_count, max_count);
            }
        }
    } else {
        ds_trigger_set_en(0);
    }

    // 2c. Sync trigger state to LogicSignal UI (header trigger icons)
    // This ensures the per-channel trigger icons in the signal header
    // reflect the MCP-configured trigger, not just the ds_trigger API state.
    if (trigger_channel_index >= 0) {
        auto sigs = _session->get_signals();
        for (auto s : sigs) {
            if (s->signal_type() == SR_CHANNEL_LOGIC) {
                auto *logicSig = static_cast<view::LogicSignal*>(s);
                auto indices = logicSig->get_index_list();
                if (!indices.empty() && indices.front() == trigger_channel_index) {
                    int trig_type = view::LogicSignal::NONTRIG;
                    if (trigger_type == "rising") trig_type = view::LogicSignal::POSTRIG;
                    else if (trigger_type == "falling") trig_type = view::LogicSignal::NEGTRIG;
                    else if (trigger_type == "pulse_high") trig_type = view::LogicSignal::HIGTRIG;
                    else if (trigger_type == "pulse_low") trig_type = view::LogicSignal::LOWTRIG;
                    else trig_type = view::LogicSignal::EDGTRIG;
                    logicSig->set_trig(trig_type);
                }
                // Also sync linked channels
                for (const auto &lc : linked_channels) {
                    if (!indices.empty() && indices.front() == lc.first) {
                        int trig_type = (lc.second == "high") ?
                            view::LogicSignal::HIGTRIG : view::LogicSignal::LOWTRIG;
                        logicSig->set_trig(trig_type);
                    }
                }
            }
        }
    }

    // 2. Rebuild signal list to reflect the new channel enable/disable state.
    // This is critical: action_start_capture() checks _signals.empty() and
    // the signal list must match the currently enabled channels.
    // init_signals() also clears view data and updates sample rate/limit.
    _session->init_signals();

    // Let the UI process the signals_changed event triggered by init_signals()
    QCoreApplication::processEvents();

    dbg_log("configure_and_start: step 3 - set sample rate");

    // 3. Set sample rates
    if (digital_sample_rate > 0) {
        bool ok = _device->set_config_uint64(SR_CONF_SAMPLERATE,
                                             digital_sample_rate);
        if (!ok)
            return Result<int>::Fail(ErrorCode::ConfigInvalid,
                                     "Failed to set digital sample rate");
    }

    dbg_log("configure_and_start: step 4 - set threshold");

    // 4. Set digital threshold voltage (VTH)
    if (digital_threshold_volts != 0.0) {
        bool ok = _device->set_config_double(SR_CONF_VTH,
                                             digital_threshold_volts);
        if (!ok)
            return Result<int>::Fail(ErrorCode::ConfigInvalid,
                                     "Failed to set digital threshold voltage");
    }

    dbg_log("configure_and_start: step 5 - glitch filters");

    // 5. Configure glitch filters
    if (!glitch_filters.empty()) {
        std::vector<uint32_t> thresholds;
        std::vector<::GlitchFilterMode> modes;
        thresholds.reserve(glitch_filters.size());
        modes.reserve(glitch_filters.size());

        for (const auto &gf : glitch_filters) {
            thresholds.push_back(static_cast<uint32_t>(gf.second));
            modes.push_back(GLITCH_FILTER_BOTH);
        }

        _session->set_glitch_filter(thresholds, modes);
    }

    dbg_log("configure_and_start: step 5b - RLE");

    // 5b. Set RLE if specified
    if (rle_enabled) {
        _device->set_config_bool(SR_CONF_RLE, true);
    }

    dbg_log("configure_and_start: step 5c - stream buffer");

    // 5c. Set disk cache and stream buffer sizes
    if (disk_cache_enabled) {
        _device->set_config_bool(SR_CONF_DISK_CACHE_ENABLE, true);
        if (stream_buffer_size_gb > 0.0) {
            _device->set_config_double(SR_CONF_STREAM_BUFF, stream_buffer_size_gb);
        }
        if (!disk_cache_path.empty()) {
            _device->set_config_string(SR_CONF_DISK_CACHE_PATH, disk_cache_path.c_str());
        }
    } else {
        _device->set_config_bool(SR_CONF_DISK_CACHE_ENABLE, false);
        if (stream_mem_buffer_size_gb > 0.0) {
            _device->set_config_double(SR_CONF_STREAM_MEM_BUFF, stream_mem_buffer_size_gb);
        }
    }

    dbg_log("configure_and_start: step 5d - threshold preset");

    // 5d. Set threshold preset if specified (distinct from VTH raw voltage)
    if (!threshold_preset.empty()) {
        _device->set_config_string(SR_CONF_THRESHOLD, threshold_preset.c_str());
    }

    dbg_log("configure_and_start: step 5e - operation mode");

    // 5e. Set operation mode if specified
    if (!operation_mode.empty()) {
        // Operation mode is a list-type config; try string first, then int16
        if (!_device->set_config_string(SR_CONF_OPERATION_MODE, operation_mode.c_str())) {
            // Some devices use int16 for operation mode
            // Try common mappings: Buffer=0, Stream=1, InternalTest=2
            int16_t mode_val = -1;
            if (operation_mode == "Buffer" || operation_mode == "buffer") mode_val = 0;
            else if (operation_mode == "Stream" || operation_mode == "stream") mode_val = 1;
            else if (operation_mode == "Internal test" || operation_mode == "internal_test") mode_val = 2;
            if (mode_val >= 0) {
                _device->set_config_int16(SR_CONF_OPERATION_MODE, mode_val);
            }
        }
    }

    dbg_log("configure_and_start: step 5f - buffer options");

    // 5f. Set buffer options if specified
    if (!buffer_options.empty()) {
        _device->set_config_string(SR_CONF_BUFFER_OPTIONS, buffer_options.c_str());
    }

    dbg_log("configure_and_start: step 5g - digital filter");

    // 5g. Set digital filter if specified
    if (!digital_filter.empty()) {
        _device->set_config_string(SR_CONF_FILTER, digital_filter.c_str());
    }

    dbg_log("configure_and_start: step 6 - set capture mode");

    // 6. Set capture mode
    if (capture_mode == "single" || capture_mode == "manual") {
        _session->set_collect_mode(COLLECT_SINGLE);
    } else if (capture_mode == "repeat") {
        _session->set_collect_mode(COLLECT_REPEAT);
    } else if (capture_mode == "loop") {
        _session->set_collect_mode(COLLECT_LOOP);
    }

    // Set repeat interval if specified
    if (repeat_interval_seconds > 0.0) {
        _session->set_repeat_intvl(repeat_interval_seconds);
    }

    // 6b. Set capture ratio (trigger position percentage) if specified
    if (capture_ratio >= 0 && capture_ratio <= 100) {
        _device->set_config_uint64(SR_CONF_CAPTURE_RATIO, static_cast<uint64_t>(capture_ratio));
    }

    dbg_log("configure_and_start: step 7 - set duration");

    // 7. Set duration (sample limit) if specified
    if (duration_seconds > 0.0) {
        uint64_t rate = _device->get_sample_rate();
        if (rate > 0) {
            uint64_t sample_limit = static_cast<uint64_t>(
                duration_seconds * static_cast<double>(rate));
            _device->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_limit);
        }
    } else if (sample_count > 0) {
        // Use explicit sample count when duration is not specified
        _device->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_count);
    }

    // Let the UI process any pending config change events before starting
    QCoreApplication::processEvents();

    dbg_log("configure_and_start: step 8 - start capture");

    // 10. Start capture
    bool ok = _session->start_capture(instant);
    if (!ok) {
        dbg_log("configure_and_start: start_capture FAILED");
        return Result<int>::Fail(ErrorCode::DeviceError,
                                 "Failed to start capture");
    }

    dbg_log("configure_and_start: SUCCESS");
    _capture_id++;
    return Result<int>::Success(_capture_id);
}

int SessionService::get_current_capture_id() const {
    return _capture_id;
}

Result<void> SessionService::close_capture() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is null");

    // If capture is running, stop it first
    if (_session->is_working()) {
        bool ok = _session->stop_capture();
        if (!ok)
            return Result<void>::Fail(ErrorCode::DeviceError,
                                     "Failed to stop running capture");
    }

    // Note: We intentionally do NOT call clear_view_data() or
    // clear_all_decoder() here because those trigger UI callbacks
    // (data_updated, signals_changed) that can crash when invoked
    // from the MCP context. The next start_capture() will implicitly
    // clear old data via action_start_capture().

    return Result<void>::Success();
}

// ===========================================================================
// 2. Capture state
// ===========================================================================

CaptureState SessionService::get_capture_state() const {
    if (!_session)
        return CaptureState::Empty;

    if (_session->is_running_status())
        return CaptureState::Recording;
    if (_session->is_working())
        return CaptureState::Starting;
    if (_session->is_init_status())
        return CaptureState::Empty;

    // Stopped with data
    if (_session->have_view_data())
        return CaptureState::Stopped;
    return CaptureState::Empty;
}

CaptureStatus SessionService::get_capture_status() const {
    CaptureStatus status;
    if (!_session)
        return status;

    status.state = get_capture_state();
    status.is_instant = _session->is_instant();
    status.is_saving = _session->is_saving();
    status.have_view_data = _session->have_view_data();
    status.have_hardware_data = _session->have_hardware_data();
    status.have_decoded_result = _session->have_decoded_result();
    status.is_copy_in_progress = _session->is_copy_in_progress();
    status.is_glitch_filter_active = _session->is_glitch_filter_active();
    status.is_signal_invert_active = _session->is_signal_invert_active();

    bool triggered = false;
    int progress = 0;
    _session->get_capture_status(triggered, progress);
    status.triggered = triggered;
    status.progress = progress / 100.0;

    return status;
}

bool SessionService::can_start_capture() const {
    if (!_session)
        return false;
    return !_session->is_working() && _device && _device->have_instance();
}

bool SessionService::can_stop_capture() const {
    if (!_session)
        return false;
    return _session->is_working();
}

// ===========================================================================
// 3. Device info
// ===========================================================================

DeviceInfo SessionService::get_device_info() const {
    DeviceInfo info;
    if (!_device || !_device->have_instance())
        return info;

    info.driver_name = _device->driver_name().toStdString();
    info.display_name = _device->name().toStdString();
    info.path = _device->path().toStdString();
    info.is_hardware = _device->is_hardware();
    info.is_demo = _device->is_demo();
    info.is_file = _device->is_file();
    info.is_virtual = _device->is_virtual();
    info.is_hardware_logic = _device->is_hardware_logic();
    info.is_hardware_dso = _device->is_hardware_dso();
    info.is_dsl_device = _device->is_dsl_device();
    info.is_compat_device = _device->is_compat_device();

    int usb_speed = 3; // LIBUSB_SPEED_HIGH
    _device->get_config_int32(SR_CONF_USB_SPEED, usb_speed);
    info.usb_speed = usb_speed;

    // Device ID from handle
    auto handle = _device->handle();
    // Convert handle to string representation
    info.id = std::to_string(static_cast<intptr_t>(handle));

    return info;
}

WorkMode SessionService::get_work_mode() const {
    if (!_device)
        return WorkMode::Unknown;

    int mode = _device->get_work_mode();
    switch (mode) {
    case LOGIC:
        return WorkMode::Logic;
    case ANALOG:
        return WorkMode::Analog;
    case DSO:
        return WorkMode::Dso;
    default:
        return WorkMode::Unknown;
    }
}

Result<std::vector<WorkMode>> SessionService::get_supported_work_modes() const {
    if (!_device || !_device->have_instance())
        return Result<std::vector<WorkMode>>::Fail(
            ErrorCode::MissingDevice, "No device connected");

    std::vector<WorkMode> modes;
    const GSList *mode_list = _device->get_device_mode_list();
    for (const GSList *l = mode_list; l; l = l->next) {
        auto *mode = static_cast<uint64_t *>(l->data);
        if (mode) {
            switch (*mode) {
            case LOGIC:
                modes.push_back(WorkMode::Logic);
                break;
            case ANALOG:
                modes.push_back(WorkMode::Analog);
                break;
            case DSO:
                modes.push_back(WorkMode::Dso);
                break;
            default:
                break;
            }
        }
    }
    return Result<std::vector<WorkMode>>::Success(modes);
}

// ===========================================================================
// 4. Channel management
// ===========================================================================

std::vector<ChannelInfo> SessionService::get_channels() const {
    std::vector<ChannelInfo> result;
    if (!_device)
        return result;

    GSList *channels = _device->get_channels();
    for (GSList *l = channels; l; l = l->next) {
        auto *ch = static_cast<sr_channel *>(l->data);
        if (!ch)
            continue;

        ChannelInfo info;
        info.index = static_cast<int32_t>(ch->index);
        info.name = ch->name ? ch->name : "";
        info.type = sr_channel_type_to_api(ch->type);
        info.enabled = ch->enabled;
        info.enabled_default = ch->enabled;
        result.push_back(info);
    }
    return result;
}

Result<void> SessionService::set_channel_enabled(int16_t index, bool enabled) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->enable_probe(index, enabled);
    if (!ok)
        return Result<void>::Fail(ErrorCode::ChannelNotFound,
                                  "Failed to enable/disable channel");
    return Result<void>::Success();
}

Result<void> SessionService::set_channel_name(int16_t index,
                                              const std::string &name) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_channel_name(index, name.c_str());
    if (!ok)
        return Result<void>::Fail(ErrorCode::ChannelNotFound,
                                  "Failed to set channel name");
    return Result<void>::Success();
}

uint16_t SessionService::get_channel_count(ChannelType type) const {
    if (!_session)
        return 0;

    int sr_type = SR_CHANNEL_LOGIC;
    switch (type) {
    case ChannelType::Logic:
        sr_type = SR_CHANNEL_LOGIC;
        break;
    case ChannelType::Analog:
        sr_type = SR_CHANNEL_ANALOG;
        break;
    case ChannelType::Dso:
        sr_type = SR_CHANNEL_DSO;
        break;
    }
    return _session->get_ch_num(sr_type);
}

// ===========================================================================
// 5. Sample config
// ===========================================================================

SampleConfig SessionService::get_sample_config() const {
    SampleConfig config;
    if (!_device || !_device->have_instance())
        return config;

    config.sample_rate = _device->get_sample_rate();
    config.sample_limit = _device->get_sample_limit();
    config.time_base = static_cast<double>(_device->get_time_base());

    if (_session) {
        config.collect_mode = static_cast<CollectMode>(_session->get_collect_mode());
        config.repeat_interval = _session->get_repeat_intvl();
        config.repeat_hold_percent = _session->get_repeat_hold() / 100.0;
    }

    config.stream_mode = _device->is_stream_mode();

    return config;
}

Result<void> SessionService::set_sample_rate(uint64_t rate) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_uint64(SR_CONF_SAMPLERATE, rate);
    if (!ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set sample rate");
    return Result<void>::Success();
}

Result<void> SessionService::set_sample_limit(uint64_t limit) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_uint64(SR_CONF_LIMIT_SAMPLES, limit);
    if (!ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set sample limit");
    return Result<void>::Success();
}

Result<void> SessionService::set_time_base(uint64_t tb) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_uint64(SR_CONF_TIMEBASE, tb);
    if (!ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set time base");
    return Result<void>::Success();
}

Result<void> SessionService::set_collect_mode(CollectMode mode) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    DEVICE_COLLECT_MODE cm = COLLECT_SINGLE;
    switch (mode) {
    case CollectMode::Single:
        cm = COLLECT_SINGLE;
        break;
    case CollectMode::Repeat:
        cm = COLLECT_REPEAT;
        break;
    case CollectMode::Loop:
        cm = COLLECT_LOOP;
        break;
    }

    _session->set_collect_mode(cm);
    return Result<void>::Success();
}

Result<void> SessionService::set_repeat_interval(double seconds) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    _session->set_repeat_intvl(seconds);
    return Result<void>::Success();
}

Result<uint64_t> SessionService::get_actual_sample_rate() const {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is null");

    uint64_t rate = _session->cur_samplerate();
    if (rate == 0)
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "Sample rate not available");
    return Result<uint64_t>::Success(rate);
}

Result<uint64_t> SessionService::get_actual_sample_count() const {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is null");

    uint64_t count = _session->cur_samplelimits();
    if (count == 0)
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "Sample count not available");
    return Result<uint64_t>::Success(count);
}

// ===========================================================================
// 6. Trigger config
// ===========================================================================

LogicTriggerConfig SessionService::get_logic_trigger_config() const {
    LogicTriggerConfig config;
    if (!_device || !_device->have_instance())
        return config;

    // Logic trigger configuration is managed through the ds_trigger API
    // rather than standard SR_CONF keys. Return the trigger enable state
    // and position as a JSON representation.
    uint16_t en = ds_trigger_get_en();
    uint16_t pos = ds_trigger_get_pos();

    config.config_json = "{\"enabled\":" + std::to_string(en) +
                         ",\"position\":" + std::to_string(pos) + "}";
    config.stage_count = 0;

    return config;
}

Result<void> SessionService::set_logic_trigger_config(
    const LogicTriggerConfig &config) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    // Logic trigger configuration is applied through the ds_trigger API.
    // The config_json should contain trigger pattern data that can be
    // parsed and applied via ds_trigger_set_stage, ds_trigger_set_en, etc.
    if (config.stage_count > 0) {
        ds_trigger_set_stage(static_cast<uint16_t>(config.stage_count - 1));
    }

    ds_trigger_set_en(config.config_json.empty() ? 0 : 1);

    return Result<void>::Success();
}

DsoTriggerConfig SessionService::get_dso_trigger_config() const {
    DsoTriggerConfig config;
    if (!_device || !_device->have_instance())
        return config;

    int value = 0;
    if (_device->get_config_int32(SR_CONF_TRIGGER_SOURCE, value))
        config.source = static_cast<TriggerSource>(value);

    if (_device->get_config_int32(SR_CONF_TRIGGER_SLOPE, value))
        config.slope = static_cast<TriggerSlope>(value);

    double dval = 0;
    if (_device->get_config_double(SR_CONF_HORIZ_TRIGGERPOS, dval))
        config.horiz_pos = dval;

    if (_device->get_config_double(SR_CONF_TRIGGER_HOLDOFF, dval))
        config.holdoff = dval;

    if (_device->get_config_double(SR_CONF_TRIGGER_MARGIN, dval))
        config.margin = dval;

    if (_device->get_config_int32(SR_CONF_TRIGGER_CHANNEL, value))
        config.channel = value;

    return config;
}

Result<void> SessionService::set_dso_trigger_config(
    const DsoTriggerConfig &config) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool any_ok = false;

    if (_device->set_config_int32(SR_CONF_TRIGGER_SOURCE,
                                  static_cast<int>(config.source)))
        any_ok = true;
    if (_device->set_config_int32(SR_CONF_TRIGGER_SLOPE,
                                  static_cast<int>(config.slope)))
        any_ok = true;
    if (_device->set_config_double(SR_CONF_HORIZ_TRIGGERPOS,
                                   config.horiz_pos))
        any_ok = true;
    if (_device->set_config_double(SR_CONF_TRIGGER_HOLDOFF,
                                   config.holdoff))
        any_ok = true;
    if (_device->set_config_double(SR_CONF_TRIGGER_MARGIN,
                                   config.margin))
        any_ok = true;
    if (_device->set_config_int32(SR_CONF_TRIGGER_CHANNEL,
                                  config.channel))
        any_ok = true;

    if (!any_ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set any DSO trigger config");
    return Result<void>::Success();
}

// ===========================================================================
// 7. Probe config
// ===========================================================================

ProbeConfig SessionService::get_probe_config(int16_t channel) const {
    ProbeConfig config;
    if (!_device || !_device->have_instance())
        return config;

    // Find the channel to get per-channel config
    GSList *channels = _device->get_channels();
    sr_channel *target_ch = nullptr;
    for (GSList *l = channels; l; l = l->next) {
        auto *ch = static_cast<sr_channel *>(l->data);
        if (ch && ch->index == channel) {
            target_ch = ch;
            break;
        }
    }

    double dval = 0;
    int ival = 0;

    if (target_ch) {
        if (_device->get_config_double(SR_CONF_PROBE_VDIV, dval, target_ch))
            config.vdiv = dval;

        if (_device->get_config_int32(SR_CONF_PROBE_COUPLING, ival, target_ch))
            config.coupling = (ival == 0) ? Coupling::AC : Coupling::DC;

        if (_device->get_config_double(SR_CONF_PROBE_FACTOR, dval, target_ch))
            config.vfactor = dval;
    } else {
        // Try without channel
        if (_device->get_config_double(SR_CONF_PROBE_VDIV, dval))
            config.vdiv = dval;
        if (_device->get_config_int32(SR_CONF_PROBE_COUPLING, ival))
            config.coupling = (ival == 0) ? Coupling::AC : Coupling::DC;
        if (_device->get_config_double(SR_CONF_PROBE_FACTOR, dval))
            config.vfactor = dval;
    }

    return config;
}

Result<void> SessionService::set_probe_config(int16_t channel,
                                              const ProbeConfig &config) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    // Find the channel
    GSList *channels = _device->get_channels();
    sr_channel *target_ch = nullptr;
    for (GSList *l = channels; l; l = l->next) {
        auto *ch = static_cast<sr_channel *>(l->data);
        if (ch && ch->index == channel) {
            target_ch = ch;
            break;
        }
    }

    bool any_ok = false;

    if (target_ch) {
        if (_device->set_config_double(SR_CONF_PROBE_VDIV, config.vdiv, target_ch))
            any_ok = true;
        if (_device->set_config_int32(SR_CONF_PROBE_COUPLING,
                                      config.coupling == Coupling::AC ? 0 : 1,
                                      target_ch))
            any_ok = true;
        if (_device->set_config_double(SR_CONF_PROBE_FACTOR, config.vfactor,
                                       target_ch))
            any_ok = true;
    } else {
        if (_device->set_config_double(SR_CONF_PROBE_VDIV, config.vdiv))
            any_ok = true;
        if (_device->set_config_int32(SR_CONF_PROBE_COUPLING,
                                      config.coupling == Coupling::AC ? 0 : 1))
            any_ok = true;
        if (_device->set_config_double(SR_CONF_PROBE_FACTOR, config.vfactor))
            any_ok = true;
    }

    if (!any_ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set probe config");
    return Result<void>::Success();
}

// ===========================================================================
// 8. Generic device config
// ===========================================================================

Result<std::string> SessionService::get_config_string(int key) {
    if (!_device || !_device->have_instance())
        return Result<std::string>::Fail(ErrorCode::MissingDevice,
                                         "No device connected");

    QString value;
    bool ok = _device->get_config_string(key, value);
    if (!ok)
        return Result<std::string>::Fail(ErrorCode::ConfigNotSupported,
                                         "Config key not supported");
    return Result<std::string>::Success(value.toStdString());
}

Result<bool> SessionService::set_config_string(int key,
                                               const std::string &value) {
    if (!_device || !_device->have_instance())
        return Result<bool>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_string(key, value.c_str());
    if (!ok)
        return Result<bool>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set config string");
    return Result<bool>::Success(true);
}

Result<bool> SessionService::get_config_bool(int key) {
    if (!_device || !_device->have_instance())
        return Result<bool>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool value = false;
    bool ok = _device->get_config_bool(key, value);
    if (!ok)
        return Result<bool>::Fail(ErrorCode::ConfigNotSupported,
                                  "Config key not supported");
    return Result<bool>::Success(value);
}

Result<bool> SessionService::set_config_bool(int key, bool value) {
    if (!_device || !_device->have_instance())
        return Result<bool>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_bool(key, value);
    if (!ok)
        return Result<bool>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set config bool");
    return Result<bool>::Success(true);
}

Result<uint64_t> SessionService::get_config_uint64(int key) {
    if (!_device || !_device->have_instance())
        return Result<uint64_t>::Fail(ErrorCode::MissingDevice,
                                      "No device connected");

    uint64_t value = 0;
    bool ok = _device->get_config_uint64(key, value);
    if (!ok)
        return Result<uint64_t>::Fail(ErrorCode::ConfigNotSupported,
                                      "Config key not supported");
    return Result<uint64_t>::Success(value);
}

Result<bool> SessionService::set_config_uint64(int key, uint64_t value) {
    if (!_device || !_device->have_instance())
        return Result<bool>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_uint64(key, value);
    if (!ok)
        return Result<bool>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set config uint64");
    return Result<bool>::Success(true);
}

Result<int32_t> SessionService::get_config_int32(int key) {
    if (!_device || !_device->have_instance())
        return Result<int32_t>::Fail(ErrorCode::MissingDevice,
                                     "No device connected");

    int value = 0;
    bool ok = _device->get_config_int32(key, value);
    if (!ok)
        return Result<int32_t>::Fail(ErrorCode::ConfigNotSupported,
                                     "Config key not supported");
    return Result<int32_t>::Success(static_cast<int32_t>(value));
}

Result<bool> SessionService::set_config_int32(int key, int32_t value) {
    if (!_device || !_device->have_instance())
        return Result<bool>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_int32(key, value);
    if (!ok)
        return Result<bool>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set config int32");
    return Result<bool>::Success(true);
}

Result<double> SessionService::get_config_double(int key) {
    if (!_device || !_device->have_instance())
        return Result<double>::Fail(ErrorCode::MissingDevice,
                                    "No device connected");

    double value = 0;
    bool ok = _device->get_config_double(key, value);
    if (!ok)
        return Result<double>::Fail(ErrorCode::ConfigNotSupported,
                                    "Config key not supported");
    return Result<double>::Success(value);
}

Result<bool> SessionService::set_config_double(int key, double value) {
    if (!_device || !_device->have_instance())
        return Result<bool>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_double(key, value);
    if (!ok)
        return Result<bool>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set config double");
    return Result<bool>::Success(true);
}

Result<uint8_t> SessionService::get_config_byte(int key) {
    if (!_device || !_device->have_instance())
        return Result<uint8_t>::Fail(ErrorCode::MissingDevice,
                                     "No device connected");

    int value = 0;
    bool ok = _device->get_config_byte(key, value);
    if (!ok)
        return Result<uint8_t>::Fail(ErrorCode::ConfigNotSupported,
                                     "Config key not supported");
    return Result<uint8_t>::Success(static_cast<uint8_t>(value));
}

Result<bool> SessionService::set_config_byte(int key, uint8_t value) {
    if (!_device || !_device->have_instance())
        return Result<bool>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    bool ok = _device->set_config_byte(key, value);
    if (!ok)
        return Result<bool>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set config byte");
    return Result<bool>::Success(true);
}

bool SessionService::has_config(int key) const {
    if (!_device || !_device->have_instance())
        return false;
    return _device->have_config(key);
}

// ===========================================================================
// 9. Time & trigger
// ===========================================================================

TimeInfo SessionService::get_time_info() const {
    TimeInfo info;
    if (!_session)
        return info;

    info.session_start_ms =
        _session->get_session_time().toMSecsSinceEpoch();
    info.trigger_pos = static_cast<int64_t>(_session->get_trigger_pos());
    info.trigger_time_ms = _session->get_trig_time().toMSecsSinceEpoch();
    info.is_triggered = _session->is_triged();
    info.session_duration_sec = _session->cur_sampletime();
    info.sample_time_sec = _session->cur_snap_sampletime();
    info.view_time_sec = _session->cur_view_time();

    return info;
}

uint64_t SessionService::get_samplerate() const {
    if (!_session)
        return 0;
    return _session->cur_samplerate();
}

uint64_t SessionService::get_sample_count() const {
    if (!_session)
        return 0;
    return _session->cur_samplelimits();
}

double SessionService::get_sample_time() const {
    if (!_session)
        return 0.0;
    return _session->cur_sampletime();
}

uint64_t SessionService::get_trigger_pos() const {
    if (!_session)
        return 0;
    return _session->get_trigger_pos();
}

// ===========================================================================
// 10. Signal list
// ===========================================================================

std::vector<SignalInfo> SessionService::get_signal_list() const {
    std::vector<SignalInfo> result;
    if (!_session)
        return result;

    auto &sig_list = _session->get_signals();
    for (auto *sig : sig_list) {
        if (!sig)
            continue;

        SignalInfo info;
        info.index = sig->get_index();
        info.name = sig->get_name().toStdString();
        info.type = sr_channel_type_to_api(sig->signal_type());
        info.enabled = sig->enabled();
        info.color = sig->get_colour().name(QColor::HexRgb).toStdString();

        // Probe config for analog/dso signals
        if (info.type == ChannelType::Analog ||
            info.type == ChannelType::Dso) {
            info.probe = get_probe_config(static_cast<int16_t>(info.index));
        }

        result.push_back(info);
    }
    return result;
}

// ===========================================================================
// 11. Waveform data reading
// ===========================================================================

Result<uint64_t> SessionService::get_logic_samples(
    uint64_t start_sample, uint64_t end_sample,
    const std::vector<int16_t> &channel_indices,
    std::vector<uint8_t> &out_data) {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is null");

    auto *snapshot = _session->get_logic_snapshot();
    if (!snapshot || !snapshot->have_data())
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "No logic data available");

    out_data.clear();
    uint64_t total_copied = 0;

    for (auto ch_idx : channel_indices) {
        uint64_t actual_end = end_sample;
        const uint8_t *data = snapshot->get_samples(start_sample, actual_end,
                                                     static_cast<int>(ch_idx));
        if (!data)
            continue;

        uint64_t count = actual_end - start_sample + 1;
        size_t byte_count = static_cast<size_t>(count);
        out_data.insert(out_data.end(), data, data + byte_count);
        total_copied += count;
    }

    return Result<uint64_t>::Success(total_copied);
}

Result<uint64_t> SessionService::get_analog_samples(
    uint64_t start_sample, uint64_t end_sample,
    int16_t channel_index,
    std::vector<float> &out_data) {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is null");

    auto *snapshot = _session->get_analog_snapshot();
    if (!snapshot || !snapshot->have_data())
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "No analog data available");

    out_data.clear();

    const uint8_t *raw = snapshot->get_samples(static_cast<int64_t>(start_sample));
    if (!raw)
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "Failed to read analog samples");

    uint64_t count = end_sample - start_sample + 1;
    int pitch = snapshot->get_scale_factor();

    out_data.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; i++) {
        uint8_t byte_val = raw[i * pitch + channel_index];
        out_data.push_back(static_cast<float>(byte_val) / 255.0f);
    }

    return Result<uint64_t>::Success(count);
}

Result<uint64_t> SessionService::get_dso_samples(
    uint64_t start_sample, uint64_t end_sample,
    int16_t channel_index,
    std::vector<float> &out_data) {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is null");

    auto *snapshot = _session->get_dso_snapshot();
    if (!snapshot || !snapshot->have_data())
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "No DSO data available");

    out_data.clear();

    const uint8_t *raw = snapshot->get_samples(
        static_cast<int64_t>(start_sample),
        static_cast<int64_t>(end_sample),
        static_cast<uint16_t>(channel_index));
    if (!raw)
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "Failed to read DSO samples");

    uint64_t count = end_sample - start_sample + 1;
    float data_scale = snapshot->get_data_scale(channel_index);

    out_data.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; i++) {
        out_data.push_back(static_cast<float>(raw[i]) * data_scale);
    }

    return Result<uint64_t>::Success(count);
}

Result<uint64_t> SessionService::find_next_edge(
    uint64_t from_sample, int16_t channel_index, bool rising_edge) {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is null");

    auto *snapshot = _session->get_logic_snapshot();
    if (!snapshot || !snapshot->have_data())
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "No logic data available");

    uint64_t index = from_sample;
    bool last_sample = !rising_edge;
    uint64_t end = snapshot->get_sample_count() - 1;
    bool found = snapshot->get_nxt_edge(index, last_sample, end, 0,
                                         static_cast<int>(channel_index));
    if (!found)
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "No edge found");

    return Result<uint64_t>::Success(index);
}

Result<uint64_t> SessionService::find_pattern(
    uint64_t from_sample, int16_t channel_index,
    const std::string &pattern) {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is null");

    auto *snapshot = _session->get_logic_snapshot();
    if (!snapshot || !snapshot->have_data())
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "No logic data available");

    std::map<uint16_t, QString> pattern_map;
    pattern_map[static_cast<uint16_t>(channel_index)] =
        QString::fromStdString(pattern);

    int64_t index = static_cast<int64_t>(from_sample);
    int64_t end = static_cast<int64_t>(snapshot->get_sample_count() - 1);
    bool found = snapshot->pattern_search(static_cast<int64_t>(from_sample),
                                          end, index, pattern_map, true);
    if (!found)
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "Pattern not found");

    return Result<uint64_t>::Success(static_cast<uint64_t>(index));
}

// ===========================================================================
// 12. Decoder management
// ===========================================================================

std::vector<DecoderDescriptor> SessionService::get_available_decoders() const {
    std::vector<DecoderDescriptor> result;

    // Python decoders
    const GSList *decoders = srd_decoder_list();
    for (const GSList *l = decoders; l; l = l->next) {
        auto *dec = static_cast<srd_decoder *>(l->data);
        if (!dec)
            continue;

        DecoderDescriptor desc;
        desc.id = dec->id ? dec->id : "";
        desc.name = dec->name ? dec->name : "";
        desc.long_name = ensure_utf8(dec->longname);

        int ch_count = 0;
        for (GSList *c = dec->channels; c; c = c->next)
            ch_count++;
        desc.channels = ch_count;

        int opt_count = 0;
        for (GSList *c = dec->opt_channels; c; c = c->next)
            opt_count++;
        desc.optional_channels = opt_count;

        // Include channel details (id, name, desc) so MCP clients know
        // what keys to use in the channelMap
        for (GSList *c = dec->channels; c; c = c->next) {
            auto *ch = static_cast<srd_channel*>(c->data);
            if (!ch) continue;
            DecoderChannelInfo chi;
            chi.id = ch->id ? ch->id : "";
            chi.name = ch->name ? ch->name : "";
            chi.desc = ch->desc ? ch->desc : "";
            chi.order = ch->order;
            chi.is_optional = false;
            desc.channel_info.push_back(chi);
        }
        for (GSList *c = dec->opt_channels; c; c = c->next) {
            auto *ch = static_cast<srd_channel*>(c->data);
            if (!ch) continue;
            DecoderChannelInfo chi;
            chi.id = ch->id ? ch->id : "";
            chi.name = ch->name ? ch->name : "";
            chi.desc = ch->desc ? ch->desc : "";
            chi.order = ch->order;
            chi.is_optional = true;
            desc.channel_info.push_back(chi);
        }

        result.push_back(desc);
    }

    return result;
}

Result<json> SessionService::get_decoder_options(const std::string& decoder_id) {
    // Find the decoder by ID
    const GSList *decoders = srd_decoder_list();
    const srd_decoder *target_dec = nullptr;
    for (const GSList *l = decoders; l; l = l->next) {
        auto *dec = static_cast<srd_decoder *>(l->data);
        if (!dec || !dec->id) continue;
        if (decoder_id == dec->id) {
            target_dec = dec;
            break;
        }
    }

    if (!target_dec)
        return Result<json>::Fail(ErrorCode::DecoderNotFound,
                                  "Decoder not found: " + decoder_id);

    json result;

    // Required channels
    json channels = json::array();
    for (const GSList *c = target_dec->channels; c; c = c->next) {
        auto *ch = static_cast<srd_channel*>(c->data);
        if (!ch) continue;
        channels.push_back({
            {"name", ch->name ? ch->name : ""},
            {"desc", ensure_utf8(ch->desc)},
            {"idn", ch->idn ? ch->idn : ""},
            {"required", true}
        });
    }

    // Optional channels
    for (const GSList *c = target_dec->opt_channels; c; c = c->next) {
        auto *ch = static_cast<srd_channel*>(c->data);
        if (!ch) continue;
        channels.push_back({
            {"name", ch->name ? ch->name : ""},
            {"desc", ensure_utf8(ch->desc)},
            {"idn", ch->idn ? ch->idn : ""},
            {"required", false}
        });
    }
    result["channels"] = channels;

    // Options — match the GUI dialog (DecoderOptionsDlg / DecoderOptions binding)
    json options = json::array();
    for (const GSList *o = target_dec->options; o; o = o->next) {
        auto *opt = static_cast<srd_decoder_option*>(o->data);
        if (!opt) continue;
        json opt_obj;
        opt_obj["id"] = opt->id ? opt->id : "";
        opt_obj["desc"] = ensure_utf8(opt->desc);
        opt_obj["idn"] = opt->idn ? opt->idn : "";

        // Build the enum values list first (needed to determine type)
        json values = json::array();
        if (opt->values) {
            for (const GSList *v = opt->values; v; v = v->next) {
                auto *val = static_cast<GVariant*>(v->data);
                if (!val) continue;
                if (g_variant_is_of_type(val, G_VARIANT_TYPE("s"))) {
                    const gchar *val_str = g_variant_get_string(val, nullptr);
                    if (val_str)
                        values.push_back(val_str);
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("d"))) {
                    values.push_back(g_variant_get_double(val));
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("x"))) {
                    values.push_back(g_variant_get_int64(val));
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("b"))) {
                    values.push_back(g_variant_get_boolean(val) ? "True" : "False");
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("y"))) {
                    values.push_back(static_cast<int64_t>(g_variant_get_byte(val)));
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("n"))) {
                    values.push_back(static_cast<int64_t>(g_variant_get_int16(val)));
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("q"))) {
                    values.push_back(static_cast<int64_t>(g_variant_get_uint16(val)));
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("i"))) {
                    values.push_back(static_cast<int64_t>(g_variant_get_int32(val)));
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("u"))) {
                    values.push_back(static_cast<int64_t>(g_variant_get_uint32(val)));
                } else if (g_variant_is_of_type(val, G_VARIANT_TYPE("t"))) {
                    values.push_back(static_cast<int64_t>(g_variant_get_uint64(val)));
                }
            }
        }

        // Determine option type and default value (matching DecoderOptions binding logic)
        if (opt->values) {
            // Has enum value list → enum type (same as GUI's bind_enum)
            opt_obj["type"] = "enum";
            // Default value for enum: find which value matches opt->def
            if (opt->def) {
                int idx = 0;
                for (const GSList *v = opt->values; v; v = v->next, idx++) {
                    auto *val = static_cast<GVariant*>(v->data);
                    if (!val) continue;
                    if (g_variant_compare(val, opt->def) == 0) {
                        // Return the same representation as in the values array
                        if (idx < static_cast<int>(values.size()))
                            opt_obj["default"] = values[idx];
                        break;
                    }
                }
            }
        } else if (opt->def) {
            if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("d"))) {
                opt_obj["type"] = "double";
                opt_obj["default"] = g_variant_get_double(opt->def);
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("x"))) {
                opt_obj["type"] = "int";
                opt_obj["default"] = g_variant_get_int64(opt->def);
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("s"))) {
                opt_obj["type"] = "string";
                const gchar *def_str = g_variant_get_string(opt->def, nullptr);
                opt_obj["default"] = def_str ? def_str : "";
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("b"))) {
                opt_obj["type"] = "bool";
                opt_obj["default"] = g_variant_get_boolean(opt->def) ? true : false;
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("y"))) {
                opt_obj["type"] = "int";
                opt_obj["default"] = static_cast<int64_t>(g_variant_get_byte(opt->def));
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("n"))) {
                opt_obj["type"] = "int";
                opt_obj["default"] = static_cast<int64_t>(g_variant_get_int16(opt->def));
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("q"))) {
                opt_obj["type"] = "int";
                opt_obj["default"] = static_cast<int64_t>(g_variant_get_uint16(opt->def));
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("i"))) {
                opt_obj["type"] = "int";
                opt_obj["default"] = static_cast<int64_t>(g_variant_get_int32(opt->def));
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("u"))) {
                opt_obj["type"] = "int";
                opt_obj["default"] = static_cast<int64_t>(g_variant_get_uint32(opt->def));
            } else if (g_variant_is_of_type(opt->def, G_VARIANT_TYPE("t"))) {
                opt_obj["type"] = "int";
                opt_obj["default"] = static_cast<int64_t>(g_variant_get_uint64(opt->def));
            } else {
                opt_obj["type"] = "string";
                const gchar *def_str = g_variant_print(opt->def, false);
                opt_obj["default"] = def_str ? def_str : "";
            }
        } else {
            opt_obj["type"] = "string";
        }

        opt_obj["values"] = values;
        options.push_back(opt_obj);
    }
    result["options"] = options;

    // Available signals for channel mapping (matching create_probe_selector logic)
    json available_signals = json::array();
    if (_session) {
        auto &sig_list = _session->get_signals();
        for (auto *sig : sig_list) {
            if (!sig || !sig->enabled())
                continue;
            // Only logic signals can be mapped to decoder channels
            if (sig->signal_type() != SR_CHANNEL_LOGIC)
                continue;
            available_signals.push_back({
                {"index", sig->get_index()},
                {"name", sig->get_name().toStdString()}
            });
        }
    }
    result["availableSignals"] = available_signals;

    return Result<json>::Success(result);
}

std::vector<DecoderInstance> SessionService::get_active_decoders() const {
    std::vector<DecoderInstance> result;
    if (!_session)
        return result;

    auto &traces = _session->get_decode_signals();
    for (size_t i = 0; i < traces.size(); i++) {
        auto *trace = traces[i];
        if (!trace)
            continue;

        DecoderInstance inst;
        inst.instance_id = std::to_string(
            reinterpret_cast<intptr_t>(trace));
        inst.row_index = static_cast<int32_t>(i);

        auto *decoder_stack = trace->decoder();
        if (decoder_stack) {
            inst.is_running = decoder_stack->IsRunning();
            inst.progress = decoder_stack->get_progress() / 100.0;
            const char *root_id = decoder_stack->get_root_decoder_id();
            inst.decoder_id = root_id ? root_id : "";
        }

        inst.display_name = trace->get_name().toStdString();

        result.push_back(inst);
    }
    return result;
}

Result<std::string> SessionService::add_decoder(
    const std::string &decoder_id,
    const std::map<std::string, std::string> &options,
    const std::map<std::string, int16_t> &channel_map,
    const std::string &label,
    bool wait_for_completion,
    const std::string &stack_on_analyzer_id) {
    if (!_session)
        return Result<std::string>::Fail(ErrorCode::InternalError,
                                         "Session is null");

    // Look up the decoder by ID
    srd_decoder *dec = srd_decoder_get_by_id(decoder_id.c_str());
    if (!dec)
        return Result<std::string>::Fail(ErrorCode::DecoderNotFound,
                                         "Decoder not found: " + decoder_id);

    // Handle stacked decoder: add to an existing DecoderStack instead of
    // creating a new DecodeTrace. This follows the same pattern as
    // ProtocolDock::on_add_protocol() which builds sub_decoders and
    // passes them to SigSession::add_decoder().
    if (!stack_on_analyzer_id.empty()) {
        auto do_stack = [this, dec, &options, &channel_map, &label, &stack_on_analyzer_id]() -> Result<std::string> {
            // Find the parent DecodeTrace by converting the analyzer ID
            // (which is the string representation of the trace pointer)
            auto &traces = _session->get_decode_signals();
            view::DecodeTrace *parent_trace = nullptr;
            for (auto *trace : traces) {
                if (!trace) continue;
                std::string tid =
                    std::to_string(reinterpret_cast<intptr_t>(trace));
                if (tid == stack_on_analyzer_id) {
                    parent_trace = trace;
                    break;
                }
            }

            if (!parent_trace)
                return Result<std::string>::Fail(ErrorCode::DecoderNotFound,
                                                 "Parent analyzer not found: " + stack_on_analyzer_id);

            auto *decoder_stack = parent_trace->decoder();
            if (!decoder_stack)
                return Result<std::string>::Fail(ErrorCode::DecoderError,
                                                 "Parent decoder stack is null");

            // Create the new sub-decoder and add it to the parent stack
            auto *new_decoder = new data::decode::Decoder(dec);
            decoder_stack->add_sub_decoder(new_decoder);

            // Apply label to the parent trace if specified
            if (!label.empty()) {
                parent_trace->set_name(QString::fromStdString(label));
            }

            // Apply options to the new sub-decoder
            auto &stack = decoder_stack->stack();
            if (!stack.empty()) {
                auto *sub_dec = stack.back(); // the newly added decoder

                for (const auto &opt : options) {
                    GVariant *val = nullptr;
                    bool found_type = false;

                    for (const GSList *o = dec->options; o; o = o->next) {
                        auto *opt_def = static_cast<srd_decoder_option*>(o->data);
                        if (!opt_def || !opt_def->id) continue;
                        if (opt.first != opt_def->id) continue;

                        if (opt_def->values) {
                            for (const GSList *v = opt_def->values; v; v = v->next) {
                                auto *enum_val = static_cast<GVariant*>(v->data);
                                if (!enum_val) continue;
                                gchar *enum_str = g_variant_print(enum_val, false);
                                std::string cmp_str = enum_str ? enum_str : "";
                                g_free(enum_str);
                                if (cmp_str.size() >= 2 && cmp_str.front() == '\'' && cmp_str.back() == '\'')
                                    cmp_str = cmp_str.substr(1, cmp_str.size() - 2);
                                if (cmp_str == opt.second) {
                                    if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("s")))
                                        val = g_variant_new_string(g_variant_get_string(enum_val, nullptr));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("t")))
                                        val = g_variant_new_uint64(g_variant_get_uint64(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("x")))
                                        val = g_variant_new_int64(g_variant_get_int64(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("d")))
                                        val = g_variant_new_double(g_variant_get_double(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("b")))
                                        val = g_variant_new_boolean(g_variant_get_boolean(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("i")))
                                        val = g_variant_new_int32(g_variant_get_int32(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("u")))
                                        val = g_variant_new_uint32(g_variant_get_uint32(enum_val));
                                    else
                                        val = g_variant_new_string(opt.second.c_str());
                                    break;
                                }
                            }
                            if (!val && opt_def->def) {
                                if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("s")))
                                    val = g_variant_new_string(opt.second.c_str());
                                else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("d")))
                                    val = g_variant_new_double(std::stod(opt.second));
                                else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("x")))
                                    val = g_variant_new_int64(std::stoll(opt.second));
                                else
                                    val = g_variant_new_string(opt.second.c_str());
                            }
                            if (!val)
                                val = g_variant_new_string(opt.second.c_str());
                        } else if (opt_def->def) {
                            if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("d")))
                                val = g_variant_new_double(std::stod(opt.second));
                            else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("x")))
                                val = g_variant_new_int64(std::stoll(opt.second));
                            else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("s")))
                                val = g_variant_new_string(opt.second.c_str());
                            else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("b")))
                                val = g_variant_new_boolean(opt.second == "True" || opt.second == "1");
                            else
                                val = g_variant_new_string(opt.second.c_str());
                        } else {
                            val = g_variant_new_string(opt.second.c_str());
                        }
                        found_type = true;
                        break;
                    }

                    if (!found_type)
                        val = g_variant_new_string(opt.second.c_str());

                    sub_dec->set_option(opt.first.c_str(), val);
                }
            }

            decoder_stack->set_options_changed(true);

            // Prepare decode if data is ready
            bool copy_in_progress = _session->is_copy_in_progress();
            if (!_session->have_view_data() || copy_in_progress) {
                decoder_stack->set_options_changed(true);
            } else {
                decoder_stack->set_capture_end_flag(true);
                decoder_stack->frame_ended();
            }

            _session->rebuild_decoder_pannel();

            std::string instance_id =
                std::to_string(reinterpret_cast<intptr_t>(parent_trace));

            broadcast_event(ServiceEvent::DecoderAdded,
                            {{"instance_id", instance_id},
                             {"decoder_id", dec->id ? dec->id : ""},
                             {"stacked_on", stack_on_analyzer_id}});

            return Result<std::string>::Success(instance_id);
        };

        Result<std::string> result = Result<std::string>::Fail(
            ErrorCode::InternalError, "Pending");

        if (QThread::currentThread() == qApp->thread()) {
            result = do_stack();
        } else {
            std::mutex result_mutex;
            std::condition_variable result_cv;
            bool done = false;

            QMetaObject::invokeMethod(qApp, [&do_stack, &result, &result_mutex, &result_cv, &done]() {
                result = do_stack();
                {
                    std::lock_guard<std::mutex> lock(result_mutex);
                    done = true;
                }
                result_cv.notify_one();
            }, Qt::BlockingQueuedConnection);

            {
                std::unique_lock<std::mutex> lock(result_mutex);
                result_cv.wait(lock, [&done]() { return done; });
            }
        }

        if (!result.ok())
            return result;

        // Start decode if data is ready and copy is not in progress
        {
            std::string instance_id = result.value();
            auto *decode_trace = reinterpret_cast<view::DecodeTrace*>(
                std::stoll(instance_id));
            auto *decoder_stack = decode_trace->decoder();

            if (decoder_stack && decoder_stack->options_changed() &&
                _session->have_view_data() &&
                !_session->is_copy_in_progress()) {
                QTimer::singleShot(0, qApp, [this, decode_trace]() {
                    if (decode_trace && !decode_trace->_delete_flag) {
                        _session->add_decode_task(decode_trace);
                    }
                });
            }
        }

        return result;
    }

    // The add_decoder operation creates QObjects (DecodeTrace) and triggers
    // Qt signals, so it MUST run on the main (Qt GUI) thread.
    // If already on the main thread, execute directly to avoid deadlock.

    auto do_add = [this, dec, &options, &channel_map, &label]() -> Result<std::string> {
        // Validate: decoder can only be added in LOGIC mode.
        // This mirrors ProtocolDock::add_protocol_by_id() which checks
        // get_work_mode() != LOGIC and rejects the operation.
        if (_session->get_device()->get_work_mode() != LOGIC) {
            return Result<std::string>::Fail(
                ErrorCode::DecoderError,
                "Protocol analyzers are only valid in Digital/Logic mode. "
                "Please switch to Logic mode first using switch_work_mode.");
        }

        // Validate: all required channels must be provided in channel_map.
        // This mirrors the check that ProtocolDock::create_popup() does via
        // DecoderStack::check_required_probes(). Without this validation,
        // MCP add_decoder with silent=true would bypass the channel
        // configuration dialog and allow decoders with missing required
        // channels, which would silently fail to decode.
        if (!channel_map.empty()) {
            // Check that all required channels are covered
            std::string missing;
            for (const GSList *c = dec->channels; c; c = c->next) {
                auto *ch = static_cast<srd_channel*>(c->data);
                std::string ch_id = ch->id ? ch->id : "";
                std::string ch_name = ch->name ? ch->name : "";
                std::string ch_desc = ch->desc ? ch->desc : "";

                auto ci_eq = [](const std::string& a, const std::string& b) {
                    if (a.size() != b.size()) return false;
                    for (size_t i = 0; i < a.size(); i++)
                        if (tolower(a[i]) != tolower(b[i])) return false;
                    return true;
                };

                bool found = false;
                for (const auto& [key, _val] : channel_map) {
                    if (ci_eq(key, ch_id) || ci_eq(key, ch_name) || ci_eq(key, ch_desc)) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    if (!missing.empty()) missing += ", ";
                    missing += ch_id.empty() ? (ch_name.empty() ? "?" : ch_name) : ch_id;
                }
            }

            // Auto-map: if channelMap has exactly one entry and decoder
            // has exactly one required channel, skip the missing check
            int required_ch_count = 0;
            for (const GSList *c = dec->channels; c; c = c->next)
                required_ch_count++;
            bool auto_map = (channel_map.size() == 1 && required_ch_count == 1);

            if (!missing.empty() && !auto_map) {
                return Result<std::string>::Fail(
                    ErrorCode::DecoderError,
                    "Required channel(s) not mapped: " + missing +
                    ". Please provide a channelMap with all required channels.");
            }
        } else if (dec->channels) {
            // channel_map is empty but decoder has required channels
            int required_ch_count = 0;
            for (const GSList *c = dec->channels; c; c = c->next)
                required_ch_count++;
            if (required_ch_count > 0) {
                std::string missing;
                for (const GSList *c = dec->channels; c; c = c->next) {
                    auto *ch = static_cast<srd_channel*>(c->data);
                    if (!missing.empty()) missing += ", ";
                    missing += ch->id ? ch->id : (ch->name ? ch->name : "?");
                }
                return Result<std::string>::Fail(
                    ErrorCode::DecoderError,
                    "Required channel(s) not mapped: " + missing +
                    ". Please provide a channelMap with all required channels.");
            }
        }

        // Do NOT call processEvents() or wait for _copy_in_progress here.
        // Calling processEvents() while inside do_add() on the main thread
        // causes a crash: it processes DSV_MSG_COPY_TO_DOC_DONE which calls
        // add_decode_task(), starting a decode thread that emits
        // new_decode_data() signals. The main thread is still inside do_add()
        // (e.g., in rebuild_decoder_pannel), causing a race in Qt's signal
        // delivery mechanism (crash in Qt6Core.dll).
        //
        // Instead, if copy is in progress, we just set up the decoder and
        // let DSV_MSG_COPY_TO_DOC_DONE start the decode task for us.
        // If copy is NOT in progress, we defer the decode start to after
        // do_add() returns using QTimer::singleShot.

        view::Trace *out_trace = nullptr;
        std::list<pv::data::decode::Decoder *> sub_decoders;
        // DecoderStatus must be heap-allocated; DecoderStack stores the pointer
        // and uses it for the lifetime of the decode trace.
        DecoderStatus *dstatus = new DecoderStatus();
        dstatus->m_format = (int)DecoderDataFormat::hex;

        bool ok = _session->add_decoder(dec, true, dstatus, sub_decoders,
                                        out_trace);

        if (!ok)
            return Result<std::string>::Fail(ErrorCode::DecoderError,
                                             "Failed to add decoder");

        if (!out_trace)
            return Result<std::string>::Fail(ErrorCode::DecoderError,
                                             "No trace created");

        auto *decode_trace = static_cast<view::DecodeTrace*>(out_trace);
        auto *decoder_stack = decode_trace->decoder();

        if (!label.empty()) {
            decode_trace->set_name(QString::fromStdString(label));
        }

        if (decoder_stack) {
            auto &stack = decoder_stack->stack();
            if (!stack.empty()) {
                auto *root_decoder = stack.front();

                // Apply options with correct GVariant types (matching DecoderOptions binding)
                for (const auto &opt : options) {
                    GVariant *val = nullptr;
                    bool found_type = false;

                    for (const GSList *o = dec->options; o; o = o->next) {
                        auto *opt_def = static_cast<srd_decoder_option*>(o->data);
                        if (!opt_def || !opt_def->id) continue;
                        if (opt.first != opt_def->id) continue;

                        if (opt_def->values) {
                            // Enum type: find matching value in the values list
                            // Create a NEW GVariant instead of reusing the shared one from values list,
                            // to avoid floating reference issues with shared GVariant pointers.
                            for (const GSList *v = opt_def->values; v; v = v->next) {
                                auto *enum_val = static_cast<GVariant*>(v->data);
                                if (!enum_val) continue;
                                // Compare by string representation
                                gchar *enum_str = g_variant_print(enum_val, false);
                                std::string cmp_str = enum_str ? enum_str : "";
                                g_free(enum_str);
                                // Strip quotes from string variants for comparison
                                if (cmp_str.size() >= 2 && cmp_str.front() == '\'' && cmp_str.back() == '\'')
                                    cmp_str = cmp_str.substr(1, cmp_str.size() - 2);
                                if (cmp_str == opt.second) {
                                    // Create a new GVariant of the same type instead of
                                    // reusing the shared one from the values list.
                                    // The values list GVariants may be floating references
                                    // (C decoders use g_variant_new_*() which returns floats),
                                    // and sharing them causes refcount corruption.
                                    if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("s")))
                                        val = g_variant_new_string(g_variant_get_string(enum_val, nullptr));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("t")))
                                        val = g_variant_new_uint64(g_variant_get_uint64(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("x")))
                                        val = g_variant_new_int64(g_variant_get_int64(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("d")))
                                        val = g_variant_new_double(g_variant_get_double(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("b")))
                                        val = g_variant_new_boolean(g_variant_get_boolean(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("y")))
                                        val = g_variant_new_byte(g_variant_get_byte(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("n")))
                                        val = g_variant_new_int16(g_variant_get_int16(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("q")))
                                        val = g_variant_new_uint16(g_variant_get_uint16(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("i")))
                                        val = g_variant_new_int32(g_variant_get_int32(enum_val));
                                    else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("u")))
                                        val = g_variant_new_uint32(g_variant_get_uint32(enum_val));
                                    else
                                        val = g_variant_new_string(opt.second.c_str());
                                    break;
                                }
                            }
                            // If no match found, try creating from the default value type
                            if (!val && opt_def->def) {
                                if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("s")))
                                    val = g_variant_new_string(opt.second.c_str());
                                else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("d")))
                                    val = g_variant_new_double(std::stod(opt.second));
                                else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("x")))
                                    val = g_variant_new_int64(std::stoll(opt.second));
                                else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("b")))
                                    val = g_variant_new_boolean(opt.second == "True" || opt.second == "1");
                                else
                                    val = g_variant_new_string(opt.second.c_str());
                            }
                            if (!val)
                                val = g_variant_new_string(opt.second.c_str());
                        } else if (opt_def->def) {
                            if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("d"))) {
                                val = g_variant_new_double(std::stod(opt.second));
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("x"))) {
                                val = g_variant_new_int64(std::stoll(opt.second));
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("s"))) {
                                val = g_variant_new_string(opt.second.c_str());
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("b"))) {
                                val = g_variant_new_boolean(opt.second == "True" || opt.second == "1");
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("y"))) {
                                val = g_variant_new_byte(static_cast<guchar>(std::stoi(opt.second)));
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("n"))) {
                                val = g_variant_new_int16(static_cast<gint16>(std::stoi(opt.second)));
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("q"))) {
                                val = g_variant_new_uint16(static_cast<guint16>(std::stoi(opt.second)));
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("i"))) {
                                val = g_variant_new_int32(std::stoi(opt.second));
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("u"))) {
                                val = g_variant_new_uint32(static_cast<guint32>(std::stoul(opt.second)));
                            } else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("t"))) {
                                val = g_variant_new_uint64(std::stoull(opt.second));
                            } else {
                                val = g_variant_new_string(opt.second.c_str());
                            }
                        } else {
                            val = g_variant_new_string(opt.second.c_str());
                        }
                        found_type = true;
                        break;
                    }

                    if (!found_type) {
                        val = g_variant_new_string(opt.second.c_str());
                    }

                    root_decoder->set_option(opt.first.c_str(), val);
                }

                // Build channel map from provided map.
                // Match channel keys against srd_channel id, name, and desc
                // (case-insensitive) so MCP clients can use any of them.
                // If channelMap has only one entry and the decoder has only one
                // required channel, auto-map regardless of key name.
                auto match_channel = [&channel_map](const srd_channel *ch) -> std::pair<bool, int16_t> {
                    if (!ch) return {false, 0};
                    std::string ch_id = ch->id ? ch->id : "";
                    std::string ch_name = ch->name ? ch->name : "";
                    std::string ch_desc = ch->desc ? ch->desc : "";

                    // Helper: case-insensitive compare
                    auto ci_eq = [](const std::string& a, const std::string& b) {
                        if (a.size() != b.size()) return false;
                        for (size_t i = 0; i < a.size(); i++)
                            if (tolower(a[i]) != tolower(b[i])) return false;
                        return true;
                    };

                    for (const auto& [key, val] : channel_map) {
                        if (ci_eq(key, ch_id) || ci_eq(key, ch_name) || ci_eq(key, ch_desc))
                            return {true, val};
                    }
                    return {false, 0};
                };

                std::map<const srd_channel*, int> probes;
                std::list<int> index_list;

                // Count required channels
                int required_ch_count = 0;
                for (const GSList *c = dec->channels; c; c = c->next)
                    required_ch_count++;

                // Auto-map: if channelMap has exactly one entry and decoder
                // has exactly one required channel, map regardless of key name
                bool auto_map = (channel_map.size() == 1 && required_ch_count == 1);

                for (const GSList *c = dec->channels; c; c = c->next) {
                    auto *ch = static_cast<srd_channel*>(c->data);
                    auto [found, val] = match_channel(ch);
                    if (!found && auto_map) {
                        // Auto-map: use the single channelMap value
                        val = channel_map.begin()->second;
                        found = true;
                    }
                    if (found) {
                        probes[ch] = val;
                        index_list.push_back(val);
                    }
                }

                for (const GSList *c = dec->opt_channels; c; c = c->next) {
                    auto *ch = static_cast<srd_channel*>(c->data);
                    auto [found, val] = match_channel(ch);
                    if (found) {
                        probes[ch] = val;
                        index_list.push_back(val);
                    }
                }

                root_decoder->set_probes(probes);

                if (!index_list.empty()) {
                    decode_trace->set_index_list(index_list);
                }

                decoder_stack->set_options_changed(true);

                // Note: We do NOT need to set decode_region here.
                // When silent=true, create_popup() is skipped, so
                // Decoder::_decode_end remains 0 (the default).
                // execute_decode_stack() now handles this case: if
                // dec->decode_end() is 0, it uses _sample_count - 1
                // (the full data range) automatically.

                // Debug: log channel mapping result
                {
                    QString probe_info;
                    for (const auto& [ch, idx] : probes) {
                        probe_info += QString("  %1(id=%2) -> ch%3, ")
                            .arg(ch->name ? ch->name : "?")
                            .arg(ch->id ? ch->id : "?")
                            .arg(idx);
                    }
                    pxv_info("add_decoder channel mapping: %s probes=%d index_list=%d have_view_data=%d",
                             probe_info.toUtf8().constData(),
                             (int)probes.size(), (int)index_list.size(),
                             _session->have_view_data() ? 1 : 0);
                }
            }

            // Prepare decode parameters but do NOT start the decode task here.
            // Starting the decode task while do_add() is still running on the
            // main thread causes a crash: the decode thread emits
            // new_decode_data() signals via queued connection, but the main
            // thread is still inside do_add() (e.g., in rebuild_decoder_pannel
            // or processEvents), leading to a race in Qt's signal delivery
            // (crash in Qt6Core.dll at QMetaObject::activate).
            //
            // Three cases:
            // 1. No view data yet (added before capture): Just set
            //    options_changed. When capture completes, DSV_MSG_REV_END_PACKET
            //    → copy_data_to_document → DSV_MSG_COPY_TO_DOC_DONE will
            //    automatically call frame_ended() + add_decode_task() for this
            //    decoder.
            // 2. Copy in progress (added during capture): Same as case 1 —
            //    DSV_MSG_COPY_TO_DOC_DONE will start the decode for us.
            // 3. Data is ready (added after capture): Set capture_end_flag and
            //    frame_ended(), then defer add_decode_task() to after do_add()
            //    returns via QTimer::singleShot.
            bool copy_in_progress = _session->is_copy_in_progress();
            if (!_session->have_view_data() || copy_in_progress) {
                // No data yet or copy in progress — the capture pipeline will
                // start the decode for us when data is ready.
                decoder_stack->set_options_changed(true);
            } else {
                // Data is ready — prepare decode but don't start yet.
                decoder_stack->set_capture_end_flag(true);
                decoder_stack->frame_ended();
            }
        }

        // Update the ProtocolDock so the new decoder appears in the GUI
        _session->rebuild_decoder_pannel();

        std::string instance_id =
            std::to_string(reinterpret_cast<intptr_t>(out_trace));

        broadcast_event(ServiceEvent::DecoderAdded,
                        {{"instance_id", instance_id},
                         {"decoder_id", dec->id ? dec->id : ""}});

        return Result<std::string>::Success(instance_id);
    };

    Result<std::string> result = Result<std::string>::Fail(
        ErrorCode::InternalError, "Pending");

    if (QThread::currentThread() == qApp->thread()) {
        // Already on the main thread, execute directly
        result = do_add();
    } else {
        // Dispatch to the main thread and wait synchronously
        std::mutex result_mutex;
        std::condition_variable result_cv;
        bool done = false;

        QMetaObject::invokeMethod(qApp, [&do_add, &result, &result_mutex, &result_cv, &done]() {
            result = do_add();
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                done = true;
            }
            result_cv.notify_one();
        }, Qt::BlockingQueuedConnection);

        {
            std::unique_lock<std::mutex> lock(result_mutex);
            result_cv.wait(lock, [&done]() { return done; });
        }
    }

    if (!result.ok())
        return result;

    // Now that do_add() has returned and the main thread is free,
    // start the decode task ONLY if copy was not in progress.
    // If copy was in progress, DSV_MSG_COPY_TO_DOC_DONE will start
    // the decode for us — we must not start it ourselves or we'll
    // get a duplicate decode task.
    {
        std::string instance_id = result.value();
        auto *decode_trace = reinterpret_cast<view::DecodeTrace*>(
            std::stoll(instance_id));
        auto *decoder_stack = decode_trace->decoder();

        // Only start decode if copy is NOT in progress.
        // If copy is in progress, DSV_MSG_COPY_TO_DOC_DONE handler
        // will iterate decode_traces() and start decode for us.
        if (decoder_stack && decoder_stack->options_changed() &&
            _session->have_view_data() &&
            !_session->is_copy_in_progress()) {
            // Use QTimer::singleShot(0, ...) to defer the decode start
            // to the next event loop iteration, after all pending events
            // (including the DecoderAdded broadcast) have been processed.
            QTimer::singleShot(0, qApp, [this, decode_trace]() {
                if (decode_trace && !decode_trace->_delete_flag) {
                    _session->add_decode_task(decode_trace);
                }
            });
        }
    }

    // Wait for decoder completion if requested
    if (wait_for_completion && result.ok()) {
        std::string instance_id = result.value();
        auto *decode_trace = reinterpret_cast<view::DecodeTrace*>(
            std::stoll(instance_id));
        auto *decoder_stack = decode_trace->decoder();

        if (decoder_stack) {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

            // First, wait for copy_data_to_document to complete if it's
            // still in progress. The DSV_MSG_COPY_TO_DOC_DONE handler will
            // start the decode task for our new decoder.
            {
                int wait_count = 0;
                while (_session->is_copy_in_progress() && wait_count < 200) {
                    timer.start(100);
                    loop.exec();
                    wait_count++;
                }
            }

            // Wait for decode to start
            int wait_count = 0;
            while (!decoder_stack->IsRunning() && wait_count < 50) {
                timer.start(100);
                loop.exec();
                wait_count++;
                if (decoder_stack->get_progress() >= 100)
                    break;
            }

            // Poll until decode completes
            while (decoder_stack->IsRunning()) {
                timer.start(100);
                loop.exec();

                if (!decoder_stack->error_message().isEmpty()) {
                    // Remove decoder with error on main thread
                    QMetaObject::invokeMethod(qApp, [this, decode_trace]() {
                        auto &traces = _session->get_decode_signals();
                        for (size_t i = 0; i < traces.size(); i++) {
                            if (traces[i] == decode_trace) {
                                _session->remove_decoder(static_cast<int>(i));
                                break;
                            }
                        }
                    }, Qt::BlockingQueuedConnection);
                    return Result<std::string>::Fail(
                        ErrorCode::DecoderError,
                        decoder_stack->error_message().toStdString());
                }
            }

            if (decoder_stack->get_progress() < 100) {
                if (!decoder_stack->error_message().isEmpty()) {
                    QMetaObject::invokeMethod(qApp, [this, decode_trace]() {
                        auto &traces = _session->get_decode_signals();
                        for (size_t i = 0; i < traces.size(); i++) {
                            if (traces[i] == decode_trace) {
                                _session->remove_decoder(static_cast<int>(i));
                                break;
                            }
                        }
                    }, Qt::BlockingQueuedConnection);
                    return Result<std::string>::Fail(
                        ErrorCode::DecoderError,
                        decoder_stack->error_message().toStdString());
                }
            }
        }
    }

    return result;
}

Result<void> SessionService::remove_decoder(const std::string &instance_id) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    // remove_decoder modifies Qt objects and triggers signals,
    // so it MUST run on the main thread.

    auto do_remove = [this, &instance_id]() -> Result<void> {
        auto &traces = _session->get_decode_signals();
        for (size_t i = 0; i < traces.size(); i++) {
            auto *trace = traces[i];
            if (!trace)
                continue;

            std::string tid =
                std::to_string(reinterpret_cast<intptr_t>(trace));
            if (tid == instance_id) {
                _session->remove_decoder(static_cast<int>(i));

                broadcast_event(ServiceEvent::DecoderRemoved,
                                {{"instance_id", instance_id}});
                return Result<void>::Success();
            }
        }
        return Result<void>::Fail(ErrorCode::DecoderNotFound,
                                  "Decoder instance not found");
    };

    if (QThread::currentThread() == qApp->thread()) {
        return do_remove();
    }

    std::mutex result_mutex;
    std::condition_variable result_cv;
    bool done = false;
    Result<void> result = Result<void>::Fail(
        ErrorCode::InternalError, "Pending");

    QMetaObject::invokeMethod(qApp, [&do_remove, &result, &result_mutex, &result_cv, &done]() {
        result = do_remove();
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            done = true;
        }
        result_cv.notify_one();
    }, Qt::BlockingQueuedConnection);

    {
        std::unique_lock<std::mutex> lock(result_mutex);
        result_cv.wait(lock, [&done]() { return done; });
    }

    return result;
}

Result<void> SessionService::clear_all_decoders() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    // Pass bUpdateView=false to avoid triggering signals_changed() callback
    // which can crash when invoked from the MCP context
    _session->clear_all_decoder(false);
    // Rebuild the protocol dock UI to remove stale layer items
    _session->rebuild_decoder_pannel();
    return Result<void>::Success();
}

// ===========================================================================
// 13. Decoder results
// ===========================================================================

Result<std::vector<DecoderAnnotation>> SessionService::get_decoder_annotations(
    const std::string &instance_id, uint64_t start_sample,
    uint64_t end_sample, int max_count) {
    if (!_session)
        return Result<std::vector<DecoderAnnotation>>::Fail(
            ErrorCode::InternalError, "Session is null");

    // Find the decoder trace by instance_id
    auto &traces = _session->get_decode_signals();

    // MCP debug
    {
        static QFile s_dbg;
        if (!s_dbg.isOpen()) {
            s_dbg.setFileName(QDir::tempPath() + "/pxview_mcp_debug.log");
            (void)s_dbg.open(QIODevice::WriteOnly | QIODevice::Append);
        }
        if (s_dbg.isOpen()) {
            QString msg = QString("get_decoder_annotations: instance_id='%1', traces.size()=%2\n")
                .arg(QString::fromStdString(instance_id))
                .arg(traces.size());
            s_dbg.write(msg.toUtf8());
            for (auto *trace : traces) {
                std::string tid = std::to_string(reinterpret_cast<intptr_t>(trace));
                msg = QString("  trace: ptr=%1, tid='%2'\n")
                    .arg(reinterpret_cast<quintptr>(trace))
                    .arg(QString::fromStdString(tid));
                s_dbg.write(msg.toUtf8());
            }
            s_dbg.flush();
        }
    }

    view::DecodeTrace *target_trace = nullptr;
    for (auto *trace : traces) {
        if (!trace)
            continue;
        std::string tid =
            std::to_string(reinterpret_cast<intptr_t>(trace));
        if (tid == instance_id) {
            target_trace = trace;
            break;
        }
    }

    if (!target_trace)
        return Result<std::vector<DecoderAnnotation>>::Fail(
            ErrorCode::DecoderNotFound, "Decoder instance not found");

    auto *decoder_stack = target_trace->decoder();
    if (!decoder_stack)
        return Result<std::vector<DecoderAnnotation>>::Fail(
            ErrorCode::DecoderNotFound, "Decoder stack is null");

    std::vector<DecoderAnnotation> result;
    int row_count = decoder_stack->list_rows_size();

    for (int row = 0; row < row_count; row++) {
        uint64_t ann_count = decoder_stack->list_annotation_size(
            static_cast<uint16_t>(row));

        for (uint64_t col = 0; col < ann_count && result.size() < static_cast<size_t>(max_count); col++) {
            decode::Annotation ann;
            if (!decoder_stack->list_annotation(&ann,
                    static_cast<uint16_t>(row), col))
                continue;

            if (ann.start_sample() > end_sample || ann.end_sample() < start_sample)
                continue;

            DecoderAnnotation da;
            da.start_sample = ann.start_sample();
            da.end_sample = ann.end_sample();
            da.ann_class = ann.type();

            const auto &texts = ann.annotations();
            da.texts.reserve(texts.size());
            for (const auto &text : texts)
                da.texts.push_back(text.toStdString());

            result.push_back(da);
        }
    }

    return Result<std::vector<DecoderAnnotation>>::Success(result);
}

// ===========================================================================
// 14. Measurements
// ===========================================================================

std::vector<MeasurementValue> SessionService::get_measurements() const {
    if (!_view)
        return {};

    std::vector<MeasurementValue> result;
    const char *options[] = {"width", "period", "frequency", "duty"};

    for (const char *opt : options) {
        QString val_str = _view->get_measure(QString(opt));
        if (val_str.isEmpty())
            continue;

        MeasurementValue mv;
        mv.valid = true;

        if (strcmp(opt, "width") == 0) mv.type = 0;
        else if (strcmp(opt, "period") == 0) mv.type = 1;
        else if (strcmp(opt, "frequency") == 0) mv.type = 2;
        else if (strcmp(opt, "duty") == 0) mv.type = 3;

        bool ok = false;
        mv.value = val_str.toDouble(&ok);
        if (!ok)
            mv.valid = false;

        // Try to extract unit from the string (e.g., "1.5 kHz")
        QString unit_part;
        for (int i = 0; i < val_str.size(); i++) {
            if (!val_str[i].isDigit() && val_str[i] != '.' &&
                val_str[i] != '-' && val_str[i] != 'e' && val_str[i] != 'E' &&
                val_str[i] != '+') {
                unit_part = val_str.mid(i).trimmed();
                break;
            }
        }
        mv.unit = unit_part.toStdString();

        result.push_back(mv);
    }

    return result;
}

// ===========================================================================
// 15. Cursors
// ===========================================================================

std::vector<CursorInfo> SessionService::get_cursors() const {
    if (!_view)
        return {};

    std::vector<CursorInfo> result;
    auto &cursor_list = _view->get_cursorList();
    uint64_t samplerate = _session ? _session->cur_samplerate() : 0;

    int idx = 0;
    for (auto *cursor : cursor_list) {
        if (!cursor)
            continue;

        CursorInfo info;
        info.index = idx;
        info.sample_pos = static_cast<int64_t>(cursor->index());
        info.time_sec = samplerate > 0
            ? static_cast<double>(cursor->index()) / samplerate
            : 0.0;

        result.push_back(info);
        idx++;
    }

    return result;
}

Result<void> SessionService::add_cursor(uint64_t sample_pos) {
    if (!_view)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "View is not set");

    _view->add_cursor(sample_pos);
    return Result<void>::Success();
}

Result<void> SessionService::remove_cursor(int index) {
    if (!_view)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "View is not set");

    auto &cursor_list = _view->get_cursorList();
    int idx = 0;
    for (auto it = cursor_list.begin(); it != cursor_list.end(); ++it, ++idx) {
        if (idx == index && *it) {
            _view->del_cursor(*it);
            return Result<void>::Success();
        }
    }

    return Result<void>::Fail(ErrorCode::InvalidRequest,
                              "Cursor index not found");
}

Result<void> SessionService::clear_cursors() {
    if (!_view)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "View is not set");

    _view->clear_cursors();
    return Result<void>::Success();
}

// ===========================================================================
// 16. Signal processing
// ===========================================================================

Result<void> SessionService::set_glitch_filter(const GlitchFilterConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    std::vector<uint32_t> thresholds;
    std::vector<::GlitchFilterMode> modes;

    thresholds.reserve(config.channels.size());
    modes.reserve(config.modes.size());

    for (size_t i = 0; i < config.channels.size() && i < config.thresholds.size(); i++) {
        thresholds.push_back(static_cast<uint32_t>(config.thresholds[i]));
    }
    for (size_t i = 0; i < config.modes.size(); i++) {
        switch (config.modes[i]) {
        case GlitchFilterMode::Both:
            modes.push_back(GLITCH_FILTER_BOTH);
            break;
        case GlitchFilterMode::High:
            modes.push_back(GLITCH_FILTER_HIGH);
            break;
        case GlitchFilterMode::Low:
            modes.push_back(GLITCH_FILTER_LOW);
            break;
        }
    }

    _session->set_glitch_filter(thresholds, modes);
    return Result<void>::Success();
}

Result<void> SessionService::clear_glitch_filter() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    _session->clear_glitch_filter();
    return Result<void>::Success();
}

GlitchFilterConfig SessionService::get_glitch_filter_config() const {
    GlitchFilterConfig config;
    if (!_session)
        return config;

    // SigSession does not expose the thresholds/modes through a public
    // getter. Return the active state only; detailed config requires
    // extending SigSession's public API.
    if (_session->is_glitch_filter_active()) {
        // Placeholder: indicate the filter is active but thresholds
        // are not accessible through the current public API.
        config.thresholds.push_back(0);
        config.modes.push_back(GlitchFilterMode::Both);
    }

    return config;
}

Result<void> SessionService::set_signal_invert(const SignalInvertConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    std::vector<bool> channels;
    channels.reserve(config.channels.size());
    for (size_t i = 0; i < config.channels.size() && i < config.invert_states.size(); i++) {
        channels.push_back(config.invert_states[i]);
    }

    _session->set_signal_invert(channels);
    return Result<void>::Success();
}

Result<void> SessionService::clear_signal_invert() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    _session->clear_signal_invert();
    return Result<void>::Success();
}

SignalInvertConfig SessionService::get_signal_invert_config() const {
    SignalInvertConfig config;
    if (!_session)
        return config;

    // SigSession does not expose the invert channel list through a
    // public getter. Return the active state only; detailed config
    // requires extending SigSession's public API.
    if (_session->is_signal_invert_active()) {
        config.invert_states.push_back(true);
    }

    return config;
}

// ===========================================================================
// 17. Disk cache
// ===========================================================================

DiskCacheInfo SessionService::get_disk_cache_info() const {
    DiskCacheInfo info;
    if (!_session)
        return info;

    info.enabled = true; // Disk cache is always available
    info.write_speed_mbps = _session->get_disk_write_speed_mbps();
    info.write_queue_depth =
        static_cast<int32_t>(_session->get_disk_write_queue_depth());
    info.is_disk_full = _session->is_disk_write_disk_full();

    return info;
}

// ===========================================================================
// 18. File operations
// ===========================================================================

Result<void> SessionService::load_file(const std::string &path) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    bool ok = _session->set_file(QString::fromStdString(path));
    if (!ok)
        return Result<void>::Fail(ErrorCode::LoadFailed,
                                  "Failed to load file: " + path);
    return Result<void>::Success();
}

Result<void> SessionService::save_file(const std::string &path) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    StoreSession store(_session);
    store._sessionDataGetter = nullptr;
    store.SetFileName(QString::fromStdString(path));
    bool ok = store.save_start();
    if (!ok)
        return Result<void>::Fail(ErrorCode::SaveFailed,
                                  "Failed to save file: " + path);

    store.wait();
    return Result<void>::Success();
}

Result<void> SessionService::export_data(const ExportConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    StoreSession store(_session);
    store._sessionDataGetter = nullptr;
    store.SetFileName(QString::fromStdString(config.output_path));
    store.SetDataRange(config.start_sample, config.end_sample);
    
    // Set specific channels and type for export
    store.set_export_channels(config.channels);
    store.set_export_channel_type(config.is_logic ? SR_CHANNEL_LOGIC : SR_CHANNEL_ANALOG);

    // Apply analog downsample ratio if > 1
    if (config.analog_downsample_ratio > 1) {
        store.set_analog_downsample_ratio(config.analog_downsample_ratio);
    }

    // Enable ISO8601 timestamp formatting if requested
    if (config.iso8601_timestamp) {
        store.set_iso8601_timestamp(true);
    }

    bool ok = store.export_start();
    if (!ok)
        return Result<void>::Fail(ErrorCode::ExportFailed,
                                  "Failed to export data");

    store.wait();
    return Result<void>::Success();
}

Result<void> SessionService::export_binary(const ExportConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    QString output_dir = QString::fromStdString(config.output_path);
    QDir dir(output_dir);
    if (!dir.exists()) {
        if (!dir.mkpath(output_dir))
            return Result<void>::Fail(ErrorCode::ExportFailed,
                                      "Failed to create output directory");
    }

    uint64_t start = config.start_sample;
    uint64_t end = config.end_sample;
    if (end == 0) {
        // Default to all data
        if (_session->get_logic_snapshot() && _session->get_logic_snapshot()->have_data())
            end = _session->get_logic_snapshot()->get_sample_count() - 1;
        else if (_session->get_analog_snapshot() && _session->get_analog_snapshot()->have_data())
            end = _session->cur_samplelimits() - 1;
        else if (_session->get_dso_snapshot() && _session->get_dso_snapshot()->have_data())
            end = _session->cur_samplelimits() - 1;
        if (end == 0)
            end = _session->cur_samplelimits() > 0 ? _session->cur_samplelimits() - 1 : 0;
    }

    // If no channels specified, export all enabled channels
    std::vector<int32_t> channels = config.channels;
    if (channels.empty()) {
        auto &sig_list = _session->get_signals();
        for (auto *sig : sig_list) {
            if (sig && sig->enabled())
                channels.push_back(sig->get_index());
        }
    }

    for (auto ch_idx : channels) {
        // Determine channel type
        auto &sig_list = _session->get_signals();
        ChannelType ch_type = ChannelType::Logic;
        for (auto *sig : sig_list) {
            if (sig && sig->get_index() == ch_idx) {
                ch_type = sr_channel_type_to_api(sig->signal_type());
                break;
            }
        }

        QString filename = output_dir + QString("/channel_%1.bin").arg(ch_idx);
        QFile file(filename);
        if (!file.open(QIODevice::WriteOnly))
            return Result<void>::Fail(ErrorCode::ExportFailed,
                                      "Failed to open file: " + filename.toStdString());

        if (ch_type == ChannelType::Logic) {
            auto *snapshot = _session->get_logic_snapshot();
            if (!snapshot || !snapshot->have_data())
                continue;

            uint64_t actual_end = end;
            const uint8_t *data = snapshot->get_samples(start, actual_end,
                                                         static_cast<int>(ch_idx));
            if (!data)
                continue;

            uint64_t count = actual_end - start + 1;
            // Logic: 1 bit per channel per sample, packed into bytes
            size_t byte_count = static_cast<size_t>((count + 7) / 8);
            file.write(reinterpret_cast<const char*>(data), byte_count);
        } else if (ch_type == ChannelType::Analog) {
            auto *snapshot = _session->get_analog_snapshot();
            if (!snapshot || !snapshot->have_data())
                continue;

            const uint8_t *raw = snapshot->get_samples(static_cast<int64_t>(start));
            if (!raw)
                continue;

            uint64_t count = end - start + 1;
            int pitch = snapshot->get_scale_factor();

            // Apply downsample ratio
            uint64_t step = config.analog_downsample_ratio > 1
                                ? config.analog_downsample_ratio : 1;

            for (uint64_t i = 0; i < count; i += step) {
                float val = static_cast<float>(raw[i * pitch + ch_idx]) / 255.0f;
                file.write(reinterpret_cast<const char*>(&val), sizeof(float));
            }
        } else if (ch_type == ChannelType::Dso) {
            auto *snapshot = _session->get_dso_snapshot();
            if (!snapshot || !snapshot->have_data())
                continue;

            const uint8_t *raw = snapshot->get_samples(
                static_cast<int64_t>(start),
                static_cast<int64_t>(end),
                static_cast<uint16_t>(ch_idx));
            if (!raw)
                continue;

            uint64_t count = end - start + 1;
            float data_scale = snapshot->get_data_scale(ch_idx);

            for (uint64_t i = 0; i < count; i++) {
                float val = static_cast<float>(raw[i]) * data_scale;
                file.write(reinterpret_cast<const char*>(&val), sizeof(float));
            }
        }

        file.close();
    }

    return Result<void>::Success();
}

Result<void> SessionService::export_decoder_table(
    const std::string &filepath,
    const std::vector<AnalyzerExportConfig> &analyzers,
    bool iso8601_timestamp) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    auto &traces = _session->get_decode_signals();
    if (traces.empty())
        return Result<void>::Fail(ErrorCode::NoData,
                                  "No active decoders");

    uint64_t samplerate = _session->cur_samplerate();

    // Determine which decoders to export
    std::vector<std::pair<view::DecodeTrace*, int>> selected;
    if (analyzers.empty()) {
        // Export all decoders
        for (size_t i = 0; i < traces.size(); i++) {
            if (traces[i])
                selected.push_back({traces[i], 4}); // default Ascii radix
        }
    } else {
        for (const auto &cfg : analyzers) {
            for (auto *trace : traces) {
                if (!trace) continue;
                std::string tid =
                    std::to_string(reinterpret_cast<intptr_t>(trace));
                if (tid == cfg.analyzer_id) {
                    selected.push_back({trace, cfg.radix_type});
                    break;
                }
            }
        }
    }

    if (selected.empty())
        return Result<void>::Fail(ErrorCode::DecoderNotFound,
                                  "No matching decoders found");

    QFile file(QString::fromStdString(filepath));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return Result<void>::Fail(ErrorCode::ExportFailed,
                                  "Failed to open file: " + filepath);

    QTextStream out(&file);

    // CSV header
    out << "start_sample,end_sample,analyzer_name,annotation_class,text\n";

    for (auto &[trace, radix] : selected) {
        auto *decoder_stack = trace->decoder();
        if (!decoder_stack)
            continue;

        std::string analyzer_name = trace->get_name().toStdString();
        int row_count = decoder_stack->list_rows_size();

        for (int row = 0; row < row_count; row++) {
            uint64_t ann_count = decoder_stack->list_annotation_size(
                static_cast<uint16_t>(row));

            for (uint64_t col = 0; col < ann_count; col++) {
                decode::Annotation ann;
                if (!decoder_stack->list_annotation(&ann,
                        static_cast<uint16_t>(row), col))
                    continue;

                // Format start/end sample
                QString start_str, end_str;
                if (iso8601_timestamp && samplerate > 0) {
                    double start_sec = static_cast<double>(ann.start_sample()) / samplerate;
                    double end_sec = static_cast<double>(ann.end_sample()) / samplerate;
                    auto start_dt = QDateTime::fromMSecsSinceEpoch(
                        static_cast<qint64>(start_sec * 1000), QTimeZone::UTC);
                    auto end_dt = QDateTime::fromMSecsSinceEpoch(
                        static_cast<qint64>(end_sec * 1000), QTimeZone::UTC);
                    start_str = start_dt.toString(Qt::ISODateWithMs);
                    end_str = end_dt.toString(Qt::ISODateWithMs);
                } else {
                    start_str = QString::number(ann.start_sample());
                    end_str = QString::number(ann.end_sample());
                }

                // Format annotation text with radix
                const auto &texts = ann.annotations();
                QString text;
                if (!texts.empty()) {
                    text = texts[0];
                    // Apply radix formatting for numeric values
                    if (ann.is_numberic() && radix != 4) {
                        bool ok = false;
                        qulonglong val = text.toULongLong(&ok, 0);
                        if (ok) {
                            switch (radix) {
                            case 1: text = "0b" + QString::number(val, 2); break;
                            case 2: text = QString::number(val, 10); break;
                            case 3: text = "0x" + QString::number(val, 16); break;
                            default: break;
                            }
                        }
                    }
                    // Escape CSV
                    text.replace("\"", "\"\"");
                }

                out << start_str << "," << end_str << ","
                    << QString::fromStdString(analyzer_name) << ","
                    << ann.type() << ","
                    << "\"" << text << "\"\n";
            }
        }
    }

    file.close();
    return Result<void>::Success();
}

// ===========================================================================
// 18b. MCP-specific file operations
// ===========================================================================

Result<void> SessionService::export_raw_data_csv(
    const std::string &directory,
    const std::vector<int32_t> &digital_channels,
    const std::vector<int32_t> &analog_channels,
    int analog_downsample_ratio,
    bool iso8601_timestamp) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    // Ensure output directory exists
    QDir dir(QString::fromStdString(directory));
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            return Result<void>::Fail(ErrorCode::ExportFailed,
                                      "Failed to create output directory");
        }
    }

    // Export digital channels as CSV
    for (int32_t ch : digital_channels) {
        ExportConfig config;
        config.output_path = directory + "/channel_" + std::to_string(ch) + ".csv";
        config.channels = {ch};
        config.is_logic = true;
        config.include_headers = true;
        config.analog_downsample_ratio = static_cast<uint64_t>(analog_downsample_ratio);
        config.iso8601_timestamp = iso8601_timestamp;

        auto r = export_data(config);
        if (!r)
            return r;
    }

    // Export analog channels as CSV
    for (int32_t ch : analog_channels) {
        ExportConfig config;
        config.output_path = directory + "/analog_" + std::to_string(ch) + ".csv";
        config.channels = {ch};
        config.is_logic = false;
        config.include_headers = true;
        config.analog_downsample_ratio = static_cast<uint64_t>(analog_downsample_ratio);
        config.iso8601_timestamp = iso8601_timestamp;

        auto r = export_data(config);
        if (!r)
            return r;
    }

    return Result<void>::Success();
}

Result<void> SessionService::export_raw_data_binary(
    const std::string &directory,
    const std::vector<int32_t> &digital_channels,
    const std::vector<int32_t> &analog_channels,
    int analog_downsample_ratio) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    ExportConfig config;
    config.analog_downsample_ratio = static_cast<uint64_t>(analog_downsample_ratio);

    // Combine all channels
    std::vector<int32_t> all_channels;
    all_channels.insert(all_channels.end(), digital_channels.begin(), digital_channels.end());
    all_channels.insert(all_channels.end(), analog_channels.begin(), analog_channels.end());

    if (all_channels.empty()) {
        // Default to all enabled channels
        auto &sig_list = _session->get_signals();
        for (auto *sig : sig_list) {
            if (sig && sig->enabled())
                all_channels.push_back(sig->get_index());
        }
    }

    config.channels = all_channels;
    config.output_path = directory;

    return export_binary(config);
}

Result<void> SessionService::export_data_table_csv(
    const std::string &filepath,
    const std::string &analyzer_id,
    int radix_type,
    bool iso8601_timestamp) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    std::vector<AnalyzerExportConfig> analyzers;
    if (!analyzer_id.empty()) {
        AnalyzerExportConfig cfg;
        cfg.analyzer_id = analyzer_id;
        cfg.radix_type = radix_type;
        analyzers.push_back(cfg);
    }

    return export_decoder_table(filepath, analyzers, iso8601_timestamp);
}

// ===========================================================================
// 19. View control
// ===========================================================================

Result<void> SessionService::show_region(uint64_t start_sample,
                                         uint64_t end_sample) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    _session->show_region(start_sample, end_sample, false);
    return Result<void>::Success();
}

Result<void> SessionService::zoom_fit() {
    if (!_view)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "View is not set");

    _view->auto_set_max_scale();
    return Result<void>::Success();
}

Result<void> SessionService::zoom_in() {
    if (!_view)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "View is not set");

    _view->zoom(1);
    return Result<void>::Success();
}

Result<void> SessionService::zoom_out() {
    if (!_view)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "View is not set");

    _view->zoom(-1);
    return Result<void>::Success();
}

// ===========================================================================
// 20. Spectrum/Lissajous/Math
// ===========================================================================

Result<void> SessionService::enable_spectrum(int16_t channel_index,
                                             bool enable) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    // Find the matching SpectrumTrace and toggle its enabled state
    auto &traces = _session->get_spectrum_traces();
    for (auto *trace : traces) {
        if (trace && trace->get_index() == channel_index) {
            trace->set_enable(enable);
            break;
        }
    }

    _session->spectrum_rebuild();
    return Result<void>::Success();
}

Result<void> SessionService::enable_lissajous(int16_t x_channel,
                                              int16_t y_channel,
                                              double percent) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    _session->lissajous_rebuild(true, x_channel, y_channel, percent);
    return Result<void>::Success();
}

Result<void> SessionService::disable_lissajous() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    _session->lissajous_disable();
    return Result<void>::Success();
}

Result<void> SessionService::enable_math(int16_t ch1, int16_t ch2,
                                         int math_type) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is null");

    // Look up DsoSignal pointers by channel index
    view::DsoSignal *sig1 = nullptr;
    view::DsoSignal *sig2 = nullptr;
    auto &sig_list = _session->get_signals();
    for (auto *sig : sig_list) {
        if (!sig) continue;
        if (sig->get_index() == ch1 && sig->signal_type() == SR_CHANNEL_DSO)
            sig1 = dynamic_cast<view::DsoSignal*>(sig);
        if (sig->get_index() == ch2 && sig->signal_type() == SR_CHANNEL_DSO)
            sig2 = dynamic_cast<view::DsoSignal*>(sig);
    }

    if (!sig1 || !sig2)
        return Result<void>::Fail(ErrorCode::ChannelNotFound,
                                  "DSO channel not found");

    auto type = static_cast<data::MathStack::MathType>(math_type);
    _session->math_rebuild(true, sig1, sig2, type);
    return Result<void>::Success();
}

// ===========================================================================
// 21. Event subscription
// ===========================================================================

void SessionService::add_event_listener(IServiceEventListener *listener) {
    std::lock_guard<std::mutex> lock(_listeners_mutex);
    if (listener) {
        auto it = std::find(_listeners.begin(), _listeners.end(), listener);
        if (it == _listeners.end())
            _listeners.push_back(listener);
    }
}

void SessionService::remove_event_listener(IServiceEventListener *listener) {
    std::lock_guard<std::mutex> lock(_listeners_mutex);
    auto it = std::find(_listeners.begin(), _listeners.end(), listener);
    if (it != _listeners.end())
        _listeners.erase(it);
}

// ===========================================================================
// ISessionCallback implementation
// ===========================================================================

void SessionService::session_error() {
    broadcast_event(ServiceEvent::ErrorOccurred,
                    {{"source", "session"}});
}

void SessionService::session_save() {
    // Internal save event, no direct ServiceEvent mapping
}

void SessionService::data_updated() {
    broadcast_event(ServiceEvent::DataUpdated);

    // Check for decode progress and emit DecodeProgress events
    if (_session) {
        auto &traces = _session->get_decode_signals();
        for (auto *trace : traces) {
            if (!trace) continue;
            auto *decoder_stack = trace->decoder();
            if (decoder_stack && decoder_stack->IsRunning()) {
                int progress = decoder_stack->get_progress();
                std::string instance_id =
                    std::to_string(reinterpret_cast<intptr_t>(trace));
                broadcast_event(ServiceEvent::DecodeProgress,
                                {{"instance_id", instance_id},
                                 {"progress", std::to_string(progress)}});
            }
        }
    }
}

void SessionService::update_capture() {
    // Include capture progress percentage
    int progress = 0;
    bool triggered = false;
    if (_session) {
        _session->get_capture_status(triggered, progress);
    }
    broadcast_event(ServiceEvent::CaptureProgress,
                    {{"progress", std::to_string(progress)}});
}

void SessionService::cur_snap_samplerate_changed() {
    broadcast_event(ServiceEvent::DataUpdated,
                    {{"detail", "samplerate_changed"}});
}

void SessionService::signals_changed() {
    broadcast_event(ServiceEvent::SignalsChanged);
}

void SessionService::receive_trigger(quint64 trigger_pos) {
    broadcast_event(ServiceEvent::TriggerReceived,
                    {{"position", std::to_string(trigger_pos)}});
}

void SessionService::frame_ended() {
    broadcast_event(ServiceEvent::FrameEnded);
}

void SessionService::frame_began() {
    broadcast_event(ServiceEvent::FrameBegan);
}

void SessionService::show_region(uint64_t start, uint64_t end, bool keep) {
    (void)keep;
    broadcast_event(ServiceEvent::DataUpdated,
                    {{"start", std::to_string(start)},
                     {"end", std::to_string(end)}});
}

void SessionService::show_wait_trigger() {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "waiting_trigger"}});
}

void SessionService::repeat_hold(int percent) {
    broadcast_event(ServiceEvent::CaptureProgress,
                    {{"percent", std::to_string(percent)}});
}

void SessionService::decode_done() {
    broadcast_event(ServiceEvent::DecodeDone);
}

void SessionService::receive_data_len(quint64 len) {
    broadcast_event(ServiceEvent::DataUpdated,
                    {{"data_length", std::to_string(len)}});
}

void SessionService::receive_header() {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "header_received"}});
}

void SessionService::trigger_message(int msg) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"trigger_msg", std::to_string(msg)}});
}

void SessionService::delay_prop_msg(QString strMsg) {
    broadcast_event(ServiceEvent::ErrorOccurred,
                    {{"message", strMsg.toStdString()}});
}

// ===========================================================================
// IMessageListener implementation
// ===========================================================================

void SessionService::OnMessage(int msg) {
    switch (msg) {
    // Collect lifecycle
    case DSV_MSG_START_COLLECT_WORK_PREV:
        broadcast_event(ServiceEvent::CaptureStateChanged,
                        {{"detail", "start_collect_prev"}});
        break;
    case DSV_MSG_START_COLLECT_WORK:
        broadcast_event(ServiceEvent::CaptureStateChanged,
                        {{"detail", "start_collect"}});
        break;
    case DSV_MSG_COLLECT_START:
        broadcast_event(ServiceEvent::CaptureStateChanged,
                        {{"detail", "collect_start"}});
        break;
    case DSV_MSG_COLLECT_END:
        broadcast_event(ServiceEvent::CaptureStateChanged,
                        {{"detail", "collect_end"}});
        break;
    case DSV_MSG_END_COLLECT_WORK_PREV:
        broadcast_event(ServiceEvent::CaptureStateChanged,
                        {{"detail", "end_collect_prev"}});
        break;
    case DSV_MSG_END_COLLECT_WORK:
        broadcast_event(ServiceEvent::CaptureStateChanged,
                        {{"detail", "end_collect"}});
        break;
    case DSV_MSG_REV_END_PACKET:
        broadcast_event(ServiceEvent::DataUpdated,
                        {{"detail", "end_packet"}});
        break;
    case DSV_MSG_CAPTURE_STATE_CHANGED:
        broadcast_event(ServiceEvent::CaptureStateChanged);
        break;

    // Device events
    case DSV_MSG_DEVICE_LIST_UPDATED:
        broadcast_event(ServiceEvent::DeviceListUpdated);
        break;
    case DSV_MSG_DEVICE_MODE_CHANGED:
        broadcast_event(ServiceEvent::DeviceModeChanged);
        break;
    case DSV_MSG_DEVICE_CONFIG_UPDATED:
        broadcast_event(ServiceEvent::DeviceConfigChanged);
        break;
    case DSV_MSG_CURRENT_DEVICE_DETACHED:
        broadcast_event(ServiceEvent::DeviceDetached);
        break;
    case DSV_MSG_NEW_USB_DEVICE:
        broadcast_event(ServiceEvent::NewUsbDevice);
        break;
    case DSV_MSG_CURRENT_DEVICE_CHANGED:
        broadcast_event(ServiceEvent::DeviceModeChanged,
                        {{"detail", "device_changed"}});
        break;
    case DSV_MSG_DEVICE_OPTIONS_UPDATED:
        broadcast_event(ServiceEvent::DeviceConfigChanged,
                        {{"detail", "options_updated"}});
        break;
    case DSV_MSG_DEVICE_DURATION_UPDATED:
        broadcast_event(ServiceEvent::DeviceConfigChanged,
                        {{"detail", "duration_updated"}});
        break;
    case DSV_MSG_COLLECT_MODE_CHANGED:
        broadcast_event(ServiceEvent::DeviceConfigChanged,
                        {{"detail", "collect_mode_changed"}});
        break;
    case DSV_MSG_DATA_POOL_CHANGED:
        broadcast_event(ServiceEvent::DataUpdated,
                        {{"detail", "data_pool_changed"}});
        break;
    case DSV_MSG_SIMPLE_TRIGGER_CHANGED:
        broadcast_event(ServiceEvent::DeviceConfigChanged,
                        {{"detail", "trigger_changed"}});
        break;

    // Glitch filter
    case DSV_MSG_GLITCH_FILTER_STARTED:
        broadcast_event(ServiceEvent::GlitchFilterStarted);
        break;
    case DSV_MSG_GLITCH_FILTER_PROGRESS:
        broadcast_event(ServiceEvent::GlitchFilterProgress);
        break;
    case DSV_MSG_GLITCH_FILTER_COMPLETED:
        broadcast_event(ServiceEvent::GlitchFilterCompleted);
        break;
    case DSV_MSG_GLITCH_FILTER_CLEARED:
        broadcast_event(ServiceEvent::GlitchFilterCleared);
        break;

    // Signal invert
    case DSV_MSG_SIGNAL_INVERT_STARTED:
        broadcast_event(ServiceEvent::SignalInvertStarted);
        break;
    case DSV_MSG_SIGNAL_INVERT_COMPLETED:
        broadcast_event(ServiceEvent::SignalInvertCompleted);
        break;
    case DSV_MSG_SIGNAL_INVERT_CLEARED:
        broadcast_event(ServiceEvent::SignalInvertCleared);
        break;

    // Copy / sample count
    case DSV_MSG_COPY_TO_DOC_DONE:
        broadcast_event(ServiceEvent::DataUpdated,
                        {{"detail", "copy_to_doc_done"}});
        break;
    case DSV_MSG_SAMPLE_COUNT_UPDATED:
        broadcast_event(ServiceEvent::DataUpdated,
                        {{"detail", "sample_count_updated"}});
        break;

    // Trigger & save
    case DSV_MSG_TRIG_NEXT_COLLECT:
        broadcast_event(ServiceEvent::TriggerReceived,
                        {{"detail", "next_collect"}});
        break;
    case DSV_MSG_SAVE_COMPLETE:
        broadcast_event(ServiceEvent::SaveComplete);
        break;
    case DSV_MSG_STORE_CONF_PREV:
        broadcast_event(ServiceEvent::SaveComplete,
                        {{"detail", "store_conf_prev"}});
        break;

    // Decode
    case DSV_MSG_CLEAR_DECODE_DATA:
        broadcast_event(ServiceEvent::DecodeDone,
                        {{"detail", "clear_decode_data"}});
        break;

    // App options
    case DSV_MSG_APP_OPTIONS_CHANGED:
    case DSV_MSG_FONT_OPTIONS_CHANGED:
    case DSV_MSG_SHORTCUT_CHANGED:
    case DSV_MSG_STYLE_CHANGED:
        // App-level events, not directly mapped to session events
        break;

    default:
        break;
    }
}

} // namespace api
} // namespace pv
