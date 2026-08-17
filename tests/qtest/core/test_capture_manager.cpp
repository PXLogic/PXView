/*
 * test_capture_manager.cpp — QTest unit tests for CaptureManager
 *
 * Spec harden-review, T7. Covers the OFFLINE (no USB / no hardware-thread)
 * state & query logic of CaptureManager: collect-mode setter/getters,
 * data-lock state, store-confirm flag interplay, the atomic instant/stream
 * flags, derived queries (is_repeating / is_realtime_refresh), the
 * synthesized get_capture_status() query, and the early-exit gates of
 * start_capture()/stop_capture().
 *
 * CaptureManager's constructor requires an EventBus* and an ISessionState*
 * (which IS-A ISessionCoordination). We inject:
 *   - a FakeDispatcher (implements IAsyncDispatcher) so EventBus::broadcast_async
 *     queues events instead of posting to a Qt event loop — no QCoreApplication
 *     needed (same pattern as test_eventbus.cpp).
 *   - a MockSessionState that satisfies the full ISessionState abstract
 *     interface. The heavy concrete members it must return by reference
 *     (DeviceAgent / SessionData) are default-constructed and never driven;
 *     the capture manager never calls them on the offline paths under test.
 *
 * Honest scope notes (asserted in the test bodies where relevant):
 *   - start_capture() falls back to `false` immediately when there are no
 *     signal models (no device loaded). The full success path needs a device,
 *     signal models, and a real libsigrok session — not exercised offline.
 *   - is_first_store_confirm() only returns true once _work_time_id has been
 *     incremented by a successful capture; offline we assert the baseline
 *     (false) and that clear_store_confirm_flag() keeps it consistent/false.
 *   - get_capture_status() reports progress=0 whenever cur_samplelimits()==0
 *     (fresh state) — we assert the triggered-truth propagation and the
 *     limits==0 progress path, not real per-sample progress.
 */

#include <QtTest>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "pv/base/log.h"          // pxv_log extern + assert redefinition
#include "pv/core/capturemanager.h"
#include "pv/ui/langresource.h"    // GUI l10n helper (stubbed below)
#include "pv/ui/msgbox.h"          // GUI message box (stubbed below)

// ---- Link shims for GUI-only helpers ----
// CaptureManager::exec_capture()'s error path builds a message via
// LangResource (L_S macro) and shows it through MsgBox. These are GUI/View
// helpers that live outside the headless core, and the offline paths under
// test never reach them, so we satisfy the linker with minimal no-op stubs
// (Instance() returns nullptr — never dereferenced because exec_capture() is
// not exercised offline). Signatures match nav langresource.h / msgbox.h.
LangResource *LangResource::Instance() { return nullptr; }
const char *LangResource::get_lang_text(int, const char *, const char *default_str) {
    return default_str;
}
void MsgBox::Show(const QString) {}

// ---- Link shims for QColor::fromString / QColor::lightness ----
// These are real Qt6Gui exports, referenced by pxview-config's AppConfig theme
// helpers (via dllimport __imp__ indirection). In this static-lib test link,
// MinGW's --as-needed drops the Qt6Gui import library before the transitively
// appended pxview-config.a is scanned, so the __imp__ references stay undefined.
// The offline tests never reach AppConfig's color helpers, so a null __imp__
// indirection slot satisfies the linker and is never dereferenced at runtime.
void *__imp__ZN6QColor10fromStringE14QAnyStringView = nullptr;
void *__imp__ZNK6QColor10lightnessFEv = nullptr;

using pv::core::CaptureManager;
using pv::core::EventBus;
using pv::core::IAsyncDispatcher;

// pxv_log is provided by pxview-core's log.cpp (which the test links via the
// pxview-core static lib). The xlog_* backend symbols come from
// ${CMAKE_SOURCE_DIR}/common/log/xlog.c (added in tests/qtest/CMakeLists.txt).
// We therefore do NOT define pxv_log here — doing so would cause a duplicate
// symbol against pxview-core's log.cpp at link time.

