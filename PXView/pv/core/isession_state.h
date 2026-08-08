#ifndef PXVIEW_CORE_ISESSION_STATE_H
#define PXVIEW_CORE_ISESSION_STATE_H

#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <QDateTime>
#include <QString>

#include "pv/core/eventbus.h"
#include "pv/data/document/sessiondata.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/triggerconfig.h"
#include "pv/data/stack/lissajousmodel.h"
#include "pv/data/stack/spectrumstack.h"
#include "pv/data/stack/mathstack.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/core/cursorregistry.h"
#include "pv/session/deviceagent.h"
#include "pv/core/isession_coordination.h"

namespace pv {

namespace core {
class DataFeedParser;
class FilterProcessor;
class DocumentRegistry;
} // namespace core

/**
 * ISessionState — shared session state access interface (Spec v3 Task 5 fix).
 *
 * Breaks the Manager <-> SessionStateContext circular dependency. Managers now
 * depend only on this abstract interface (which composes ISessionCoordination
 * for coordination/notification), never on the concrete SessionStateContext.
 *
 * Every method mirrors a SessionStateContext public method. SessionStateContext
 * implements them; signatures must match exactly (including const). The
 * optional `override` keyword is not required in C++ for a derived class to
 * satisfy a base pure-virtual, but signatures must be identical.
 *
 * Methods intentionally EXCLUDED (kept on the concrete class only):
 *   - ctors/dtors, copy/assign
 *   - set_event_bus / event_bus (EventBus injection, Managers don't need it)
 *   - the 5 set_*_manager() back-pointer setters
 *   - error() / SESSION_ERROR_STATUS (enum is defined inside SessionStateContext;
 *     set_error(int) lives on ISessionCoordination instead)
 *   - any method already declared on ISessionCoordination (coordination/notify)
 */
class ISessionState : public ISessionCoordination {
public:
    virtual ~ISessionState() = default;

    // --- Manager back-pointer accessors (Managers query sibling Managers) ---
    virtual core::DataFeedParser *data_feed_parser() = 0;
    virtual core::DocumentRegistry *document_registry() = 0;
    virtual core::FilterProcessor *filter_processor() = 0;

    // --- Mutex (non-movable, exposed by reference) ---
    virtual std::mutex &data_mutex() = 0;
    virtual std::mutex &sampling_mutex() = 0;

    // --- Business objects ---
    virtual std::vector<std::shared_ptr<data::SignalModel>> &signal_models() = 0;
    // TS-2 fix: thread-safe snapshot for callers that don't hold the mutex.
    virtual std::vector<std::shared_ptr<data::SignalModel>> signal_models_snapshot() = 0;
    virtual std::vector<std::shared_ptr<data::SpectrumStack>> &spectrum_stacks() = 0;
    virtual data::LissajousModel *lissajous_model() const = 0;
    virtual void set_lissajous_model(std::unique_ptr<data::LissajousModel> m) = 0;
    virtual const std::shared_ptr<data::MathStack> &math_stack() const = 0;
    virtual void set_math_stack(std::shared_ptr<data::MathStack> m) = 0;

    // --- Time ---
    virtual QDateTime session_time() const = 0;
    virtual QDateTime trig_time() const = 0;

    // --- Bool state ---
    virtual bool is_triged() const = 0;
    virtual bool trigger_flag() const = 0;
    virtual bool hw_replied() const = 0;
    virtual void set_bClose(bool v) = 0;
    virtual bool is_saving() const = 0;
    virtual void set_saving(bool v) = 0;

    // --- Numeric state ---
    virtual int trigger_ch() const = 0;
    virtual uint64_t error_pattern() const = 0;
    virtual void set_error_pattern(uint64_t v) = 0;
    virtual uint64_t save_start() const = 0;
    virtual void set_save_start(uint64_t v) = 0;
    virtual uint64_t save_end() const = 0;
    virtual void set_save_end(uint64_t v) = 0;
    virtual int map_zoom() const = 0;
    virtual void set_map_zoom(int v) = 0;

    // --- Atomic state ---
    virtual bool is_working() const = 0;
    virtual int device_status() const = 0;
    virtual void set_device_status(int v) = 0;

    // --- Device ---
    virtual DeviceAgent &device_agent() = 0;

    // --- Data buffers ---
    virtual SessionData *view_data() = 0;
    virtual void set_view_data(SessionData *d) = 0;
    virtual SessionData *capture_data() = 0;
    virtual std::vector<std::unique_ptr<SessionData>> &data_list() = 0;
    virtual bool is_single_buffer() const = 0;

    // --- Trigger config ---
    virtual const data::TriggerConfig &trigger_config() const = 0;
    virtual void set_trigger_config(const data::TriggerConfig &c) = 0;

    // --- Cursor registry ---
    virtual core::CursorRegistry &cursor_registry() = 0;
    virtual const core::CursorRegistry &cursor_registry() const = 0;

    // --- Decode-stack helpers ---
    virtual std::vector<std::shared_ptr<data::DecoderStack>> &
    decode_traces(data::SessionDocument *doc = nullptr) = 0;
    virtual std::vector<std::shared_ptr<data::DecoderStack>> &
    get_decoder_stacks(data::SessionDocument *doc = nullptr) = 0;
    virtual std::shared_ptr<data::DecoderStack>
    get_decoder_trace(int index, data::SessionDocument *doc = nullptr) = 0;
    virtual int get_trace_index_by_key_handel(void *handel,
                                              data::SessionDocument *doc = nullptr) = 0;

    // --- Misc (kept for interface completeness) ---
    virtual void cur_snap_samplerate_changed() = 0;
    virtual void frame_ended() = 0;
    virtual void repeat_hold(int percent) = 0;
    virtual void show_wait_trigger() = 0;
    virtual void delay_prop_msg(QString strMsg) = 0;
    virtual uint64_t next_decoder_handle_id() = 0;
};

} // namespace pv

#endif // PXVIEW_CORE_ISESSION_STATE_H
