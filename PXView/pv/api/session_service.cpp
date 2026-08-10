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

#include "pv/api/session_service.h"

#include "pv/session/sigsession.h"
#include "pv/core/documentregistry.h"
#include "pv/core/eventbus.h"
#include "pv/session/deviceagent.h"
#include "pv/base/pxvdef.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/stack/mathstack.h"
#include "pv/data/stack/spectrumstack.h"
#include "pv/data/stack/lissajousmodel.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/annotation.h"
#include "pv/data/decode/row.h"
#include "pv/data/triggerconfig.h"
#include "pv/session/storesession.h"
#include "pv/base/log.h"

#include <libsigrok/libsigrok.h>
#include <libsigrokdecode/libsigrokdecode.h>

#include <QCoreApplication>
#include <QDebug>
#include <QColor>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <functional>

// Headless-mode ISessionDataGetter implementation.
// In GUI mode, MainWindow provides genSessionData() which serializes the full
// session config (device options, channel layout, trigger config, etc.).
// In headless mode there is no MainWindow, so we provide a minimal
// implementation that generates a basic JSON config with device info.
namespace {
class HeadlessSessionDataGetter : public ISessionDataGetter {
public:
    HeadlessSessionDataGetter(pv::SigSession *session, DeviceAgent *device)
        : _session(session), _device(device) {}

    bool genSessionData(std::string &str) override {
        if (!_session || !_device)
            return false;

        QJsonObject sessionVar;
        sessionVar["Version"] = QJsonValue::fromVariant(SESSION_FORMAT_VERSION);
        sessionVar["Device"] = QJsonValue::fromVariant(_device->driver_name());
        sessionVar["DeviceMode"] =
            QJsonValue::fromVariant(_device->get_work_mode());
        sessionVar["Language"] = 0;
        sessionVar["Title"] = QJsonValue::fromVariant(
            QString("PXView v") + QCoreApplication::applicationVersion());

        QJsonDocument doc(sessionVar);
        QString data = QString::fromUtf8(doc.toJson());
        str.append(data.toLocal8Bit().data());
        return true;
    }

private:
    pv::SigSession *_session;
    DeviceAgent *_device;
};
} // namespace

#ifdef WIN32
#include <windows.h>
// windows.h defines `interface` as a macro for COM interface declarations,
// which conflicts with the `pv::interface` namespace used by events.h. The
// events.h header undefs it at include time, but windows.h is included here
// AFTER session_service.h, re-defining the macro. Undef again so the
// on_event(const pv::interface::XxxPrev &) definitions below parse correctly.
#undef interface
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
// Cross-thread dispatch helper
// ---------------------------------------------------------------------------
//
// Several SessionService methods (add_decoder, remove_decoder, ...) touch
// QObject-derived types (DecoderStack, DecodeTrace, ...) and therefore must
// run on the Qt main thread. The original code dispatched the work via
// `QMetaObject::invokeMethod(qApp, lambda, Qt::QueuedConnection)` and waited
// on a `std::condition_variable`.
//
// That pattern DEADLOCKS when the caller is already on the main thread:
// `Qt::QueuedConnection` posts the lambda to the main thread's event queue,
// but the main thread is blocked inside `result_cv.wait()` and never processes
// the event loop, so the lambda never runs and the condition variable is
// never notified. This is exactly what happens for MCP requests: the QTcpServer
// owned by McpTransport lives on the main thread, so its `readyRead` signal
// (and therefore `on_add_analyzer`) runs on the main thread.
//
// The helpers below detect this case and invoke the lambda inline. Otherwise
// they fall back to the queued dispatch + condition_variable wait.
inline bool on_main_thread() {
    // Use EventBus::on_main_thread() (std::this_thread::get_id()) instead of
    // QThread::currentThread() — the latter creates a QThreadData on worker
    // threads, causing SIGSEGV on thread exit.
    return pv::core::EventBus::on_main_thread();
}

// Run a `Result<std::string>`-returning lambda on the main thread.
inline Result<std::string> run_string_on_main_thread(
    const std::function<Result<std::string>()>& fn) {
    if (on_main_thread())
        return fn();

    Result<std::string> result =
        Result<std::string>::Fail(ErrorCode::InternalError, "Pending");
    std::mutex result_mutex;
    std::condition_variable result_cv;
    bool done = false;

    // Use post_async_dispatch (QCoreApplication::postEvent) instead of
    // QMetaObject::invokeMethod to avoid creating QThreadData on worker threads.
    pv::core::EventBus::post_async_dispatch([&fn, &result, &result_mutex, &result_cv, &done]() {
        result = fn();
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            done = true;
        }
        result_cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lock(result_mutex);
        result_cv.wait(lock, [&done]() { return done; });
    }
    return result;
}

// Run a `Result<void>`-returning lambda on the main thread.
inline Result<void> run_void_on_main_thread(
    const std::function<Result<void>()>& fn) {
    if (on_main_thread())
        return fn();

    Result<void> result =
        Result<void>::Fail(ErrorCode::InternalError, "Pending");
    std::mutex result_mutex;
    std::condition_variable result_cv;
    bool done = false;

    pv::core::EventBus::post_async_dispatch([&fn, &result, &result_mutex, &result_cv, &done]() {
        result = fn();
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            done = true;
        }
        result_cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lock(result_mutex);
        result_cv.wait(lock, [&done]() { return done; });
    }
    return result;
}

// ---------------------------------------------------------------------------
// Generic main-thread dispatch helper
// ---------------------------------------------------------------------------
//
// invoke_or_call() invokes a callable on the Qt main thread. If the caller is
// already on the main thread, the callable runs inline (no event-loop posting,
// no blocking). Otherwise it is dispatched via Qt::BlockingQueuedConnection so
// the caller blocks until the callable returns.
//
// This replaces the ad-hoc `QMetaObject::invokeMethod(qApp, lambda,
// Qt::QueuedConnection)` + `std::condition_variable::wait()` pattern that
// DEADLOCKS when the caller is already on the main thread (the posted lambda
// can never run because the main thread is blocked waiting on the cv).
template <typename F>
inline void invoke_or_call(QObject *ctx, F &&fn) {
    if (on_main_thread()) {
        fn();
        return;
    }
    // Use post_async_dispatch + condition_variable instead of
    // QMetaObject::invokeMethod(BlockingQueuedConnection) — the latter creates
    // a QThreadData on the calling worker thread → SIGSEGV on thread exit.
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    pv::core::EventBus::post_async_dispatch([&fn, &m, &cv, &done]() {
        fn();
        {
            std::lock_guard<std::mutex> lock(m);
            done = true;
        }
        cv.notify_one();
    });
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&done]() { return done; });
}

// ---------------------------------------------------------------------------
// Decoder instance_id helpers (<handle_id>:<version>)
// ---------------------------------------------------------------------------
//
// Decoder stacks are identified to MCP/API callers by a stable
// "<handle_id>:<version>" string. The handle_id is allocated by SigSession
// when the stack is created and stays constant for the stack's lifetime; the
// version is bumped when the stack is rebuilt. This replaces the previous
// raw-pointer stringification which became stale whenever the stack vector
// reallocated or the stack was rebuilt, causing get_decoder_annotations /
// remove_decoder / export_decoder_table to fail to match the stack the caller
// was holding.

// Format a DecoderStack's stable instance identifier.
inline std::string make_instance_id(const pv::data::DecoderStack *stack) {
    if (!stack)
        return std::string("0:0");
    return std::to_string(stack->handle_id()) + ":" +
           std::to_string(stack->version());
}

// Parse a "<handle_id>:<version>" instance identifier. Returns false on
// malformed input (missing colon, empty fields, non-numeric values).
inline bool parse_instance_id(const std::string &instance_id,
                              uint64_t &handle_id, uint64_t &version) {
    auto pos = instance_id.find(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= instance_id.size())
        return false;
    try {
        size_t h_end = 0, v_end = 0;
        handle_id = std::stoull(instance_id.substr(0, pos), &h_end, 10);
        version = std::stoull(instance_id.substr(pos + 1), &v_end, 10);
        if (h_end != pos || v_end != instance_id.size() - pos - 1)
            return false;
    } catch (...) {
        return false;
    }
    return true;
}