namespace {

// Marker event type used only to seed EventBus::has_subscribers() so the
// assert(_event_bus->has_subscribers()) gate inside start_capture() passes
// even in debug builds where pxv_assert is active.
struct DummyEvent {};

// ---- injectable fake dispatcher: queue events, never run ----
// Lets broadcast_async() complete deterministically without a Qt event loop.
class FakeDispatcher : public IAsyncDispatcher {
public:
    void post(std::function<void()> fn) override { ++post_count; _pending.push(std::move(fn)); }
    bool on_target_thread() const override { return true; }
    unsigned post_count = 0;
private:
    std::queue<std::function<void()>> _pending;
};

// ---- minimal ISessionState / ISessionCoordination mock ----
// Only the queries exercised by the tested offline paths need real backing;
// everything else is a safe default. Members that the interface forces us to
// return by reference (DeviceAgent / SessionData / stack containers) are held
// as default-constructed placeholders and never driven by the tests below.
class MockSessionState : public pv::ISessionState {
public:
    // --- test helpers (not part of the interface) ---
    void setWorking(bool v) { _is_working = v; }
    void setTriged(bool v) { _is_triged = v; }
    void setCurSampleLimits(uint64_t v) { _cur_samplelimits = v; }
    void setCurSnapSamplerate(uint64_t v) { _cur_snap_samplerate = v; }
    bool getTriged() const { return _is_triged; }

    // --- ISessionCoordination ---
    void clear_all_decode_task2() override {}
    void add_decode_task(std::shared_ptr<pv::data::DecoderStack>) override {}
    void attach_data_to_signal(pv::SessionData*) override {}
    void sync_trigger_to_libsigrok(bool) override {}
    void clear_glitch_filter_state_for_capture() override {}
    uint16_t get_ch_num(int) override { return 0; }
    uint64_t cur_samplelimits() override { return _cur_samplelimits; }
    uint64_t cur_snap_samplerate() override { return _cur_snap_samplerate; }
    void set_cur_snap_samplerate(uint64_t v) override { _cur_snap_samplerate = v; }
    void set_cur_samplelimits(uint64_t v) override { _cur_samplelimits = v; }
    void data_updated() override {}
    void signals_changed() override {}
    void update_capture() override {}
    void receive_header() override {}
    void receive_trigger(uint64_t) override {}
    void frame_began() override {}
    void session_error() override {}
    void set_trigger_flag(bool v) override { _trigger_flag = v; }
    void set_trigger_ch(int v) override { _trigger_ch = v; }
    void set_hw_replied(bool v) override { _hw_replied = v; }
    void set_receive_data_len(uint64_t) override {}
    void set_capture_data(pv::SessionData*) override {}
    void set_session_time(QDateTime t) override { _session_time = t; }
    void set_is_working(bool v) override { _is_working = v; }
    void set_is_triged(bool v) override { _is_triged = v; }
    void set_trig_time(QDateTime t) override { _trig_time = t; }
    void set_error(int) override {}
    bool bClose() const override { return _bClose; }

    // --- ISessionState: manager back-pointers (null; offline paths never touch) ---
    pv::core::DataFeedParser *data_feed_parser() override { return nullptr; }
    pv::core::DocumentRegistry *document_registry() override { return nullptr; }
    pv::core::FilterProcessor *filter_processor() override { return nullptr; }

    // --- Mutexes ---
    std::mutex &data_mutex() override { return _data_mutex; }
    std::mutex &sampling_mutex() override { return _sampling_mutex; }

    // --- Business objects ---
    std::vector<std::shared_ptr<pv::data::SignalModel>> &signal_models() override { return _signal_models; }
    std::vector<std::shared_ptr<pv::data::SignalModel>> signal_models_snapshot() override { return _signal_models; }
    std::vector<std::shared_ptr<pv::data::SpectrumStack>> &spectrum_stacks() override { return _spectrum_stacks; }
    pv::data::LissajousModel *lissajous_model() const override { return nullptr; }
    void set_lissajous_model(std::unique_ptr<pv::data::LissajousModel>) override {}
    const std::shared_ptr<pv::data::MathStack> &math_stack() const override { return _math_stack; }
    void set_math_stack(std::shared_ptr<pv::data::MathStack> m) override { _math_stack = std::move(m); }

    // --- Time ---
    QDateTime session_time() const override { return _session_time; }
    QDateTime trig_time() const override { return _trig_time; }

    // --- Bool state ---
    bool is_triged() const override { return _is_triged; }
    bool trigger_flag() const override { return _trigger_flag; }
    bool hw_replied() const override { return _hw_replied; }
    void set_bClose(bool v) override { _bClose = v; }
    bool is_saving() const override { return _is_saving; }
    void set_saving(bool v) override { _is_saving = v; }

