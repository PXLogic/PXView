#ifndef PXVIEW_CORE_ISESSION_COORDINATION_H
#define PXVIEW_CORE_ISESSION_COORDINATION_H

#include <QDateTime>
#include <cstdint>
#include <memory>

struct srd_decoder;
class DecoderStatus;

namespace pv {

// SessionData is defined in the pv namespace (not pv::data)
class SessionData;

namespace data {
class DecoderStack;
namespace decode { class Decoder; }
} // namespace data



/**
 * ISessionCoordination — cross-manager coordination interface.
 *
 * Spec v2 Task 10 created this interface but no Manager actually used it.
 * Spec v3 Task 5 expands it with ALL coordination/notification methods that
 * the 5 Managers call on SessionStateContext, and makes each Manager hold
 * an ISessionCoordination* pointer for those calls.
 *
 * Managers still hold SessionStateContext* for pure shared-state access
 * (view_data, capture_data, device_agent, signal_models, etc.), but all
 * cross-manager coordination and EventBus notification dispatch now goes
 * through this interface, breaking the concrete-type coupling in the
 * coordination direction.
 *
 * Methods are grouped by calling Manager:
 * - CaptureManager: update_capture, set_trigger_*, set_hw_replied,
 *   set_receive_data_len, set_capture_data, set_session_time
 * - DecodeTaskManager: data_updated, signals_changed, bClose
 * - DataFeedParser: receive_header, receive_trigger, frame_began,
 *   session_error, set_is_triged, set_trig_time, set_error
 * - FilterProcessor: (reuses data_updated)
 * - DocumentRegistry: set_is_working
 *
 * DataFeedParser also needs direct (typed) access to CaptureManager /
 * DecodeTaskManager state. Rather than exposing those concrete types
 * through this interface (which would couple the abstraction back to the
 * concrete managers), SigSession injects the manager pointers directly
 * into DataFeedParser via set_managers() — keeping ISessionCoordination
 * free of any concrete-manager dependency.
 */
class ISessionCoordination {
public:
    virtual ~ISessionCoordination() = default;

    // --- Error status (used by DataFeedParser) ---
    enum ErrorStatus {
        No_err,
        Hw_err,
        Malloc_err,
        Test_timeout_err,
        Pkt_data_err,
        Data_overflow
    };

    // --- Decode task coordination (Spec v2) ---
    virtual void clear_all_decode_task2() = 0;
    virtual void add_decode_task(std::shared_ptr<data::DecoderStack> stack) = 0;
    virtual void attach_data_to_signal(SessionData *data) = 0;

    // --- Trigger coordination (Spec v2) ---
    virtual void sync_trigger_to_libsigrok(bool disable_trigger = false) = 0;

    // --- Glitch filter coordination (Spec v2) ---
    virtual void clear_glitch_filter_state_for_capture() = 0;

    // --- Query methods (Spec v2) ---
    virtual uint16_t get_ch_num(int type) = 0;
    virtual uint64_t cur_samplelimits() = 0;
    virtual uint64_t cur_snap_samplerate() = 0;
    virtual void set_cur_snap_samplerate(uint64_t samplerate) = 0;
    virtual void set_cur_samplelimits(uint64_t samplelimits) = 0;

    // --- EventBus notification dispatch (Spec v3 Task 5) ---
    // Called by DecodeTaskManager, FilterProcessor, CaptureManager
    virtual void data_updated() = 0;
    virtual void signals_changed() = 0;
    virtual void update_capture() = 0;
    // Called by DataFeedParser
    virtual void receive_header() = 0;
    virtual void receive_trigger(uint64_t trigger_pos) = 0;
    virtual void frame_began() = 0;
    virtual void session_error() = 0;

    // --- State mutation (Spec v3 Task 5) ---
    // Called by CaptureManager, DataFeedParser
    virtual void set_trigger_flag(bool v) = 0;
    virtual void set_trigger_ch(int v) = 0;
    virtual void set_hw_replied(bool v) = 0;
    virtual void set_receive_data_len(uint64_t len) = 0;
    virtual void set_capture_data(SessionData *d) = 0;
    virtual void set_session_time(QDateTime t) = 0;
    virtual void set_is_working(bool v) = 0;
    virtual void set_is_triged(bool v) = 0;
    virtual void set_trig_time(QDateTime t) = 0;
    virtual void set_error(int e) = 0;

    // --- Query (Spec v3 Task 5) ---
    virtual bool bClose() const = 0;
};

} // namespace pv

#endif // PXVIEW_CORE_ISESSION_COORDINATION_H