// Find a decoder stack by instance_id ("<handle_id>:<version>") in a stacks
// vector. Returns nullptr if instance_id is malformed or no stack matches.
inline std::shared_ptr<pv::data::DecoderStack> find_stack_by_instance_id(
    std::vector<std::shared_ptr<pv::data::DecoderStack>> &stacks,
    const std::string &instance_id) {
    uint64_t handle_id = 0, version = 0;
    if (!parse_instance_id(instance_id, handle_id, version))
        return nullptr;
    for (auto &stack : stacks) {
        if (stack && stack->handle_id() == handle_id &&
            stack->version() == version)
            return stack;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SessionService::SessionService(SigSession *session, DeviceAgent *device)
    : _session(session), _device(device),
      _capture_id(0), _wait_capture_stop_flag(false) {
    // _api_doc_index defaults to SIZE_MAX via its in-class default initializer.
    // It is later injected via set_api_document() by AppService.
    if (_session) {
        // Spec v2 Task 7: add_callback removed, only IEventListener registration remains
        _session->add_event_listener(this);
    }
}

SessionService::~SessionService() {
    if (_session) {
        // Spec v2 Task 7: remove_callback removed
        _session->remove_event_listener(this);
    }
    // phase 2: release the MCP-dedicated document slot. Ownership is held by
    // DocumentRegistry, so release_document() frees the document (marked
    // deletion — slot stays, index stays stable). No manual unregister + delete.
    if (_api_doc_index != SIZE_MAX && _session && _session->document_registry()) {
        _session->document_registry()->release_document(_api_doc_index);
        _api_doc_index = SIZE_MAX;
    }
}

// ---------------------------------------------------------------------------
// MCP document injection
// ---------------------------------------------------------------------------

void SessionService::set_api_document(size_t doc_index) {
    // If a previous document was injected, release it first to avoid leaks.
    if (_api_doc_index != SIZE_MAX && _api_doc_index != doc_index &&
        _session && _session->document_registry()) {
        _session->document_registry()->release_document(_api_doc_index);
    }
    _api_doc_index = doc_index;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

pv::data::SessionDocument *SessionService::api_document() const {
    if (_api_doc_index == SIZE_MAX || !_session || !_session->document_registry())
        return nullptr;
    return _session->document_registry()->get_document_by_index(_api_doc_index);
}

bool SessionService::is_gui_mode() {
    // Detects whether we're running inside the full GUI (QApplication) or
    // a plain QCoreApplication (headless mode). Uses inherits() so that
    // QApplication.h does not need to be included here.
    QCoreApplication *app = QCoreApplication::instance();
    return app && app->inherits("QApplication");
}

void SessionService::broadcast_event(
ServiceEvent event, const std::map<std::string, std::string> &params) const {
std::lock_guard<std::mutex> lock(_listeners_mutex);
ServiceEventData data;
data.event = event;
data.params = params;
// P0-2: Versioned state — assign a monotonically increasing version number
// and a millisecond timestamp so WS/MCP clients can detect stale state
data.version = ++_state_version_counter;
data.timestamp_ms = QDateTime::currentMSecsSinceEpoch();
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
        pxv_info("sr_channel_type_to_api: unknown SR channel type %d", sr_type);
        return ChannelType::Unknown;
    }
}

// ===========================================================================
// configure_and_start helper methods (split from 470-line function)
// ===========================================================================

void SessionService::ensure_logic_mode_for_digital(
    const std::vector<int16_t>& digital_channels) {
    int cur_mode = _device->get_work_mode();

    GSList *channels = _device->get_channels();
    int ch_count = 0;
    for (GSList *l = channels; l; l = l->next) ch_count++;

    // If we need digital channels but device isn't in LOGIC mode or has
    // too few channels, force switch to LOGIC mode.
    if (!digital_channels.empty() && (cur_mode != LOGIC || ch_count < 16)) {
        if (cur_mode != LOGIC) {
            _session->switch_work_mode(LOGIC);
        } else {
            // Device reports LOGIC mode but has too few channels.
            // Force a mode cycle to reinitialize.
            _device->set_work_mode(DSO);
            if (is_gui_mode()) QCoreApplication::processEvents();
            _device->set_work_mode(LOGIC);
            _session->init_signals();
        }
        if (is_gui_mode()) {
            QCoreApplication::processEvents();
            QCoreApplication::processEvents();
        }
    }
}

void SessionService::configure_capture_channels(
    const std::vector<int16_t>& digital_channels,
    const std::vector<int16_t>& analog_channels) {
    GSList *channels = _device->get_channels();

    if (digital_channels.empty() && analog_channels.empty()) {
        // No channel selection requested — keep driver defaults.
        return;
    }

    // Disable all channels first
    for (GSList *l = channels; l; l = l->next) {
        auto *ch = static_cast<sr_channel *>(l->data);
        if (ch && ch->enabled) {
            _device->enable_probe(ch->index, false);
        }
    }

    // Enable specified digital channels
    for (int16_t idx : digital_channels) {
        _device->enable_probe(idx, true);
    }

    // Enable specified analog channels
    for (int16_t idx : analog_channels) {
        _device->enable_probe(idx, true);
    }
}

void SessionService::apply_trigger_to_signal_models(
    int trigger_channel_index,
    const std::string& trigger_type,
    const std::vector<std::pair<int16_t, std::string>>& linked_channels) {
    if (trigger_channel_index < 0)
        return;

    auto &sigs = _session->get_signal_models();
    for (auto m : sigs) {
        if (!m || m->type() != SR_CHANNEL_LOGIC)
            continue;

        if (m->index() == trigger_channel_index) {
            int trig_type = pv::data::SignalModel::NONTRIG;
            if (trigger_type == "rising") trig_type = pv::data::SignalModel::POSTRIG;
            else if (trigger_type == "falling") trig_type = pv::data::SignalModel::NEGTRIG;
            else if (trigger_type == "pulse_high") trig_type = pv::data::SignalModel::HIGTRIG;
            else if (trigger_type == "pulse_low") trig_type = pv::data::SignalModel::LOWTRIG;
            else trig_type = pv::data::SignalModel::EDGTRIG;
            m->set_trig_type(trig_type);
        } else {
            bool is_linked = false;
            for (const auto &lc : linked_channels) {
                if (m->index() == lc.first) {
                    is_linked = true;
                    int trig_type = (lc.second == "high") ?
                        pv::data::SignalModel::HIGTRIG : pv::data::SignalModel::LOWTRIG;
                    m->set_trig_type(trig_type);
                    break;
                }
            }
            if (!is_linked) {
                m->set_trig_type(pv::data::SignalModel::NONTRIG);
            }
        }
    }
}

void SessionService::configure_device_options(
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
    const std::string& pattern) {
    // Set channel mode if specified (e.g. "Buffer", "Stream")
    if (!channel_mode.empty()) {
        _device->set_config_string(SR_CONF_CHANNEL_MODE, channel_mode.c_str());
    }

    // Set RLE if specified
    if (rle_enabled) {
        _device->set_config_bool(SR_CONF_RLE, true);
    }

    // Set disk cache and stream buffer sizes
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

    // Set threshold preset if specified (distinct from VTH raw voltage)
    if (!threshold_preset.empty()) {
        _device->set_config_string(SR_CONF_THRESHOLD, threshold_preset.c_str());
    }

    // Set operation mode if specified
    if (!operation_mode.empty()) {
        if (!_device->set_config_string(SR_CONF_OPERATION_MODE, operation_mode.c_str())) {
            std::string full_name;
            if (operation_mode == "Buffer" || operation_mode == "buffer")
                full_name = "Buffer Mode";
            else if (operation_mode == "Stream" || operation_mode == "stream")
                full_name = "Stream Mode";
            else if (operation_mode == "Internal test" ||
                     operation_mode == "internal_test" ||
                     operation_mode == "Internal Test")
                full_name = "Internal Test";
            if (!full_name.empty()) {
                _device->set_config_string(SR_CONF_OPERATION_MODE, full_name.c_str());
            }
        }
    }

    // Set demo logic pattern mode if specified
    if (!pattern.empty()) {
        _device->set_config_string(SR_CONF_PATTERN_MODE, pattern.c_str());
    }

    // Set buffer options if specified
    if (!buffer_options.empty()) {
        _device->set_config_string(SR_CONF_BUFFER_OPTIONS, buffer_options.c_str());
    }

    // Set digital filter if specified
    if (!digital_filter.empty()) {
        _device->set_config_string(SR_CONF_FILTER, digital_filter.c_str());
    }
}

void SessionService::configure_capture_timing(
    const std::string& capture_mode,
    double repeat_interval_seconds,
    int capture_ratio,
    double duration_seconds,
    uint64_t sample_count) {
    // Set capture mode
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

    // Set capture ratio (trigger position percentage) if specified
    if (capture_ratio >= 0 && capture_ratio <= 100) {
        _device->set_config_uint64(SR_CONF_CAPTURE_RATIO,
                                    static_cast<uint64_t>(capture_ratio));
    }

    // Set duration (sample limit) if specified.
    if (duration_seconds > 0.0) {
        uint64_t rate = _device->get_sample_rate();
        if (rate > 0) {
            uint64_t sample_limit = static_cast<uint64_t>(
                duration_seconds * static_cast<double>(rate));
            _device->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_limit);
        }
    } else if (sample_count > 0) {
        _device->set_config_uint64(SR_CONF_LIMIT_SAMPLES, sample_count);
    }
}

// ===========================================================================
// 1. Capture control
// ===========================================================================

Result<void> SessionService::start_capture(bool instant) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is nullptr");
    if (_session->is_working())
        return Result<void>::Fail(ErrorCode::CaptureInProgress,
                                 "Capture already in progress");

    bool ok = _session->start_capture(instant, api_document());
    if (!ok)
        return Result<void>::Fail(ErrorCode::DeviceError,
                                 "Failed to start capture");
    return Result<void>::Success();
}

Result<void> SessionService::stop_capture() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is nullptr");
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
                                 "Session is nullptr");

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
    case WorkMode::Mso:
        sr_mode = MSO;
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
                                 "Session is nullptr");

    bool ok = _session->re_start();
    if (!ok)
        return Result<void>::Fail(ErrorCode::DeviceError,
                                 "Failed to restart capture");
    return Result<void>::Success();
}

Result<void> SessionService::wait_capture_complete(uint64_t timeout_ms) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is nullptr");

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

    // Block until capture stops or timeout.
    //
    // CRITICAL: We must always use QEventLoop (not condition_variable) in
    // BOTH GUI and headless modes. EventBus::dispatch_to posts callbacks
    // (frame_ended, update_capture) to the main thread via
    // QCoreApplication::postEvent. If the main thread is blocked in
    // _wait_cv.wait_for(), those posted events never get processed,
    // _wait_cv.notify_all() is never called, and the wait deadlocks.
    //
    // QEventLoop::exec() pumps the Qt event queue so posted callbacks
    // are delivered, while a check_timer polls the capture state directly.
    _wait_capture_stop_flag = false;

    {
        QEventLoop loop;
        QTimer timeout_timer;
        timeout_timer.setSingleShot(true);

        QObject::connect(&timeout_timer, &QTimer::timeout, &loop, [&]() {
            _wait_capture_stop_flag = false;
            loop.quit();
        });

        // Check capture state periodically via a repeating timer.
        QTimer check_timer;
        int check_count = 0;
        QObject::connect(&check_timer, &QTimer::timeout, &loop, [&]() {
            bool working = _session->is_working();
            bool running = _session->is_running_status();
            check_count++;
            if (check_count % 10 == 1) { // log every ~1s
                pxv_info("wait_capture check#%d: is_working=%d is_running=%d",
                         check_count, working, running);
            }
            if (!working && !running) {
                pxv_info("wait_capture: capture complete detected at check#%d", check_count);
                _wait_capture_stop_flag = true;
                loop.quit();
            }
        });

        timeout_timer.start(static_cast<int>(timeout_ms));
        check_timer.start(100); // check every 100 ms

        loop.exec();

        check_timer.stop();
        timeout_timer.stop();
    }

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
    const std::string& pattern,
    int capture_ratio,
    double repeat_interval_seconds,
    uint64_t sample_count) {
    (void)analog_sample_rate;
    (void)min_pulse_width_seconds;
    (void)max_pulse_width_seconds;

    if (!_session)
        return Result<int>::Fail(ErrorCode::InternalError,
                                 "Session is nullptr");
    if (!_device || !_device->have_instance())
        return Result<int>::Fail(ErrorCode::MissingDevice,
                                 "No device connected");
    if (_session->is_working())
        return Result<int>::Fail(ErrorCode::CaptureInProgress,
                                 "Capture already in progress");

    // Step 0: Ensure device is in LOGIC mode if digital channels are requested.
    ensure_logic_mode_for_digital(digital_channels);

    // Step 1: Configure channels (enable/disable per caller specification).
    configure_capture_channels(digital_channels, analog_channels);

    // Step 2: Set channel mode, then rebuild signal list.
    // Note: decoders are NOT cleared here. If the user added decoders before
    // start_capture (recommended MCP workflow), they are preserved for auto-decode.
    if (!channel_mode.empty()) {
        _device->set_config_string(SR_CONF_CHANNEL_MODE, channel_mode.c_str());
    }

    // Step 2b: Configure logic trigger if specified.
    if (trigger_channel_index >= 0) {
        pv::data::TriggerConfig cfg;
        cfg.set_mode(pv::data::TriggerConfig::Simple);
        if (after_trigger_seconds > 0.0) {
            uint64_t rate = (digital_sample_rate > 0) ? digital_sample_rate : _device->get_sample_rate();
            uint64_t sample_limit = _device->get_sample_limit();
            if (rate > 0 && sample_limit > 0) {
                uint64_t after_samples = static_cast<uint64_t>(
                    after_trigger_seconds * static_cast<double>(rate));
                uint16_t pos = static_cast<uint16_t>(
                    (after_samples * 100) / sample_limit);
                if (pos > 100) pos = 100;
                cfg.set_trigger_pos(pos);
            }
        }
        _session->set_trigger_config(cfg);
    }

    // Step 2c: Rebuild signal list to reflect new channel state.
    _session->init_signals();

    // Step 2d: Apply trigger types to SignalModel (drives UI rendering).
    apply_trigger_to_signal_models(trigger_channel_index, trigger_type, linked_channels);

    // Let UI process signals_changed event. Skip in headless mode.
    if (is_gui_mode()) QCoreApplication::processEvents();

    // Step 3: Set sample rate
    if (digital_sample_rate > 0) {
        bool ok = _device->set_config_uint64(SR_CONF_SAMPLERATE, digital_sample_rate);
        if (!ok)
            return Result<int>::Fail(ErrorCode::ConfigInvalid,
                                     "Failed to set digital sample rate");
    }

    // Step 4: Set digital threshold voltage (VTH)
    if (digital_threshold_volts != 0.0) {
        bool ok = _device->set_config_double(SR_CONF_VTH, digital_threshold_volts);
        if (!ok)
            return Result<int>::Fail(ErrorCode::ConfigInvalid,
                                     "Failed to set digital threshold voltage");
    }

    // Step 5: Configure glitch filters
    if (!glitch_filters.empty()) {
        std::map<int, uint32_t> thresholds;
        std::map<int, ::GlitchFilterMode> modes;
        for (const auto &gf : glitch_filters) {
            thresholds[(int)gf.first] = static_cast<uint32_t>(gf.second);
            modes[(int)gf.first] = ::GlitchFilterMode::Both;
        }
        _session->set_glitch_filter(thresholds, modes);
    }

    // Steps 5a-5g: Set device-level options
    configure_device_options(
        channel_mode, rle_enabled,
        stream_buffer_size_gb, stream_mem_buffer_size_gb,
        disk_cache_enabled, disk_cache_path,
        threshold_preset, operation_mode,
        buffer_options, digital_filter, pattern);

    // Steps 6-7: Set collect mode, repeat interval, capture ratio, duration
    configure_capture_timing(
        capture_mode, repeat_interval_seconds,
        capture_ratio, duration_seconds, sample_count);

    // Let UI process pending config change events before starting.
    if (is_gui_mode()) QCoreApplication::processEvents();

    // Step 8: Start capture
    bool ok = _session->start_capture(instant, api_document());
    if (!ok)
        return Result<int>::Fail(ErrorCode::DeviceError,
                                 "Failed to start capture");

    _capture_id++;
    broadcast_event(ServiceEvent::SampleConfigChanged, {});
    return Result<int>::Success(_capture_id);
}
int SessionService::get_current_capture_id() const {
    return _capture_id;
}