    // --- Numeric state ---
    int trigger_ch() const override { return _trigger_ch; }
    uint64_t error_pattern() const override { return _error_pattern; }
    void set_error_pattern(uint64_t v) override { _error_pattern = v; }
    uint64_t save_start() const override { return _save_start; }
    void set_save_start(uint64_t v) override { _save_start = v; }
    uint64_t save_end() const override { return _save_end; }
    void set_save_end(uint64_t v) override { _save_end = v; }
    int map_zoom() const override { return _map_zoom; }
    void set_map_zoom(int v) override { _map_zoom = v; }

    // --- Atomic state ---
    bool is_working() const override { return _is_working; }
    int device_status() const override { return _device_status; }
    void set_device_status(int v) override { _device_status = v; }

    // --- Device ---
    DeviceAgent &device_agent() override { return _agent; }

    // --- Data buffers ---
    pv::SessionData *view_data() override { return &_view_data; }
    void set_view_data(pv::SessionData*) override {}
    pv::SessionData *capture_data() override { return &_capture_data; }
    std::vector<std::unique_ptr<pv::SessionData>> &data_list() override { return _data_list; }
    bool is_single_buffer() const override { return false; }

    // --- Trigger config ---
    const pv::data::TriggerConfig &trigger_config() const override { return _trigger_config; }
    void set_trigger_config(const pv::data::TriggerConfig &c) override { _trigger_config = c; }

    // --- Cursor registry ---
    pv::core::CursorRegistry &cursor_registry() override { return _cursors; }
    const pv::core::CursorRegistry &cursor_registry() const override { return _cursors; }

    // --- Decode-stack helpers ---
    std::vector<std::shared_ptr<pv::data::DecoderStack>> &
    decode_traces(pv::data::SessionDocument *) override { return _decode_traces; }
    std::vector<std::shared_ptr<pv::data::DecoderStack>> &
    get_decoder_stacks(pv::data::SessionDocument *) override { return _decode_traces; }
    std::shared_ptr<pv::data::DecoderStack>
    get_decoder_trace(int, pv::data::SessionDocument *) override { return nullptr; }
    int get_trace_index_by_key_handel(void *, pv::data::SessionDocument *) override { return -1; }

    // --- Misc ---
    void cur_snap_samplerate_changed() override {}
    void frame_ended() override {}
    void repeat_hold(int) override {}
    void show_wait_trigger() override {}
    void delay_prop_msg(QString) override {}
    uint64_t next_decoder_handle_id() override { return 0; }

    // --- SharedState signaling ---
    void notify_decode_complete() override {}
    void reset_capture_complete() override {}

private:
    // Shared state backed by real default-constructed objects.
    std::atomic<bool> _is_working{false};
    std::atomic<bool> _is_triged{false};
    std::atomic<int> _device_status{0};
    bool _bClose = false;
    bool _trigger_flag = false;
    bool _hw_replied = false;
    bool _is_saving = false;
    int _trigger_ch = 0;
    uint64_t _error_pattern = 0;
    uint64_t _save_start = 0;
    uint64_t _save_end = 0;
    int _map_zoom = 0;
    uint64_t _cur_samplelimits = 0;
    uint64_t _cur_snap_samplerate = 0;
    QDateTime _session_time;
    QDateTime _trig_time;
    std::mutex _data_mutex;
    std::mutex _sampling_mutex;

    // Concrete objects the interface forces us to return by reference.
    DeviceAgent _agent;
    pv::SessionData _view_data;
    pv::SessionData _capture_data;
    std::vector<std::unique_ptr<pv::SessionData>> _data_list;
    std::vector<std::shared_ptr<pv::data::SignalModel>> _signal_models;
    std::vector<std::shared_ptr<pv::data::SpectrumStack>> _spectrum_stacks;
    std::shared_ptr<pv::data::MathStack> _math_stack;
    std::vector<std::shared_ptr<pv::data::DecoderStack>> _decode_traces;
    pv::data::TriggerConfig _trigger_config;
    pv::core::CursorRegistry _cursors;
};

} // namespace