Result<void> SessionService::close_capture() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                 "Session is nullptr");

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

    // SR_CONF_USB_SPEED fork key deleted — use DeviceAgent typed wrapper
    // (reads libusb_get_device_speed via sr_dev_inst_usb_speed_get).
    info.usb_speed = _device->get_usb_speed();

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
    case MSO:
        return WorkMode::Mso;
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
        auto *mode = static_cast<const sr_dev_mode *>(l->data);
        if (mode) {
            switch (mode->mode) {
            case LOGIC:
                modes.push_back(WorkMode::Logic);
                break;
            case ANALOG:
                modes.push_back(WorkMode::Analog);
                break;
            case DSO:
                modes.push_back(WorkMode::Dso);
                break;
            case MSO:
                modes.push_back(WorkMode::Mso);
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
    broadcast_event(ServiceEvent::ChannelConfigChanged,
                    {{"field", "enabled"},
                     {"channel_index", std::to_string(index)},
                     {"value", enabled ? "1" : "0"}});
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
    broadcast_event(ServiceEvent::ChannelConfigChanged,
                    {{"field", "name"},
                     {"channel_index", std::to_string(index)},
                     {"value", name}});
    return Result<void>::Success();
}

uint16_t SessionService::get_channel_count(ChannelType type) const {
    if (!_session)
        return 0;

    int sr_type;
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
    default:
        return 0;
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
    broadcast_event(ServiceEvent::SampleConfigChanged,
                    {{"field", "sample_rate"},
                     {"value", std::to_string(rate)}});
    return Result<void>::Success();
}

Result<void> SessionService::set_sample_limit(uint64_t limit) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    // fx2lafw 等上游驱动支持 limit_samples 停止条件（protocol.c 中
    // sent_samples >= limit_samples 时调用 fx2lafw_abort_acquisition）。
    // Stream 模式下设置非零 limit 会让流式采集在到达后自动停止；
    // 传 0 则保持持续流（hwdriver.c 拒绝 set 0，DeviceAgent 静默忽略）。
    bool ok = _device->set_config_uint64(SR_CONF_LIMIT_SAMPLES, limit);
    if (!ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set sample limit");
    broadcast_event(ServiceEvent::SampleConfigChanged,
                    {{"field", "sample_limit"},
                     {"value", std::to_string(limit)}});
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
    broadcast_event(ServiceEvent::SampleConfigChanged,
                    {{"field", "time_base"},
                     {"value", std::to_string(tb)}});
    return Result<void>::Success();
}

Result<void> SessionService::set_collect_mode(CollectMode mode) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

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

    int old_mode = _session->get_collect_mode();
    _session->set_collect_mode(cm);
    if (old_mode == static_cast<int>(cm))
        return Result<void>::Success();

    broadcast_event(ServiceEvent::SampleConfigChanged,
                    {{"field", "collect_mode"},
                     {"value", std::to_string(static_cast<int>(mode))}});
    return Result<void>::Success();
}

Result<void> SessionService::set_repeat_interval(double seconds) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    double old_interval = _session->get_repeat_intvl();
    _session->set_repeat_intvl(seconds);
    if (old_interval == seconds)
        return Result<void>::Success();

    broadcast_event(ServiceEvent::SampleConfigChanged,
                    {{"field", "repeat_interval"},
                     {"value", std::to_string(seconds)}});
    return Result<void>::Success();
}

Result<uint64_t> SessionService::get_actual_sample_rate() const {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is nullptr");

    uint64_t rate = _session->cur_samplerate();
    if (rate == 0)
        return Result<uint64_t>::Fail(ErrorCode::NoData,
                                      "Sample rate not available");
    return Result<uint64_t>::Success(rate);
}

Result<uint64_t> SessionService::get_actual_sample_count() const {
    if (!_session)
        return Result<uint64_t>::Fail(ErrorCode::InternalError,
                                      "Session is nullptr");

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

    // Fork libsigrok's ds_trigger_get_en/get_pos are gone. Trigger state is
    // now read from the Core TriggerConfig (single source of truth).
    QJsonObject root;
    if (_session) {
        const auto& tcfg = _session->trigger_config();
        root["enabled"] = (tcfg.mode() != data::TriggerConfig::Simple ||
                           tcfg.stage_count() > 0) ? 1 : 0;
        root["position"] = static_cast<int>(tcfg.trigger_pos());
        root["trigger_config"] = tcfg.to_json();
        config.stage_count = tcfg.stage_count();
    } else {
        root["enabled"] = 0;
        root["position"] = 0;
        config.stage_count = 0;
    }

    config.config_json =
        QJsonDocument(root).toJson(QJsonDocument::Compact).toStdString();

    return config;
}

Result<void> SessionService::set_logic_trigger_config(
    const LogicTriggerConfig &config) {
    if (!_device || !_device->have_instance())
        return Result<void>::Fail(ErrorCode::MissingDevice,
                                  "No device connected");

    // Trigger state is written to the Core TriggerConfig (single source of
    // truth) and synced to libsigrok via sync_trigger_to_libsigrok() at
    // capture start. The config_json field carries the full trigger
    // configuration as JSON (produced by TriggerConfig::to_json()).
    if (_session) {
        if (!config.config_json.empty()) {
            // Parse the JSON and build a TriggerConfig from it.
            QJsonParseError parse_err;
            QJsonDocument doc = QJsonDocument::fromJson(
                QByteArray::fromStdString(config.config_json), &parse_err);
            if (parse_err.error != QJsonParseError::NoError || !doc.isObject()) {
                return Result<void>::Fail(ErrorCode::ConfigInvalid,
                    "Invalid trigger config JSON: " + parse_err.errorString().toStdString());
            }
            auto tcfg = pv::data::TriggerConfig::from_json(doc.object());

            // Override stage_count if explicitly provided.
            if (config.stage_count > 0)
                tcfg.set_stage_count(config.stage_count);

            _session->set_trigger_config(tcfg);
        } else {
            // No JSON provided — just update stage_count on existing config.
            auto tcfg = _session->trigger_config();
            if (config.stage_count > 0)
                tcfg.set_stage_count(config.stage_count);
            _session->set_trigger_config(tcfg);
        }
    }

    broadcast_event(ServiceEvent::TriggerConfigChanged,
                    {{"kind", "logic"},
                     {"stage_count", std::to_string(config.stage_count)}});
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

    if (!any_ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set any DSO trigger config");
    broadcast_event(ServiceEvent::TriggerConfigChanged,
                    {{"kind", "dso"},
                     {"channel", std::to_string(config.channel)}});
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

    if (target_ch) {
        if (_device->get_config_double(SR_CONF_PROBE_FACTOR, dval, target_ch))
            config.vfactor = dval;
    } else {
        // Try without channel
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
        if (_device->set_config_double(SR_CONF_PROBE_FACTOR, config.vfactor,
                                       target_ch))
            any_ok = true;
    } else {
        if (_device->set_config_double(SR_CONF_PROBE_FACTOR, config.vfactor))
            any_ok = true;
    }

    if (!any_ok)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "Failed to set probe config");
    broadcast_event(ServiceEvent::ChannelConfigChanged,
                    {{"field", "probe_config"},
                     {"channel_index", std::to_string(channel)}});
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

    auto &sig_list = _session->get_signal_models();
    for (auto m : sig_list) {
        if (!m)
            continue;

        SignalInfo info;
        info.index = m->index();
        info.name = m->name();
        info.type = sr_channel_type_to_api(m->type());
        info.enabled = m->enabled();
        info.color = m->color();

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
                                      "Session is nullptr");

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
                                      "Session is nullptr");

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
                                      "Session is nullptr");

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
                                      "Session is nullptr");

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
                                      "Session is nullptr");

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
        auto &sig_list = _session->get_signal_models();
        for (auto m : sig_list) {
            if (!m || !m->enabled())
                continue;
            // Only logic signals can be mapped to decoder channels
            if (m->type() != SR_CHANNEL_LOGIC)
                continue;
            json sig;
            sig["index"] = m->index();
            sig["name"] = m->name();
            available_signals.push_back(sig);
        }
    }
    result["availableSignals"] = available_signals;

    return Result<json>::Success(result);
}

std::vector<DecoderInstance> SessionService::get_active_decoders() const {
    std::vector<DecoderInstance> result;
    if (!_session)
        return result;

    auto &stacks = _session->get_decoder_stacks(api_document());
    for (size_t i = 0; i < stacks.size(); i++) {
        auto stack = stacks[i];
        if (!stack)
            continue;

        DecoderInstance inst;
        inst.instance_id = make_instance_id(stack.get());
        inst.row_index = static_cast<int32_t>(i);

        inst.is_running = stack->IsRunning();
        inst.progress = stack->get_progress() / 100.0;
        const char *root_id = stack->get_root_decoder_id();
        inst.decoder_id = root_id ? root_id : "";

        // Derive display name from the root decoder's name. If a custom label
        // is set on the stack, append it in parentheses so multiple instances
        // of the same decoder can be distinguished (e.g. "SPI(CH2.SPI)").
        std::string display_name;
        auto &dec_list = stack->stack();
        if (!dec_list.empty()) {
            auto *root_dec = dec_list.front().get();
            if (root_dec && root_dec->decoder() && root_dec->decoder()->name)
                display_name = root_dec->decoder()->name;
        }
        QString custom_label = stack->label();
        if (custom_label.isEmpty())
            custom_label = stack->auto_label();
        if (!custom_label.isEmpty())
            display_name += "(" + custom_label.toStdString() + ")";
        inst.display_name = display_name;

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
                                         "Session is nullptr");

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
            // Find the parent DecoderStack by its stable
            // "<handle_id>:<version>" instance identifier.
            auto &stacks = _session->get_decoder_stacks(api_document());
            std::shared_ptr<data::DecoderStack> parent_stack =
                find_stack_by_instance_id(stacks, stack_on_analyzer_id);

            if (!parent_stack)
                return Result<std::string>::Fail(ErrorCode::DecoderNotFound,
                                                 "Parent analyzer not found: " + stack_on_analyzer_id);

            auto decoder_stack = parent_stack;

            // Create the new sub-decoder and add it to the parent stack
            auto new_decoder = std::make_unique<data::decode::Decoder>(dec);
            decoder_stack->add_sub_decoder(std::move(new_decoder));

            // Store the custom label on the DecoderStack so exports and
            // list_analyzers can distinguish multiple instances of the same
            // decoder (e.g. "0:SPI(CH2.SPI)" vs "1:SPI(CH3.SPI)").
            if (!label.empty()) {
                decoder_stack->set_label(QString::fromStdString(label));
            }

            // Apply options to the new sub-decoder
            auto &stack = decoder_stack->stack();
            if (!stack.empty()) {
                auto *sub_dec = stack.back().get(); // the newly added decoder

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

            std::string instance_id = make_instance_id(parent_stack.get());

            broadcast_event(ServiceEvent::DecoderAdded,
                            {{"instance_id", instance_id},
                             {"decoder_id", dec->id ? dec->id : ""},
                             {"stacked_on", stack_on_analyzer_id}});

            return Result<std::string>::Success(instance_id);
        };

        // do_stack() touches Qt objects (DecoderStack is a QObject) and must
        // run on the main thread. Use the helper that invokes inline when
        // already on the main thread (avoids Qt::QueuedConnection deadlock).
        Result<std::string> result =
            run_string_on_main_thread(do_stack);

        if (!result.ok())
            return result;

        // Start decode if data is ready and copy is not in progress
        {
            std::string instance_id = result.value();
            auto &stacks = _session->get_decoder_stacks(api_document());
            std::shared_ptr<data::DecoderStack> decoder_stack =
                find_stack_by_instance_id(stacks, instance_id);

            if (decoder_stack && decoder_stack->options_changed() &&
                _session->have_view_data() &&
                !_session->is_copy_in_progress()) {
                QTimer::singleShot(0, qApp, [this, decoder_stack]() {
                    if (decoder_stack) {
                        // P0-3 fix: _delete_flag removed — shared_ptr manages
                        // lifetime. If the stack was removed, the shared_ptr
                        // would still be valid but the stack won't be found
                        // in the stacks list.
                        auto &st = _session->get_decoder_stacks(api_document());
                        for (size_t i = 0; i < st.size(); i++) {
                            if (st[i].get() == decoder_stack.get()) {
                                _session->rst_decoder(static_cast<int>(i),
                                                      api_document());
                                break;
                            }
                        }
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
        // Validate: decoder can only be added in LOGIC or MSO mode.
        // MSO (Mixed Signal Oscilloscope) includes logic channels, so
        // protocol decoders are valid. This mirrors ProtocolDock::add_protocol_by_id().
        int cur_mode = _session->get_device()->get_work_mode();
        if (cur_mode != LOGIC && cur_mode != MSO) {
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
        // causes a crash: it processes CopyToDocDone which calls
        // add_decode_task(), starting a decode thread that emits
        // new_decode_data() signals. The main thread is still inside do_add()
        // (e.g., in rebuild_decoder_pannel), causing a race in Qt's signal
        // delivery mechanism (crash in Qt6Core.dll).
        //
        // Instead, if copy is in progress, we just set up the decoder and
        // let CopyToDocDone start the decode task for us.
        // If copy is NOT in progress, we defer the decode start to after
        // do_add() returns using QTimer::singleShot.

        // Do NOT call processEvents() or wait for _copy_in_progress here.
        // Calling processEvents() while inside do_add() on the main thread
        // causes a crash: it processes CopyToDocDone which calls
        // add_decode_task(), starting a decode thread that emits
        // new_decode_data() signals. The main thread is still inside do_add()
        // (e.g., in rebuild_decoder_pannel), causing a race in Qt's signal
        // delivery mechanism (crash in Qt6Core.dll).
        //
        // Instead, if copy is in progress, we just set up the decoder and
        // let CopyToDocDone start the decode task for us.
        // If copy is NOT in progress, we defer the decode start to after
        // do_add() returns using QTimer::singleShot.

        std::shared_ptr<data::DecoderStack> decoder_stack;
        std::list<pv::data::decode::Decoder *> sub_decoders;
        // DecoderStatus must be heap-allocated; DecoderStack stores the pointer
        // and uses it for the lifetime of the decode trace.
        DecoderStatus *dstatus = new DecoderStatus();
        dstatus->m_format = (int)DecoderDataFormat::hex;

        bool ok = _session->add_decoder(dec, true, dstatus, sub_decoders,
                                        decoder_stack, api_document());

        if (!ok)
            return Result<std::string>::Fail(ErrorCode::DecoderError,
                                             "Failed to add decoder");

        if (!decoder_stack)
            return Result<std::string>::Fail(ErrorCode::DecoderError,
                                             "No decoder stack created");

        // Store the custom label on the DecoderStack so exports and
        // list_analyzers can distinguish multiple instances of the same
        // decoder (e.g. "0:SPI(CH2.SPI)" vs "1:SPI(CH3.SPI)").
        if (!label.empty()) {
            decoder_stack->set_label(QString::fromStdString(label));
        }

        {
            auto &stack = decoder_stack->stack();
            if (!stack.empty()) {
auto *root_decoder = stack.front().get();

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

                // set_index_list was a view::Trace method used to render the
                // connected-signal indicators in the trace header. DecoderStack
                // has no equivalent; the View layer/DecoderPannel is responsible
                // for tracking channel index lists. We retain index_list only
                // for the debug log below.
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
            //    options_changed. When capture completes, RevEndPacket
            //    → copy_data_to_document → CopyToDocDone will
            //    automatically call frame_ended() + add_decode_task() for this
            //    decoder.
            // 2. Copy in progress (added during capture): Same as case 1 —
            //    CopyToDocDone will start the decode for us.
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

        std::string instance_id = make_instance_id(decoder_stack.get());

        broadcast_event(ServiceEvent::DecoderAdded,
                        {{"instance_id", instance_id},
                         {"decoder_id", dec->id ? dec->id : ""}});

        return Result<std::string>::Success(instance_id);
    };

    // do_add() touches Qt objects (DecoderStack is a QObject, sub-decoders
    // are QObjects) and must run on the Qt main thread. Use the helper that
    // invokes inline when already on the main thread (avoids the
    // Qt::QueuedConnection + result_cv.wait() deadlock that occurs when MCP
    // requests — which already run on the main thread — call add_decoder).
    Result<std::string> result =
        run_string_on_main_thread(do_add);

    if (!result.ok())
        return result;

    // Now that do_add() has returned and the main thread is free,
    // start the decode task ONLY if copy was not in progress.
    // If copy was in progress, CopyToDocDone will start
    // the decode for us — we must not start it ourselves or we'll
    // get a duplicate decode task.
    {
        std::string instance_id = result.value();
        auto &stacks = _session->get_decoder_stacks(api_document());
        std::shared_ptr<data::DecoderStack> decoder_stack =
            find_stack_by_instance_id(stacks, instance_id);

        // Only start decode if copy is NOT in progress.
        // If copy is in progress, CopyToDocDone handler
        // will iterate decoder_stacks() and start decode for us.
        if (decoder_stack && decoder_stack->options_changed() &&
            _session->have_view_data() &&
            !_session->is_copy_in_progress()) {
            // Use QTimer::singleShot(0, ...) to defer the decode start
            // to the next event loop iteration, after all pending events
            // (including the DecoderAdded broadcast) have been processed.
            QTimer::singleShot(0, qApp, [this, decoder_stack]() {
                if (decoder_stack) {
                    // P0-3 fix: _delete_flag removed — shared_ptr manages lifetime.
                    auto &st = _session->get_decoder_stacks(api_document());
                    for (size_t i = 0; i < st.size(); i++) {
                        if (st[i].get() == decoder_stack.get()) {
                            _session->rst_decoder(static_cast<int>(i),
                                                  api_document());
                            break;
                        }
                    }
                }
            });
        }
    }

    // Wait for decoder completion if requested
    if (wait_for_completion && result.ok()) {
        std::string instance_id = result.value();
        // Look up the stack by its stable "<handle_id>:<version>" identifier.
        // The previous code reinterpret_cast<std::stoll(instance_id)> which
        // only worked when instance_id was a stringified raw pointer and was
        // undefined behavior for the new handle:version format.
        auto &stacks_for_wait = _session->get_decoder_stacks(api_document());
        std::shared_ptr<data::DecoderStack> decoder_stack =
            find_stack_by_instance_id(stacks_for_wait, instance_id);

        if (decoder_stack) {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

            // First, wait for copy_data_to_document to complete if it's
            // still in progress. The CopyToDocDone handler will
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

            // Remove this decoder on the main thread. invoke_or_call() runs
            // inline when the caller is already on the main thread (the common
            // MCP case) and otherwise dispatches via BlockingQueuedConnection.
            // This replaces the previous QueuedConnection + condition_variable
            // wait pattern which DEADLOCKED when the caller blocked the main
            // thread inside loop.exec()/wait().
            auto remove_this_stack = [this, decoder_stack]() {
                auto &stacks = _session->get_decoder_stacks(api_document());
                for (size_t i = 0; i < stacks.size(); i++) {
                    if (stacks[i].get() == decoder_stack.get()) {
                        _session->remove_decoder(static_cast<int>(i),
                                                 api_document());
                        break;
                    }
                }
            };

            // Poll until decode completes
            while (decoder_stack->IsRunning()) {
                timer.start(100);
                loop.exec();

                if (!decoder_stack->error_message().isEmpty()) {
                    // Remove decoder with error on main thread.
                    invoke_or_call(qApp, remove_this_stack);
                    return Result<std::string>::Fail(
                        ErrorCode::DecoderError,
                        decoder_stack->error_message().toStdString());
                }
            }

            if (decoder_stack->get_progress() < 100) {
                if (!decoder_stack->error_message().isEmpty()) {
                    invoke_or_call(qApp, remove_this_stack);
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
                                  "Session is nullptr");

    // remove_decoder modifies Qt objects (DecoderStack is a QObject) and
    // triggers signals, so it MUST run on the main thread.

    auto do_remove = [this, &instance_id]() -> Result<void> {
        auto &stacks = _session->get_decoder_stacks(api_document());
        for (size_t i = 0; i < stacks.size(); i++) {
            auto stack = stacks[i];
            if (!stack)
                continue;

            if (make_instance_id(stack.get()) == instance_id) {
                _session->remove_decoder(static_cast<int>(i), api_document());

                broadcast_event(ServiceEvent::DecoderRemoved,
                                {{"instance_id", instance_id}});
                return Result<void>::Success();
            }
        }
        return Result<void>::Fail(ErrorCode::DecoderNotFound,
                                  "Decoder instance not found");
    };

    // Dispatch to the main thread, invoking inline when already on the main
    // thread (avoids the Qt::QueuedConnection + result_cv.wait() deadlock).
    return run_void_on_main_thread(do_remove);
}

Result<void> SessionService::clear_all_decoders() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // Snapshot the current decoder stacks BEFORE clearing so each removed
    // stack can be reported via DecoderRemoved events (mirrors remove_decoder).
    auto &stacks_before = _session->get_decoder_stacks(api_document());
    std::vector<std::string> removed_ids;
    removed_ids.reserve(stacks_before.size());
    for (auto stack : stacks_before) {
        if (!stack)
            continue;
        removed_ids.push_back(make_instance_id(stack.get()));
    }

    // bUpdateView=true so the View layer refreshes; we emit DecoderRemoved
    // events ourselves below so subscribers can react to each removal.
    _session->clear_all_decoder(true);
    // Rebuild the protocol dock UI to remove stale layer items
    _session->rebuild_decoder_pannel();

    for (const auto &instance_id : removed_ids) {
        broadcast_event(ServiceEvent::DecoderRemoved,
                        {{"instance_id", instance_id}});
    }
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
            ErrorCode::InternalError, "Session is nullptr");

    // Find the decoder stack by instance_id
    auto &stacks = _session->get_decoder_stacks(api_document());

    // MCP debug
    {
        static QFile s_dbg;
        if (!s_dbg.isOpen()) {
            s_dbg.setFileName(QDir::tempPath() + "/pxview_automation_debug.log");
            (void)s_dbg.open(QIODevice::WriteOnly | QIODevice::Append);
        }
        if (s_dbg.isOpen()) {
            QString msg = QString("get_decoder_annotations: instance_id='%1', stacks.size()=%2\n")
                .arg(QString::fromStdString(instance_id))
                .arg(stacks.size());
            s_dbg.write(msg.toUtf8());
            for (auto stack : stacks) {
                std::string tid = make_instance_id(stack.get());
                msg = QString("  stack: handle_id=%1, version=%2, tid='%3'\n")
                    .arg(stack ? static_cast<qulonglong>(stack->handle_id()) : 0)
                    .arg(stack ? static_cast<qulonglong>(stack->version()) : 0)
                    .arg(QString::fromStdString(tid));
                s_dbg.write(msg.toUtf8());
            }
            s_dbg.flush();
        }
    }

    // Match by stable "<handle_id>:<version>" identifier. A malformed
    // instance_id (no colon / non-numeric) yields DecoderNotFound rather
    // than a crash.
    std::shared_ptr<data::DecoderStack> target_stack =
        find_stack_by_instance_id(stacks, instance_id);

    if (!target_stack)
        return Result<std::vector<DecoderAnnotation>>::Fail(
            ErrorCode::DecoderNotFound, "Decoder instance not found");

    auto decoder_stack = target_stack;

    std::vector<DecoderAnnotation> result;
    int row_count = decoder_stack->list_rows_size();

    // [PWMDBG] read-side diagnostics: how many annotations does the stack
    // actually hold vs how many we return (decode-side vs read-side bug)
    pxv_info("[PWMDBG] get_decoder_annotations: stack=%p, rows=%d, result_count=%llu, range=%llu..%llu, max_count=%d",
             decoder_stack.get(), row_count,
             (unsigned long long)decoder_stack->get_result_count(),
             (unsigned long long)start_sample, (unsigned long long)end_sample,
             max_count);

    for (int row = 0; row < row_count; row++) {
        uint64_t ann_count = decoder_stack->list_annotation_size(
            static_cast<uint16_t>(row));

        pxv_info("[PWMDBG] get_decoder_annotations: row=%d ann_count=%llu",
                 row, (unsigned long long)ann_count);

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

    pxv_info("[PWMDBG] get_decoder_annotations: returning %zu annotations",
             result.size());

    return Result<std::vector<DecoderAnnotation>>::Success(result);
}

// ===========================================================================
// 14. Measurements
// ===========================================================================

std::vector<MeasurementValue> SessionService::get_measurements() const {
    // Task C1.6: measurement computation now lives in the Core layer
    // (core::MeasureCalculator, reached via SigSession::get_measurements).
    // This works in both headless and GUI modes — no View pointer needed.
    // The old headless special case (returning empty + ErrorOccurred) is
    // removed. _session is a non-const pointer member, so calling the
    // non-const SigSession::get_measurements() from this const method is
    // legal (the pointed-to SigSession is not const-qualified).
    if (!_session)
        return {};
    return _session->get_measurements();
}

// ===========================================================================
// 15. Cursors
// ===========================================================================

std::vector<CursorInfo> SessionService::get_cursors() const {
    // Task C2.5: cursor state now lives in the Core layer
    // (SessionStateContext::cursor_registry(), reached via
    // SigSession::get_cursors / DataSource::get_cursors). This works in
    // both headless and GUI modes — no View pointer needed. The old
    // headless special case (returning empty + ErrorOccurred) is removed.
    // _session is a non-const pointer member, so calling the non-const
    // SigSession::get_cursors() from this const method is legal (the
    // pointed-to SigSession is not const-qualified; get_cursors is const
    // anyway). The Core CursorEntry is converted to the API CursorInfo
    // type here at the SessionService boundary (sample_position -> time_sec
    // via cur_snap_samplerate).
    if (!_session)
        return {};

    std::vector<CursorInfo> out;
    auto entries = _session->get_cursors();
    const uint64_t samplerate = _session->cur_snap_samplerate();
    out.reserve(entries.size());
    for (const auto &e : entries) {
        CursorInfo ci;
        ci.index      = e.index;
        ci.sample_pos = static_cast<int64_t>(e.sample_position);
        ci.time_sec   = (samplerate > 0)
                          ? static_cast<double>(e.sample_position) / static_cast<double>(samplerate)
                          : 0.0;
        out.push_back(ci);
    }
    return out;
}

Result<void> SessionService::add_cursor(uint64_t sample_pos) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // Task C2.5: write to the Core-layer CursorRegistry first so headless
    // mode persists state. The broadcast is retained so the GUI View layer
    // (when present) creates a matching view::Cursor rendering object via
    // MainWindow's IServiceEventListener handler. In headless mode the
    // broadcast is received by no-one and the Core state is the sole
    // effect, which is the intended behaviour.
    int new_index = _session->add_cursor(sample_pos);
    if (new_index < 0)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Failed to add cursor to Core registry");

    broadcast_event(ServiceEvent::ViewCursorAdded,
                    {{"sample_pos", std::to_string(sample_pos)}});
    return Result<void>::Success();
}

Result<void> SessionService::remove_cursor(int index) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // Task C2.5: remove from the Core-layer CursorRegistry. The broadcast
    // is retained so the GUI View layer (when present) can remove the
    // matching view::Cursor rendering object.
    bool ok = _session->remove_cursor(index);
    if (!ok)
        return Result<void>::Fail(ErrorCode::InvalidRequest,
                                  "Cursor index out of range");

    broadcast_event(ServiceEvent::ViewCursorRemoved,
                    {{"index", std::to_string(index)}});
    return Result<void>::Success();
}

Result<void> SessionService::clear_cursors() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // Task C2.5: clear the Core-layer CursorRegistry. The broadcast is
    // retained so the GUI View layer (when present) clears its rendering
    // objects.
    _session->clear_cursors();

    broadcast_event(ServiceEvent::ViewCursorsCleared);
    return Result<void>::Success();
}

// ===========================================================================
// 16. Signal processing
// ===========================================================================

Result<void> SessionService::set_glitch_filter(const GlitchFilterConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // 架构修复：用 channel_index 作 key，消除 View/Core 位置序号错位
    std::map<int, uint32_t> thresholds;
    std::map<int, ::GlitchFilterMode> modes;

    for (size_t i = 0; i < config.channels.size() && i < config.thresholds.size(); i++) {
        int ch_idx = (int)config.channels[i];
        thresholds[ch_idx] = static_cast<uint32_t>(config.thresholds[i]);
        // 默认 BOTH 模式
        modes[ch_idx] = ::GlitchFilterMode::Both;
    }
    // 如果有 mode 信息，覆盖默认值
    // config.modes 使用 pv::api::GlitchFilterMode, 需转换为全局 ::GlitchFilterMode
    for (size_t i = 0; i < config.channels.size() && i < config.modes.size(); i++) {
        int ch_idx = (int)config.channels[i];
        switch (config.modes[i]) {
        case pv::api::GlitchFilterMode::Both:
            modes[ch_idx] = ::GlitchFilterMode::Both;
            break;
        case pv::api::GlitchFilterMode::High:
            modes[ch_idx] = ::GlitchFilterMode::High;
            break;
        case pv::api::GlitchFilterMode::Low:
            modes[ch_idx] = ::GlitchFilterMode::Low;
            break;
        }
    }

    _session->set_glitch_filter(thresholds, modes);
    return Result<void>::Success();
}

Result<void> SessionService::clear_glitch_filter() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    _session->clear_glitch_filter();
    return Result<void>::Success();
}

GlitchFilterConfig SessionService::get_glitch_filter_config() const {
    GlitchFilterConfig config;
    if (!_session)
        return config;

    if (_session->is_glitch_filter_active()) {
        // 架构修复：从 map 读取当前配置，用 channel_index 作 key
        const auto &th = _session->glitch_filter_thresholds();
        const auto &md = _session->glitch_filter_modes();
        for (const auto &kv : th) {
            config.channels.push_back(kv.first);
            config.thresholds.push_back(static_cast<int32_t>(kv.second));
            pv::api::GlitchFilterMode m = pv::api::GlitchFilterMode::Both;
            auto mit = md.find(kv.first);
            if (mit != md.end()) {
                switch (mit->second) {
                case ::GlitchFilterMode::Both: m = pv::api::GlitchFilterMode::Both; break;
                case ::GlitchFilterMode::High: m = pv::api::GlitchFilterMode::High; break;
                case ::GlitchFilterMode::Low:  m = pv::api::GlitchFilterMode::Low;  break;
                }
            }
            config.modes.push_back(m);
        }
    }

    return config;
}

Result<void> SessionService::set_signal_invert(const SignalInvertConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

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
                                  "Session is nullptr");

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
                                  "Session is nullptr");

    bool ok = _session->set_file(QString::fromStdString(path));
    if (!ok)
        return Result<void>::Fail(ErrorCode::LoadFailed,
                                  "Failed to load file: " + path);
    broadcast_event(ServiceEvent::LoadComplete,
                    {{"path", path}});
    return Result<void>::Success();
}

Result<void> SessionService::save_file(const std::string &path) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    StoreSession store(_session);
    // Provide a headless ISessionDataGetter so save_start() can generate
    // session config JSON without MainWindow.
    HeadlessSessionDataGetter getter(_session, _device);
    store._sessionDataGetter = &getter;
    store.SetFileName(QString::fromStdString(path));
    bool ok = store.save_start();
    if (!ok)
        return Result<void>::Fail(ErrorCode::SaveFailed,
                                  "Failed to save file: " + path);

    store.wait();
    broadcast_event(ServiceEvent::SaveComplete,
                    {{"path", path}});
    return Result<void>::Success();
}

Result<void> SessionService::export_data(const ExportConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    StoreSession store(_session);
    // Provide a headless ISessionDataGetter for export operations.
    HeadlessSessionDataGetter getter(_session, _device);
    store._sessionDataGetter = &getter;
    store.SetFileName(QString::fromStdString(config.output_path));
    store.SetDataRange(config.start_sample, config.end_sample);
    
    // Set specific channels and type for export.
    // SignalModel::type() now returns the libsigrok SR_CHANNEL_* value
    // (SR_CHANNEL_LOGIC=10000, SR_CHANNEL_ANALOG=10002) as the single source
    // of truth, so set_export_channel_type must use SR_CHANNEL_* values to
    // match the type check `_export_channel_type != m->type()` in
    // export_start().
    store.set_export_channels(config.channels);
    store.set_export_channel_type(config.is_logic
        ? SR_CHANNEL_LOGIC
        : SR_CHANNEL_ANALOG);

    // Apply analog downsample ratio if > 1
    if (config.analog_downsample_ratio > 1) {
        store.set_analog_downsample_ratio(config.analog_downsample_ratio);
    }

    // Enable ISO8601 timestamp formatting if requested
    if (config.iso8601_timestamp) {
        store.set_iso8601_timestamp(true);
    }

    bool ok = store.export_start();
    if (!ok) {
        // Propagate StoreSession's specific error message (e.g. "Invalid
        // export format", "No data to save") instead of a generic string,
        // so MCP/API callers can diagnose the failure.
        QString err = store.error();
        std::string msg = err.isEmpty()
            ? std::string("Failed to export data")
            : ("Failed to export data: " + err.toStdString());
        return Result<void>::Fail(ErrorCode::ExportFailed, msg);
    }

    store.wait();
    if (store.error() != "")
        return Result<void>::Fail(ErrorCode::ExportFailed,
                                  store.error().toStdString());
    broadcast_event(ServiceEvent::ExportComplete,
                    {{"format", config.is_logic ? "csv_logic" : "csv_analog"},
                     {"path", config.output_path}});
    return Result<void>::Success();
}

Result<void> SessionService::export_binary(const ExportConfig &config) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

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
        auto &sig_list = _session->get_signal_models();
        for (auto m : sig_list) {
            if (m && m->enabled())
                channels.push_back(m->index());
        }
    }

    for (auto ch_idx : channels) {
        // Determine channel type from SignalModel
        auto &sig_list = _session->get_signal_models();
        ChannelType ch_type = ChannelType::Logic;
        for (auto m : sig_list) {
            if (m && m->index() == ch_idx) {
                ch_type = sr_channel_type_to_api(m->type());
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
            // CRITICAL FIX: get_samples() extends actual_end up to the enclosing
            // leaf-block boundary (8 samples = 1 byte), so for captures that do
            // not end exactly on a leaf boundary `count` can exceed the real
            // valid range (get_ring_sample_count) by up to 1 byte of stale /
            // zero-padded data. Trim `count` to the actual captured sample range
            // so the exported binary matches the saved .pxc L-<ch> length exactly.
            uint64_t valid = snapshot->get_ring_sample_count();
            uint64_t max_count = (valid > start) ? (valid - start) : 0;
            if (count > max_count)
                count = max_count;
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

            // CRITICAL FIX: 上游 libsigrok analog 数据布局是 interleaved：
            //   [s0_ch0][s0_ch1]...[s0_chN][s1_ch0][s1_ch1]...
            // 每个样本占 unit_bytes 字节（float=4, uint16=2, uint8=1）。
            // 旧代码用 pitch=EnvelopeScaleFactor=16 + ch_idx（如 8）+ /255.0f
            // 是 fork 时代码为 ADC 整数（0-255）写的，对上游 float 电压数据完全错误。
            // 现在按正确的 interleaved 布局读取，并用 get_ch_order 把通道索引
            // 映射到 snapshot 内部的 order（如 ch=8 在 5 通道 analog 中是 order=0）。
            int order = snapshot->get_ch_order(ch_idx);
            if (order < 0) {
                pxv_warn("export_binary: channel %d not in analog snapshot, skipping", ch_idx);
                continue;
            }

            uint32_t channel_num = snapshot->get_channel_num();
            uint8_t unit_bytes = snapshot->get_unit_bytes();
            bool is_float = snapshot->is_float();

            // interleaved 步长：每个样本占 channel_num * unit_bytes 字节
            uint64_t stride = (uint64_t)channel_num * unit_bytes;
            uint64_t ch_offset = (uint64_t)order * unit_bytes;

            uint64_t count = end - start + 1;

            // Apply downsample ratio
            uint64_t step = config.analog_downsample_ratio > 1
                                ? config.analog_downsample_ratio : 1;

            for (uint64_t i = 0; i < count; i += step) {
                const uint8_t *p = raw + i * stride + ch_offset;
                float val;
                if (is_float && unit_bytes == sizeof(float)) {
                    // float 电压数据：直接 memcpy 4 字节
                    memcpy(&val, p, sizeof(float));
                } else {
                    // 整数数据：按 unit_bytes 拼接（little-endian）后转 float
                    uint64_t iv = 0;
                    for (uint8_t b = 0; b < unit_bytes; b++) {
                        iv |= ((uint64_t)p[b]) << (b * 8);
                    }
                    val = static_cast<float>(iv);
                }
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

    broadcast_event(ServiceEvent::ExportComplete,
                    {{"format", "binary"},
                     {"path", config.output_path}});
    return Result<void>::Success();
}

Result<void> SessionService::export_decoder_table(
    const std::string &filepath,
    const std::vector<AnalyzerExportConfig> &analyzers,
    bool iso8601_timestamp) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    auto &stacks = _session->get_decoder_stacks(api_document());
    if (stacks.empty())
        return Result<void>::Fail(ErrorCode::NoData,
                                  "No active decoders");

    uint64_t samplerate = _session->cur_samplerate();

    // Determine which decoders to export
    std::vector<std::pair<std::shared_ptr<data::DecoderStack>, int>> selected;
    if (analyzers.empty()) {
        // Export all decoders
        for (size_t i = 0; i < stacks.size(); i++) {
            if (stacks[i])
                selected.push_back(std::make_pair(stacks[i], 4)); // default Ascii radix
        }
    } else {
        for (const auto &cfg : analyzers) {
            for (auto stack : stacks) {
                if (!stack) continue;
                if (make_instance_id(stack.get()) == cfg.analyzer_id) {
                    selected.push_back(std::make_pair(stack, cfg.radix_type));
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

    for (auto &[stack, radix] : selected) {
        auto decoder_stack = stack;
        if (!decoder_stack)
            continue;

        // Derive analyzer name from the root decoder's name (srd_decoder.name).
        // If a custom label is set on the stack, append it in parentheses so
        // multiple instances of the same decoder can be distinguished, e.g.
        // "SPI(CH2.SPI)" vs "SPI(CH3.SPI)".
        std::string analyzer_name;
        auto &dec_list = decoder_stack->stack();
        if (!dec_list.empty()) {
            auto *root_dec = dec_list.front().get();
            if (root_dec && root_dec->decoder() && root_dec->decoder()->name)
                analyzer_name = root_dec->decoder()->name;
        }
        QString custom_label = decoder_stack->label();
        if (custom_label.isEmpty())
            custom_label = decoder_stack->auto_label();
        if (!custom_label.isEmpty())
            analyzer_name += "(" + custom_label.toStdString() + ")";
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
    broadcast_event(ServiceEvent::ExportComplete,
                    {{"format", "decoder_table_csv"},
                     {"path", filepath}});
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
                                  "Session is nullptr");

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

    broadcast_event(ServiceEvent::ExportComplete,
                    {{"path", directory},
                     {"format", "raw_csv"}});
    return Result<void>::Success();
}

Result<void> SessionService::export_raw_data_binary(
    const std::string &directory,
    const std::vector<int32_t> &digital_channels,
    const std::vector<int32_t> &analog_channels,
    int analog_downsample_ratio) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    ExportConfig config;
    config.analog_downsample_ratio = static_cast<uint64_t>(analog_downsample_ratio);

    // Combine all channels
    std::vector<int32_t> all_channels;
    all_channels.insert(all_channels.end(), digital_channels.begin(), digital_channels.end());
    all_channels.insert(all_channels.end(), analog_channels.begin(), analog_channels.end());

    if (all_channels.empty()) {
        // Default to all enabled channels
        auto &sig_list = _session->get_signal_models();
        for (auto m : sig_list) {
            if (m && m->enabled())
                all_channels.push_back(m->index());
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
                                  "Session is nullptr");

    std::vector<AnalyzerExportConfig> analyzers;
    if (!analyzer_id.empty()) {
        AnalyzerExportConfig cfg;
        cfg.analyzer_id = analyzer_id;
        cfg.radix_type = radix_type;
        analyzers.push_back(cfg);
    }

    return export_decoder_table(filepath, analyzers, iso8601_timestamp);
}

Result<void> SessionService::export_raw_data(
    const std::string &format,
    const std::string &directory,
    const std::vector<int32_t> &digital_channels,
    const std::vector<int32_t> &analog_channels,
    int analog_downsample_ratio,
    bool iso8601_timestamp) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // Normalize format -> sr_output module id + file suffix.
    // binary has no sr_output exts, but id "binary" still resolves via
    // sr_output_find(), so we keep the suffix consistent with the module id.
    std::string fmt = format;
    std::string suffix;
    if (fmt == "csv")              suffix = "csv";
    else if (fmt == "binary")      suffix = "bin";
    else if (fmt == "vcd")         suffix = "vcd";
    else if (fmt == "hex")         suffix = "hex";
    else if (fmt == "bits")        suffix = "bits"; // module id (matched against sr_output_id, not file ext)
    else
        return Result<void>::Fail(ErrorCode::ExportFailed,
                                  "Unsupported export format: " + fmt +
                                  " (supported: csv, binary, vcd, hex, bits)");

    // binary uses its own dedicated writer (raw bytes, no sr_output module)
    if (fmt == "binary")
        return export_raw_data_binary(directory, digital_channels,
                                      analog_channels, analog_downsample_ratio);

    QDir dir(QString::fromStdString(directory));
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            return Result<void>::Fail(ErrorCode::ExportFailed,
                                      "Failed to create output directory");
        }
    }

    for (int32_t ch : digital_channels) {
        ExportConfig config;
        config.output_path = directory + "/channel_" + std::to_string(ch) + "." + suffix;
        config.channels = {ch};
        config.is_logic = true;
        config.include_headers = true;
        config.analog_downsample_ratio = static_cast<uint64_t>(analog_downsample_ratio);
        config.iso8601_timestamp = iso8601_timestamp;

        auto r = export_data(config);
        if (!r)
            return r;
    }

    for (int32_t ch : analog_channels) {
        ExportConfig config;
        config.output_path = directory + "/analog_" + std::to_string(ch) + "." + suffix;
        config.channels = {ch};
        config.is_logic = false;
        config.include_headers = true;
        config.analog_downsample_ratio = static_cast<uint64_t>(analog_downsample_ratio);
        config.iso8601_timestamp = iso8601_timestamp;

        auto r = export_data(config);
        if (!r)
            return r;
    }

    broadcast_event(ServiceEvent::ExportComplete,
                    {{"path", directory}, {"format", fmt}});
    return Result<void>::Success();
}

// ===========================================================================
// 19. View control
// ===========================================================================

Result<void> SessionService::show_region(uint64_t start_sample,
                                         uint64_t end_sample) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // Broadcast a ViewShowRegion request. The View layer (when present)
    // subscribes via IServiceEventListener and calls View::set_view_region().
    // SigSession::show_region() also forwards to ISessionCallback listeners
    // for backward compatibility.
    _session->show_region(start_sample, end_sample, false);
    broadcast_event(ServiceEvent::ViewShowRegion,
                    {{"start", std::to_string(start_sample)},
                     {"end", std::to_string(end_sample)}});
    return Result<void>::Success();
}

Result<void> SessionService::zoom_fit() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // View-side operation. In GUI mode a subscribed View will respond;
    // in headless mode this is a no-op.
    broadcast_event(ServiceEvent::ViewZoomFit);
    return Result<void>::Success();
}

Result<void> SessionService::zoom_in() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    broadcast_event(ServiceEvent::ViewZoomIn);
    return Result<void>::Success();
}

Result<void> SessionService::zoom_out() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    broadcast_event(ServiceEvent::ViewZoomOut);
    return Result<void>::Success();
}