class TestCaptureManager : public QObject {
    Q_OBJECT
private:
    // Build helper: fresh EventBus (FakeDispatcher) + MockSessionState +
    // CaptureManager wiring the same object as both state and coord.
    // Use a regular struct so each test starts from pristine state.
    struct Harness {
        EventBus bus{std::make_unique<FakeDispatcher>()};
        pv::core::Subscription _subscriber;
        MockSessionState state;
        std::unique_ptr<CaptureManager> cm;
        Harness() {
            _subscriber = bus.subscribe<DummyEvent>([](const DummyEvent &) {});
            rebuild();
        }
        void rebuild() { cm = std::make_unique<CaptureManager>(&bus, &state, &state); }
    };

private slots:
    void DefaultCollectModeIsSingle();
    void CollectModeRepeatRoundTrip();
    void CollectModeLoopRoundTrip();
    void DataLockRoundTrip();
    void DataAutoLockIncrementsAndClamps();
    void StoreConfirmFlagBaseline();
    void InstantFlagRoundTrip();
    void StreamModeFlagRoundTrip();
    void IsRepeatingDependsOnInstantAndMode();
    void IsRealtimeRefreshGatesOnWorking();
    void RepeatIntervalRoundTrip();
    void CaptureStatusBaseline();
    void CaptureStatusReportsTriggeredFlag();
    void StartCaptureEarlyExitWithoutSignals();
    void StopCaptureEarlyExitWhenNotWorking();
    void DsoPacketCount();
};

void TestCaptureManager::DefaultCollectModeIsSingle() {
    Harness h;
    QVERIFY(h.cm->is_single_mode());
    QVERIFY(!h.cm->is_repeat_mode());
    QVERIFY(!h.cm->is_loop_mode());
    QCOMPARE(h.cm->get_collect_mode(), COLLECT_SINGLE);
}

void TestCaptureManager::CollectModeRepeatRoundTrip() {
    Harness h;
    h.cm->set_collect_mode(COLLECT_REPEAT);
    QVERIFY(!h.cm->is_single_mode());
    QVERIFY(h.cm->is_repeat_mode());
    QVERIFY(!h.cm->is_loop_mode());
    QCOMPARE(h.cm->get_collect_mode(), COLLECT_REPEAT);
}

void TestCaptureManager::CollectModeLoopRoundTrip() {
    Harness h;
    h.cm->set_collect_mode(COLLECT_LOOP);
    QVERIFY(!h.cm->is_single_mode());
    QVERIFY(!h.cm->is_repeat_mode());
    QVERIFY(h.cm->is_loop_mode());
    QCOMPARE(h.cm->get_collect_mode(), COLLECT_LOOP);
}

void TestCaptureManager::DataLockRoundTrip() {
    Harness h;
    QVERIFY(!h.cm->is_data_lock());       // unlocked by default
    h.cm->data_lock();
    QVERIFY(h.cm->is_data_lock());
    h.cm->data_unlock();
    QVERIFY(!h.cm->is_data_lock());
}

void TestCaptureManager::DataAutoLockIncrementsAndClamps() {
    Harness h;
    QVERIFY(!h.cm->get_data_auto_lock());
    h.cm->data_auto_lock(3);
    QVERIFY(h.cm->get_data_auto_lock());
    h.cm->data_auto_unlock();       // 3 -> 2
    QVERIFY(h.cm->get_data_auto_lock());
    h.cm->data_auto_unlock();       // 2 -> 1
    QVERIFY(h.cm->get_data_auto_lock());
    h.cm->data_auto_unlock();       // 1 -> 0
    QVERIFY(!h.cm->get_data_auto_lock());
    // A second unlock below 0 clamps to 0, stays false.
    h.cm->data_auto_unlock();
    QVERIFY(!h.cm->get_data_auto_lock());

    // Negative value: decoding a negative lock clamps to 0 on first unlock.
    Harness h2;
    h2.cm->data_auto_lock(-2);
    QVERIFY(h2.cm->get_data_auto_lock()); // -2 != 0
    h2.cm->data_auto_unlock();            // val<0 -> 0
    QVERIFY(!h2.cm->get_data_auto_lock());
}