// ===========================================================================
// 20. Spectrum/Lissajous/Math
// ===========================================================================

Result<void> SessionService::enable_spectrum(int16_t channel_index,
                                             bool enable) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // The legacy SpectrumTrace had a separate enabled flag; SpectrumStack
    // (the de-view-ified core data object) does not. We rely on
    // spectrum_rebuild() to create/refresh SpectrumStack entries for every
    // enabled DSO channel. Toggling the visibility of an individual
    // spectrum is now a View-layer concern and is not modelled here.
    (void)channel_index;
    (void)enable;

    _session->spectrum_rebuild();
    broadcast_event(ServiceEvent::ChannelConfigChanged,
                    {{"feature", "spectrum"},
                     {"enabled", enable ? "true" : "false"}});
    return Result<void>::Success();
}

Result<void> SessionService::enable_lissajous(int16_t x_channel,
                                              int16_t y_channel,
                                              double percent) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    _session->lissajous_rebuild(true, x_channel, y_channel, percent);
    broadcast_event(ServiceEvent::ChannelConfigChanged,
                    {{"feature", "lissajous"},
                     {"enabled", "true"}});
    return Result<void>::Success();
}

Result<void> SessionService::disable_lissajous() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    _session->lissajous_disable();
    return Result<void>::Success();
}

Result<void> SessionService::enable_math(int16_t ch1, int16_t ch2,
                                         int math_type) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    // Verify that both indices are valid DSO channels before invoking
    // math_rebuild (which now takes channel indices directly, not
    // view::DsoSignal pointers).
    bool found_ch1 = false;
    bool found_ch2 = false;
    auto &sig_list = _session->get_signal_models();
    for (auto m : sig_list) {
        if (!m) continue;
        if (m->type() != SR_CHANNEL_DSO) continue;
        if (m->index() == ch1) found_ch1 = true;
        if (m->index() == ch2) found_ch2 = true;
    }

    if (!found_ch1 || !found_ch2)
        return Result<void>::Fail(ErrorCode::ChannelNotFound,
                                  "DSO channel not found");

    auto type = static_cast<data::MathStack::MathType>(math_type);
    _session->math_rebuild(true, ch1, ch2, type);
    broadcast_event(ServiceEvent::ChannelConfigChanged,
                    {{"feature", "math"},
                     {"enabled", "true"}});
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
        auto &stacks = _session->get_decoder_stacks(api_document());
        for (auto stack : stacks) {
            if (!stack) continue;
            if (stack->IsRunning()) {
                int progress = stack->get_progress();
                std::string instance_id = make_instance_id(stack.get());
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

    // Wake up any headless wait_capture_complete() waiter so its predicate
    // can re-check the session state. In GUI mode this is a no-op (no one
    // is waiting on the cv).
    _wait_cv.notify_all();
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

    // Wake up wait_capture_complete() in headless mode.
    _wait_cv.notify_all();
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

void SessionService::delay_prop_msg(QString strMsg) {
    broadcast_event(ServiceEvent::ErrorOccurred,
                    {{"message", strMsg.toStdString()}});
}

// ===========================================================================
// IEventListener implementation
// ===========================================================================

// The 4 PREV ordering events are emitted via broadcast_sync<XxxPrev>({}).
// The 32 notification events are emitted via broadcast_async<TypedEvent>({}).
// Each handler re-broadcasts as a ServiceEvent for MCP/WS clients, mirroring
// the former int-message switch cases verbatim.

void SessionService::on_event(const pv::interface::StoreConfPrev &) {
    broadcast_event(ServiceEvent::SaveComplete,
                    {{"detail", "store_conf_prev"}});
}

void SessionService::on_event(const pv::interface::CurrentDeviceChangePrev &) {
    // No former case for this code — SessionService did not handle
    // CurrentDeviceChangePrev. Empty override satisfies the
    // IEventListener virtual dispatch.
}

void SessionService::on_event(const pv::interface::StartCollectWorkPrev &) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "start_collect_prev"}});
}