void TestCaptureManager::StoreConfirmFlagBaseline() {
    // is_first_store_confirm() returns true only after a SUCCESSFUL capture
    // bumps _work_time_id; capturemanager gives us no setter for it. Offline
    // we can only reach the equal (work==confirm) baseline, which must be false
    // and must remain false after clear_store_confirm_flag().
    Harness h;
    QVERIFY(!h.cm->is_first_store_confirm());
    h.cm->clear_store_confirm_flag(); // confirm := work (0), still equal
    QVERIFY(!h.cm->is_first_store_confirm());
}

void TestCaptureManager::InstantFlagRoundTrip() {
    Harness h;
    QVERIFY(!h.cm->is_instant());
    h.cm->set_is_instant(true);
    QVERIFY(h.cm->is_instant());
    h.cm->set_is_instant(false);
    QVERIFY(!h.cm->is_instant());
}

void TestCaptureManager::StreamModeFlagRoundTrip() {
    Harness h;
    QVERIFY(!h.cm->is_stream_mode());
    h.cm->set_is_stream_mode(true);
    QVERIFY(h.cm->is_stream_mode());
    h.cm->set_is_stream_mode(false);
    QVERIFY(!h.cm->is_stream_mode());
}

void TestCaptureManager::IsRepeatingDependsOnInstantAndMode() {
    Harness h;
    h.cm->set_collect_mode(COLLECT_REPEAT);
    h.cm->set_is_instant(false);
    QVERIFY(h.cm->is_repeating());   // repeat && !instant
    h.cm->set_is_instant(true);
    QVERIFY(!h.cm->is_repeating());  // instant suppresses repeat
}

void TestCaptureManager::IsRealtimeRefreshGatesOnWorking() {
    Harness h;
    // Not working by default => no realtime refresh regardless of mode/stream.
    QVERIFY(!h.cm->is_realtime_refresh());

    // Loop mode + stream: still gated by is_working.
    h.cm->set_collect_mode(COLLECT_LOOP);
    h.cm->set_is_stream_mode(true);
    QVERIFY(!h.cm->is_realtime_refresh());
    h.state.setWorking(true);        // now a live capture exists
    QVERIFY(h.cm->is_realtime_refresh()); // loop mode => true
}

void TestCaptureManager::RepeatIntervalRoundTrip() {
    Harness h;
    h.cm->set_repeat_intvl(123.5);
    QCOMPARE(h.cm->get_repeat_intvl(), 123.5);
    h.cm->set_repeat_intvl(0.1);
    QCOMPARE(h.cm->get_repeat_intvl(), 0.1);
}

void TestCaptureManager::CaptureStatusBaseline() {
    Harness h;
    bool triggered = true;
    int progress = -1;
    // cur_samplelimits()==0 (fresh) => always true, progress forced to 0.
    QVERIFY(h.cm->get_capture_status(triggered, progress));
    QVERIFY(!triggered);
    QCOMPARE(progress, 0);
}

void TestCaptureManager::CaptureStatusReportsTriggeredFlag() {
    Harness h;
    h.state.setTriged(true);
    h.state.setCurSampleLimits(100);
    bool triggered = false;
    int progress = -1;
    QVERIFY(h.cm->get_capture_status(triggered, progress));
    QVERIFY(triggered);             // propagated from the is_triged flag
    // Fresh capture snapshot has 0 samples => 0/100 progress. We assert the
    // no-sample progress floor rather than fabricate a sample count.
    QCOMPARE(progress, 0);
}

void TestCaptureManager::StartCaptureEarlyExitWithoutSignals() {
    Harness h;
    // action_start_capture returns false early: signal_models() is empty
    // (no signals configured), before any device/session work. This is the
    // offline-touchable gate of the capture-start lifecycle.
    QVERIFY(!h.cm->start_capture(false));
    QVERIFY(!h.cm->is_action()); // _is_action reset after the action wrapper
}

void TestCaptureManager::StopCaptureEarlyExitWhenNotWorking() {
    Harness h;
    // action_stop_capture returns false immediately when not working.
    QVERIFY(!h.cm->stop_capture());
}

void TestCaptureManager::DsoPacketCount() {
    Harness h;
    QCOMPARE(h.cm->dso_packet_count(), 0ull);
    h.cm->inc_dso_packet_count();
    h.cm->inc_dso_packet_count();
    QCOMPARE(h.cm->dso_packet_count(), 2ull);
}

QTEST_MAIN(TestCaptureManager)
#include "test_capture_manager.moc"