void SessionService::on_event(const pv::interface::EndCollectWorkPrev &) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "end_collect_prev"}});
}

void SessionService::on_event(const pv::interface::StartCollectWork &) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "start_collect"}});
}

void SessionService::on_event(const pv::interface::CollectStart &) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "collect_start"}});
}

void SessionService::on_event(const pv::interface::CollectEnd &) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "collect_end"}});
}

void SessionService::on_event(const pv::interface::EndCollectWork &) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"detail", "end_collect"}});
}

void SessionService::on_event(const pv::interface::RevEndPacket &) {
    broadcast_event(ServiceEvent::DataUpdated,
                    {{"detail", "end_packet"}});
}

void SessionService::on_event(const pv::interface::CaptureStateChanged &) {
    broadcast_event(ServiceEvent::CaptureStateChanged);
}

void SessionService::on_event(const pv::interface::DeviceListUpdated &) {
    broadcast_event(ServiceEvent::DeviceListUpdated);
}

void SessionService::on_event(const pv::interface::DeviceModeChanged &) {
    broadcast_event(ServiceEvent::DeviceModeChanged);
}

void SessionService::on_event(const pv::interface::DeviceConfigUpdated &) {
    broadcast_event(ServiceEvent::DeviceConfigChanged);
}

void SessionService::on_event(const pv::interface::DeviceDetached &) {
    broadcast_event(ServiceEvent::DeviceDetached);
}

void SessionService::on_event(const pv::interface::UsbDeviceArrived &) {
    broadcast_event(ServiceEvent::NewUsbDevice);
}

void SessionService::on_event(const pv::interface::CurrentDeviceChanged &) {
    broadcast_event(ServiceEvent::DeviceModeChanged,
                    {{"detail", "device_changed"}});
}

void SessionService::on_event(const pv::interface::DeviceOptionsUpdated &) {
    broadcast_event(ServiceEvent::DeviceConfigChanged,
                    {{"detail", "options_updated"}});
}

void SessionService::on_event(const pv::interface::DsoViewOptionChanged &ev) {
    broadcast_event(ServiceEvent::DeviceConfigChanged,
                    {{"detail", "dso_view_option"},
                     {"channel_index", std::to_string(ev.channel_index)}});
}

void SessionService::on_event(const pv::interface::SampleRateChanged &) {
    broadcast_event(ServiceEvent::DeviceConfigChanged,
                    {{"detail", "duration_updated"}});
}

void SessionService::on_event(const pv::interface::CollectModeChanged &) {
    broadcast_event(ServiceEvent::DeviceConfigChanged,
                    {{"detail", "collect_mode_changed"}});
}

void SessionService::on_event(const pv::interface::DataPoolChanged &) {
    broadcast_event(ServiceEvent::DataUpdated,
                    {{"detail", "data_pool_changed"}});
}

void SessionService::on_event(const pv::interface::SimpleTriggerChanged &) {
    broadcast_event(ServiceEvent::DeviceConfigChanged,
                    {{"detail", "trigger_changed"}});
}

void SessionService::on_event(const pv::interface::GlitchFilterStarted &) {
    broadcast_event(ServiceEvent::GlitchFilterStarted);
}

void SessionService::on_event(const pv::interface::GlitchFilterProgress &) {
    broadcast_event(ServiceEvent::GlitchFilterProgress);
}

void SessionService::on_event(const pv::interface::GlitchFilterCompleted &) {
    broadcast_event(ServiceEvent::GlitchFilterCompleted);
}

void SessionService::on_event(const pv::interface::GlitchFilterCleared &) {
    broadcast_event(ServiceEvent::GlitchFilterCleared);
}

void SessionService::on_event(const pv::interface::SignalInvertStarted &) {
    broadcast_event(ServiceEvent::SignalInvertStarted);
}

void SessionService::on_event(const pv::interface::SignalInvertCompleted &) {
    broadcast_event(ServiceEvent::SignalInvertCompleted);
}

void SessionService::on_event(const pv::interface::SignalInvertCleared &) {
    broadcast_event(ServiceEvent::SignalInvertCleared);
}

void SessionService::on_event(const pv::interface::CopyToDocDone &) {
    broadcast_event(ServiceEvent::DataUpdated,
                    {{"detail", "copy_to_doc_done"}});
}

void SessionService::on_event(const pv::interface::SampleCountUpdated &) {
    broadcast_event(ServiceEvent::DataUpdated,
                    {{"detail", "sample_count_updated"}});
}

void SessionService::on_event(const pv::interface::ActiveDocumentChanged &) {
    broadcast_event(ServiceEvent::ChannelConfigChanged,
                    {{"change", "active_document"}});
}

void SessionService::on_event(const pv::interface::CopyInProgressChanged &) {
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"change", "copy_in_progress"}});
}

void SessionService::on_event(const pv::interface::CaptureOwnerChanged &ev) {
    // new_owner non-nullptr = capture in progress, nullptr = idle.
    broadcast_event(ServiceEvent::CaptureStateChanged,
                    {{"change", "capture_owner"},
                     {"is_working", ev.new_owner ? "true" : "false"}});
}

void SessionService::on_event(const pv::interface::TrigNextCollect &) {
    broadcast_event(ServiceEvent::TriggerReceived,
                    {{"detail", "next_collect"}});
}

void SessionService::on_event(const pv::interface::SaveComplete &) {
    broadcast_event(ServiceEvent::SaveComplete);
}

void SessionService::on_event(const pv::interface::ClearDecodeData &) {
    broadcast_event(ServiceEvent::DecodeDone,
                    {{"detail", "clear_decode_data"}});
}

// ===========================================================================
// 22. Batch B — extended operations
// ===========================================================================

// ---------------------------------------------------------------------------
// B1.1: reconfigure_decoder — in-place option/channel_map update + re-decode
// ---------------------------------------------------------------------------

Result<void> SessionService::reconfigure_decoder(
    const std::string &instance_id,
    const std::map<std::string, std::string> &options,
    const std::map<std::string, int> &channel_map) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    auto do_reconfigure = [this, &instance_id, &options, &channel_map]() -> Result<void> {
        auto &stacks = _session->get_decoder_stacks(api_document());
        std::shared_ptr<data::DecoderStack> target_stack =
            find_stack_by_instance_id(stacks, instance_id);

        if (!target_stack)
            return Result<void>::Fail(ErrorCode::DecoderNotFound,
                                      "Decoder instance not found");

        auto &dec_list = target_stack->stack();
        if (dec_list.empty())
            return Result<void>::Fail(ErrorCode::DecoderError,
                                      "Decoder stack has no root decoder");

auto *root_decoder = dec_list.front().get();
if (!root_decoder || !root_decoder->decoder())
            return Result<void>::Fail(ErrorCode::DecoderError,
                                      "Invalid root decoder");

        const srd_decoder *dec = root_decoder->decoder();

        // Apply new options (only those present in the options map; others
        // remain unchanged). The GVariant type-matching logic mirrors
        // add_decoder's option binding.
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
                            else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("d")))
                                val = g_variant_new_double(g_variant_get_double(enum_val));
                            else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("x")))
                                val = g_variant_new_int64(g_variant_get_int64(enum_val));
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
                            else if (g_variant_is_of_type(enum_val, G_VARIANT_TYPE("t")))
                                val = g_variant_new_uint64(g_variant_get_uint64(enum_val));
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
                        else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("b")))
                            val = g_variant_new_boolean(opt.second == "True" || opt.second == "1");
                        else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("y")))
                            val = g_variant_new_byte(static_cast<guchar>(std::stoi(opt.second)));
                        else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("n")))
                            val = g_variant_new_int16(static_cast<gint16>(std::stoi(opt.second)));
                        else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("q")))
                            val = g_variant_new_uint16(static_cast<guint16>(std::stoi(opt.second)));
                        else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("i")))
                            val = g_variant_new_int32(std::stoi(opt.second));
                        else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("u")))
                            val = g_variant_new_uint32(std::stoul(opt.second));
                        else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("t")))
                            val = g_variant_new_uint64(std::stoull(opt.second));
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
                    else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("y")))
                        val = g_variant_new_byte(static_cast<guchar>(std::stoi(opt.second)));
                    else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("n")))
                        val = g_variant_new_int16(static_cast<gint16>(std::stoi(opt.second)));
                    else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("q")))
                        val = g_variant_new_uint16(static_cast<guint16>(std::stoi(opt.second)));
                    else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("i")))
                        val = g_variant_new_int32(std::stoi(opt.second));
                    else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("u")))
                        val = g_variant_new_uint32(std::stoul(opt.second));
                    else if (g_variant_is_of_type(opt_def->def, G_VARIANT_TYPE("t")))
                        val = g_variant_new_uint64(std::stoull(opt.second));
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

            root_decoder->set_option(opt.first.c_str(), val);
        }

        // Apply new channel_map (only when non-empty). The matching logic
        // mirrors add_decoder: keys are matched case-insensitively against
        // srd_channel id/name/desc. When channel_map has exactly one entry
        // and the decoder has exactly one required channel, auto-map.
        if (!channel_map.empty()) {
            auto match_channel = [&channel_map](const srd_channel *ch) -> std::pair<bool, int> {
                if (!ch) return {false, 0};
                std::string ch_id = ch->id ? ch->id : "";
                std::string ch_name = ch->name ? ch->name : "";
                std::string ch_desc = ch->desc ? ch->desc : "";

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

            int required_ch_count = 0;
            for (const GSList *c = dec->channels; c; c = c->next)
                required_ch_count++;

            bool auto_map = (channel_map.size() == 1 && required_ch_count == 1);

            for (const GSList *c = dec->channels; c; c = c->next) {
                auto *ch = static_cast<srd_channel*>(c->data);
                auto [found, val] = match_channel(ch);
                if (!found && auto_map) {
                    val = channel_map.begin()->second;
                    found = true;
                }
                if (found)
                    probes[ch] = val;
            }

            for (const GSList *c = dec->opt_channels; c; c = c->next) {
                auto *ch = static_cast<srd_channel*>(c->data);
                auto [found, val] = match_channel(ch);
                if (found)
                    probes[ch] = val;
            }

            root_decoder->set_probes(probes);
        }

        target_stack->set_options_changed(true);

        // Trigger re-decode via rst_decoder_by_key_handel. This attaches
        // view data and starts a single decode task for the affected stack.
        // Deferred via QTimer::singleShot to avoid running the decode task
        // while we are still inside this lambda on the main thread (same
        // pattern as add_decoder — running it inline can race Qt signal
        // delivery when the decode thread emits new_decode_data()).
        bool copy_in_progress = _session->is_copy_in_progress();
        if (!_session->have_view_data() || copy_in_progress) {
            // No data yet or copy in progress — the capture pipeline will
            // start the decode for us when data is ready (CopyToDocDone).
            target_stack->set_options_changed(true);
        } else {
            target_stack->set_capture_end_flag(true);
            target_stack->frame_ended();

            std::shared_ptr<data::DecoderStack> stack_ref = target_stack;
            QTimer::singleShot(0, qApp, [this, stack_ref]() {
                if (!stack_ref)
                    return;
                _session->rst_decoder_by_key_handel(stack_ref->get_key_handel(),
                                                    api_document());
            });
        }

        _session->rebuild_decoder_pannel();

        broadcast_event(ServiceEvent::ChannelConfigChanged,
                        {{"feature", "decoder_reconfigured"},
                         {"instance_id", instance_id}});

        return Result<void>::Success();
    };

    return run_void_on_main_thread(do_reconfigure);
}

// ---------------------------------------------------------------------------
// B1.2: get_error_state / clear_error_state
// ---------------------------------------------------------------------------

Result<ErrorState> SessionService::get_error_state() {
    if (!_session)
        return Result<ErrorState>::Fail(ErrorCode::InternalError,
                                        "Session is nullptr");

    ErrorState state;
    auto err = _session->get_error();
    state.error_code = static_cast<int>(err);
    state.has_error = (err != SigSession::No_err);
    state.error_pattern = _session->get_error_pattern();

    // Derive a human-readable message from the SESSION_ERROR_STATUS enum.
    switch (err) {
    case SigSession::No_err:
        state.error_message = "";
        break;
    case SigSession::Hw_err:
        state.error_message = "Hardware error";
        break;
    case SigSession::Malloc_err:
        state.error_message = "Memory allocation error";
        break;
    case SigSession::Test_timeout_err:
        state.error_message = "Test timeout error";
        break;
    case SigSession::Pkt_data_err:
        state.error_message = "Packet data error";
        break;
    case SigSession::Data_overflow:
        state.error_message = "Data overflow";
        break;
    default:
        state.error_message = "Unknown error";
        break;
    }

    return Result<ErrorState>::Success(state);
}

Result<void> SessionService::clear_error_state() {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    _session->clear_error();
    return Result<void>::Success();
}

// ---------------------------------------------------------------------------
// B1.3: set_save_range
// ---------------------------------------------------------------------------

Result<void> SessionService::set_save_range(uint64_t start_sample,
                                            uint64_t end_sample) {
    if (!_session)
        return Result<void>::Fail(ErrorCode::InternalError,
                                  "Session is nullptr");

    if (start_sample > end_sample)
        return Result<void>::Fail(ErrorCode::ConfigInvalid,
                                  "start_sample must be <= end_sample");

    _session->set_save_start(start_sample);
    _session->set_save_end(end_sample);
    return Result<void>::Success();
}

// ---------------------------------------------------------------------------
// B1.4: refresh_device_list
// ---------------------------------------------------------------------------

Result<std::vector<DeviceInfo>> SessionService::refresh_device_list() {
    if (!_session)
        return Result<std::vector<DeviceInfo>>::Fail(ErrorCode::InternalError,
                                                     "Session is nullptr");

    // Trigger a hot-plug rescan of all upstream drivers. This refreshes
    // DeviceAgent's cached scanned_sdi() list (get_device_list reuses the
    // cache to avoid LIBUSB_ERROR_ACCESS on already-opened devices).
    _session->refresh_device_list();

    // Build the device list from the refreshed cache. get_device_list
    // returns a calloc'd array of ds_device_base_info (handle + name);
    // we expand each entry into a DeviceInfo using the underlying sdi.
    int count = 0;
    int actived_index = -1;
    struct ds_device_base_info *array = _session->get_device_list(count, actived_index);

    std::vector<DeviceInfo> result;
    if (!array || count <= 0) {
        if (array)
            free(array);
        return Result<std::vector<DeviceInfo>>::Success(result);
    }

    // Enrich each entry with driver/connection info from the underlying
    // sr_dev_inst. The handle is an opaque 1-based index into the
    // scanned_sdi() vector (handle = index + 1). refresh_device_list()
    // above called DeviceAgent::set_scanned_devices() so the cache is fresh.
    DeviceAgent *agent = _session->get_device();
    static const std::vector<struct sr_dev_inst*> empty_vec;
    const std::vector<struct sr_dev_inst*> &scanned =
        agent ? agent->scanned_sdi() : empty_vec;
    const std::vector<struct sr_dev_inst*> &file_devs =
        agent ? agent->file_devices() : empty_vec;

    for (int i = 0; i < count; i++) {
        struct ds_device_base_info *entry = &array[i];
        DeviceInfo info;
        info.id = std::to_string(static_cast<intptr_t>(entry->handle));
        info.display_name = entry->name;

        // Resolve the underlying sdi to fill driver/connection fields.
        struct sr_dev_inst *sdi = nullptr;
        if (i < static_cast<int>(scanned.size()))
            sdi = scanned[i];
        if (!sdi && i < static_cast<int>(file_devs.size()))
            sdi = file_devs[i];

        if (sdi) {
            struct sr_dev_driver *drv = sr_dev_inst_driver_get(sdi);
            std::string drv_name;
            if (drv && drv->name) {
                drv_name = drv->name;
                info.driver_name = drv_name;
            }
            const char *vendor = sr_dev_inst_vendor_get(sdi);
            const char *model = sr_dev_inst_model_get(sdi);
            const char *conn = sr_dev_inst_connid_get(sdi);
            if (conn)
                info.path = conn;
            // Classify device type using the same driver-name heuristic as
            // DeviceAgent::open_by_handle (no public sr_dev_inst_type_get
            // accessor exists in upstream libsigrok 0.6.0).
            info.is_demo = (drv_name == "demo");
            info.is_file = (drv_name == "virtual-session" ||
                            drv_name.find("file") != std::string::npos);
            info.is_hardware = !info.is_demo && !info.is_file;
            info.is_virtual = info.is_file || info.is_demo;
            if (info.display_name.empty()) {
                if (vendor && model)
                    info.display_name = std::string(vendor) + " " + model;
                else if (model)
                    info.display_name = model;
                else if (conn)
                    info.display_name = conn;
            }
        }

        result.push_back(info);
    }

    free(array);

    broadcast_event(ServiceEvent::DeviceListUpdated);
    return Result<std::vector<DeviceInfo>>::Success(result);
}

// ---------------------------------------------------------------------------
// B1.5: get_math_results / get_spectrum_results / get_lissajous_results
// ---------------------------------------------------------------------------

Result<MathResult> SessionService::get_math_results() {
    if (!_session)
        return Result<MathResult>::Fail(ErrorCode::InternalError,
                                        "Session is nullptr");

    MathResult result;
    auto math_stack = _session->get_math_stack();
    if (!math_stack) {
        result.is_enabled = false;
        return Result<MathResult>::Success(result);
    }

    result.is_enabled = true;
    result.ch1_index = math_stack->ch1_index();
    result.ch2_index = math_stack->ch2_index();
    result.math_type = static_cast<int>(math_stack->get_type());
    result.sample_num = math_stack->get_sample_num();

    // Copy computed math samples. get_math(start) returns a pointer into
    // the stack's internal _math vector; we copy [0, sample_num) so the
    // caller gets a stable snapshot.
    if (result.sample_num > 0) {
        const double *samples = math_stack->get_math(0);
        if (samples) {
            result.samples.assign(samples, samples + result.sample_num);
        }
    }

    return Result<MathResult>::Success(result);
}

Result<SpectrumResult> SessionService::get_spectrum_results() {
    if (!_session)
        return Result<SpectrumResult>::Fail(ErrorCode::InternalError,
                                            "Session is nullptr");

    SpectrumResult result;
    auto &stacks = _session->get_spectrum_stacks();
    if (stacks.empty()) {
        result.is_enabled = false;
        return Result<SpectrumResult>::Success(result);
    }

    // Expose the first spectrum stack. MCP callers that need a specific
    // channel's spectrum should use enable_spectrum first.
    auto &stack = stacks.front();
    if (!stack) {
        result.is_enabled = false;
        return Result<SpectrumResult>::Success(result);
    }

    result.is_enabled = true;
    result.channel_index = stack->get_index();
    result.sample_num = stack->get_sample_num();
    result.windows_index = stack->get_windows_index();
    result.dc_ignored = stack->dc_ignored();
    result.sample_interval = stack->get_sample_interval();

    // get_fft_spectrum() returns const std::vector<double> by value (a copy
    // of the internal _power_spectrum vector). The const return type
    // prevents move semantics, so this is a copy assignment — acceptable
    // for a snapshot read.
    result.spectrum = stack->get_fft_spectrum();

    return Result<SpectrumResult>::Success(result);
}

Result<LissajousResult> SessionService::get_lissajous_results() {
    if (!_session)
        return Result<LissajousResult>::Fail(ErrorCode::InternalError,
                                             "Session is nullptr");

    LissajousResult result;
    auto *model = _session->get_lissajous_model();
    if (!model) {
        result.is_enabled = false;
        return Result<LissajousResult>::Success(result);
    }

    result.is_enabled = model->enabled();
    result.x_index = model->x_index();
    result.y_index = model->y_index();
    result.percent = model->percent();

    return Result<LissajousResult>::Success(result);
}

// ---------------------------------------------------------------------------
// B1.6: get_decoder_binary_output
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> SessionService::get_decoder_binary_output(
    const std::string &instance_id, int output_id) {
    if (!_session)
        return Result<std::vector<uint8_t>>::Fail(ErrorCode::InternalError,
                                                  "Session is nullptr");

    // DecoderStack only registers an SRD_OUTPUT_ANN callback (see
    // decoderstack.cpp:815). Binary output (SRD_OUTPUT_BINARY) is declared
    // by some decoders via srd_decoder::binary, but DecoderStack does not
    // register a binary callback and does not store binary output data.
    // Implementing this would require:
    //   1. Adding a binary callback to DecoderStack (srd_pd_output_callback_add
    //      with SRD_OUTPUT_BINARY).
    //   2. Adding a binary-data store + accessor to DecoderStack.
    // Both changes are outside the Batch B scope (which does not modify
    // data-layer classes). Return ConfigNotSupported so the MCP layer can
    // report the limitation cleanly.
    (void)instance_id;
    (void)output_id;
    return Result<std::vector<uint8_t>>::Fail(
        ErrorCode::ConfigNotSupported,
        "DecoderStack does not capture binary output. "
        "Binary output callbacks are not registered — this would require "
        "data-layer changes outside Batch B scope.");
}

// ---------------------------------------------------------------------------
// B1.7: get_decoder_class_names
// ---------------------------------------------------------------------------

Result<std::vector<DecoderClassInfo>> SessionService::get_decoder_class_names(
    const std::string &decoder_id) {
    if (!_session)
        return Result<std::vector<DecoderClassInfo>>::Fail(
            ErrorCode::InternalError, "Session is nullptr");

    // Look up the decoder by ID. srd_decoder_get_by_id is the same accessor
    // used by add_decoder.
    srd_decoder *dec = srd_decoder_get_by_id(decoder_id.c_str());
    if (!dec)
        return Result<std::vector<DecoderClassInfo>>::Fail(
            ErrorCode::DecoderNotFound,
            "Decoder not found: " + decoder_id);

    std::vector<DecoderClassInfo> result;

    // dec->annotations is a GSList of char* (nullptr-terminated descriptions).
    // The index in the list is the annotation class id, which matches the
    // ann_class field of DecoderAnnotation returned by get_decoder_annotations.
    int class_id = 0;
    for (const GSList *l = dec->annotations; l; l = l->next, class_id++) {
        DecoderClassInfo info;
        info.class_id = class_id;
        const char *desc = static_cast<const char *>(l->data);
        info.class_name = ensure_utf8(desc);
        result.push_back(info);
    }

    return Result<std::vector<DecoderClassInfo>>::Success(result);
}

} // namespace api
} // namespace pv
