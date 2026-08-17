/*
 * test_decode_task_manager.cpp — QTest unit tests for DecodeTaskManager
 *
 * Spec harden-review-findings, T8.
 *
 * Focus: the decode thread-pool harness owned by DecodeTaskManager —
 *   * de-dup on add_decode_task (a stack already registered in _running_tasks
 *     must not be enqueued twice),
 *   * the add -> worker executes decode_single_task -> dequeue lifecycle
 *     (running set returns to empty, completion fired exactly once),
 *   * stop() is idempotent and clears the running set,
 *   * clear_all_decode_task() empties the running set and resets the index.
 *
 * Reasons you cannot merely substitute a "fake" stack here:
 *   DecodeTaskManager operates on the CONCRETE type
 *   std::shared_ptr<pv::data::DecoderStack>, and DecoderStack::begin_decode_work() /
 *   stop_decode_work() are NOT virtual. There is no lightweight injectable seam, so we
 *   must construct a real DecoderStack. Its constructor requires a real libsigrokdecode
 *   decoder plus an owned DecoderStatus, so those tests are SKIPped when the decoder
 *   DLL is unavailable (e.g. when the test binary is not deployed next to libsigrokdecode).
 *
 * We deliberately DO NOT run a real decode session: the freshly-constructed stack is
 * left with _options_changed == false, so DecoderStack::do_decode_work() takes the
 * "quick return" path right after _decoder_status->clear() and never touches the
 * snapshot machines. That keeps the lifecycle deterministic and dependency-light while
 * still exercising the real worker thread + erase + completion path.
 *
 * For the de-dup test, the stack's decoder list is cleared and _options_changed is set
 * true so do_decode_work() advances to the point where it blocks on
 * ISessionHost::signal_models_mutex(). The test holds that mutex (shared) exclusively,
 * guaranteeing the worker stays parked inside _running_tasks while the duplicate
 * add_decode_task() is issued — making the de-dup determination deterministic instead
 * of racing a fast task.
 */

#include <QtTest>

#include <QCoreApplication>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <libsigrokdecode.h>

#include "pv/core/decodetaskmanager.h"
#include "pv/core/documentregistry.h"
#include "pv/core/eventbus.h"
#include "pv/core/iasync_dispatcher.h"
#include "pv/core/isession_state.h"
#include "pv/core/thread_pool.h"

#include "pv/data/isession_host.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/data/stack/decoderstack.h"

// pxv_log is provided by pxview-core's log.cpp (which this test links via the
// pxview-core static lib), so it is NOT re-defined here — a duplicate symbol
// would otherwise arise against pxview-core. Identical pattern to
// test_capture_manager.cpp.

// ---- Link shims for QColor::fromString / QColor::lightness ----
// Identical to test_capture_manager: pxview-config's AppConfig theme helpers
// reference these Qt6Gui imports via __imp__ indirection, but pxview-config.a
// is scanned AFTER the Qt6Gui import library in the link order, so the __imp__
// slots stay undefined under MinGW. These null slots are never dereferenced
// by the offline paths under test.
void *__imp__ZN6QColor10fromStringE14QAnyStringView = nullptr;
void *__imp__ZNK6QColor10lightnessFEv = nullptr;

namespace {

using pv::core::DecodeTaskManager;
using pv::core::DocumentRegistry;
using pv::core::EventBus;
using pv::core::IAsyncDispatcher;
using pv::data::DecoderStack;
using pv::data::SignalModel;

// ---------------------------------------------------------------------------
// Injectable fake async dispatcher (no real Qt event loop needed).
// ---------------------------------------------------------------------------
class FakeDispatcher : public IAsyncDispatcher {
public:
    void post(std::function<void()> fn) override {
        (void)fn;
        ++post_count; // DecodeDone / other async events coalesce here
    }
    bool on_target_thread() const override { return true; }
    int post_count = 0;
};

// ---------------------------------------------------------------------------
// ISessionHost stub — gives the DecoderStack just enough to construct.
// signal_models_mutex() returns a REAL shared_mutex so a test can hold it to
// deterministically park a decode worker inside do_decode_work().
// ---------------------------------------------------------------------------
class StubHost : public pv::data::ISessionHost {
public:
    std::shared_mutex sig_mutex;
    std::vector<std::shared_ptr<SignalModel>> sig_models;

    int event_post_count = 0;

    void event_bus_post(std::function<void()> fn) override {
        (void)fn;
        ++event_post_count;
    }
    std::shared_mutex &signal_models_mutex() override { return sig_mutex; }
    std::vector<std::shared_ptr<SignalModel>> &get_signal_models() override {
        return sig_models;
    }
    bool is_realtime_refresh() override { return false; }
    bool is_closed() override { return false; }
    int64_t get_ring_sample_count() override { return 0; }
};

// ---------------------------------------------------------------------------
// StubSession — implements ISessionState (which extends ISessionCoordination)
// with safe no-op/empty defaults. Only the handful of methods actually touched
// by the tested DecodeTaskManager paths carry interesting state
// (decode_complete_count / signals_changed / data_updated). Everything else is
// simply never called by these tests.
// ---------------------------------------------------------------------------
class StubSession : public pv::ISessionState {
public:
    int decode_complete_count = 0;
    int data_updated_count = 0;
    int signals_changed_count = 0;
    pv::core::DocumentRegistry *doc_reg = nullptr;

    void set_doc_reg(pv::core::DocumentRegistry *d) { doc_reg = d; }

    // --- coordination / notification (ISessionCoordination half) ---
    void clear_all_decode_task2() override {}
    void add_decode_task(std::shared_ptr<DecoderStack>) override {}
    void attach_data_to_signal(pv::SessionData *) override {}
    void sync_trigger_to_libsigrok(bool) override {}
    void clear_glitch_filter_state_for_capture() override {}
    uint16_t get_ch_num(int) override { return 0; }
    uint64_t cur_samplelimits() override { return 0; }
    uint64_t cur_snap_samplerate() override { return 0; }
    void set_cur_snap_samplerate(uint64_t) override {}
    void set_cur_samplelimits(uint64_t) override {}
    void data_updated() override { ++data_updated_count; }
    void signals_changed() override { ++signals_changed_count; }
    void update_capture() override {}
    void receive_header() override {}
    void receive_trigger(uint64_t) override {}
    void frame_began() override {}
    void session_error() override {}
    void set_trigger_flag(bool) override {}
    void set_trigger_ch(int) override {}
    void set_hw_replied(bool) override {}
    void set_receive_data_len(uint64_t) override {}
    void set_capture_data(pv::SessionData *) override {}
    void set_session_time(QDateTime) override {}
    void set_is_working(bool) override {}
    void set_is_triged(bool) override {}
    void set_trig_time(QDateTime) override {}
    void set_error(int) override {}
    bool bClose() const override { return false; }

    // --- manager back-pointers ---
    pv::core::DataFeedParser *data_feed_parser() override { return nullptr; }
    pv::core::DocumentRegistry *document_registry() override { return doc_reg; }
    pv::core::FilterProcessor *filter_processor() override { return nullptr; }

    // --- mutexes ---
    std::mutex data_mtx, sampling_mtx;
    std::mutex &data_mutex() override { return data_mtx; }
    std::mutex &sampling_mutex() override { return sampling_mtx; }

    // --- business objects ---
    std::vector<std::shared_ptr<SignalModel>> sig_models;
    std::vector<std::shared_ptr<pv::data::SpectrumStack>> spectrum_stacks_mem;
    std::vector<std::shared_ptr<DecoderStack>> trace_stack;
    std::shared_ptr<pv::data::MathStack> math;

    std::vector<std::shared_ptr<SignalModel>> &signal_models() override {
        return sig_models;
    }
    std::vector<std::shared_ptr<SignalModel>> signal_models_snapshot() override {
        return sig_models;
    }
    std::vector<std::shared_ptr<pv::data::SpectrumStack>> &spectrum_stacks() override {
        return spectrum_stacks_mem;
    }
    pv::data::LissajousModel *lissajous_model() const override { return nullptr; }
    void set_lissajous_model(std::unique_ptr<pv::data::LissajousModel>) override {}
    const std::shared_ptr<pv::data::MathStack> &math_stack() const override {
        return math;
    }
    void set_math_stack(std::shared_ptr<pv::data::MathStack> m) override { math = m; }

    // --- time ---
    QDateTime session_time() const override { return QDateTime(); }
    QDateTime trig_time() const override { return QDateTime(); }

    // --- bool state ---
    bool is_triged() const override { return false; }
    bool trigger_flag() const override { return false; }
    bool hw_replied() const override { return false; }
    void set_bClose(bool) override {}
    bool is_saving() const override { return false; }
    void set_saving(bool) override {}
    bool is_working() const override { return false; }
    bool is_single_buffer() const override { return false; }

    // --- numeric state ---
    int trigger_ch() const override { return 0; }
    uint64_t error_pattern() const override { return 0; }
    void set_error_pattern(uint64_t) override {}
    uint64_t save_start() const override { return 0; }
    void set_save_start(uint64_t) override {}
    uint64_t save_end() const override { return 0; }
    void set_save_end(uint64_t) override {}
    int map_zoom() const override { return 0; }
    void set_map_zoom(int) override {}
    int device_status() const override { return 0; }
    void set_device_status(int) override {}

    // --- device ---
    // Never dereferenced by any path under test (device_agent() is not part of
    // the DecodeTaskManager contract it calls). The returned reference is only
    // materialized to satisfy a pure-virtual; constructing a real DeviceAgent
    // would pull the whole libsigrok device stack into the test. NOTE: the base
    // ISessionState declares it in the pv namespace, but DeviceAgent itself is
    // a GLOBAL-class (deviceagent.h), so the override must use ::DeviceAgent.
    ::DeviceAgent &device_agent() override {
        return *reinterpret_cast<::DeviceAgent *>(static_cast<uintptr_t>(0x1));
    }

    // --- data buffers ---
    pv::SessionData *view_data_mem = nullptr;
    std::vector<std::unique_ptr<pv::SessionData>> data_list_mem;

    pv::SessionData *view_data() override { return view_data_mem; }
    void set_view_data(pv::SessionData *d) override { view_data_mem = d; }
    pv::SessionData *capture_data() override { return nullptr; }
    std::vector<std::unique_ptr<pv::SessionData>> &data_list() override {
        return data_list_mem;
    }

    // --- trigger config ---
    pv::data::TriggerConfig trigger_cfg;
    const pv::data::TriggerConfig &trigger_config() const override { return trigger_cfg; }
    void set_trigger_config(const pv::data::TriggerConfig &c) override { trigger_cfg = c; }

    // --- cursor registry ---
    pv::core::CursorRegistry cursor_reg;
    pv::core::CursorRegistry &cursor_registry() override { return cursor_reg; }
    const pv::core::CursorRegistry &cursor_registry() const override { return cursor_reg; }

    // --- decode-stack helpers ---
    std::vector<std::shared_ptr<DecoderStack>> &decode_traces(pv::data::SessionDocument * = nullptr) override {
        return trace_stack;
    }
    std::vector<std::shared_ptr<DecoderStack>> &get_decoder_stacks(pv::data::SessionDocument * = nullptr) override {
        return trace_stack;
    }
    std::shared_ptr<DecoderStack> get_decoder_trace(int, pv::data::SessionDocument * = nullptr) override {
        return nullptr;
    }
    int get_trace_index_by_key_handel(void *handel, pv::data::SessionDocument * = nullptr) override {
        (void)handel;
        return -1;
    }

    // --- misc ---
    void cur_snap_samplerate_changed() override {}
    void frame_ended() override {}
    void repeat_hold(int) override {}
    void show_wait_trigger() override {}
    void delay_prop_msg(QString) override {}
    uint64_t next_decoder_handle_id() override { return 0; }

    // --- SharedState signaling (Phase 3) ---
    void notify_decode_complete() override { ++decode_complete_count; }
    void reset_capture_complete() override {}
};

// ---------------------------------------------------------------------------
// Small wait helpers (bounded polls so a regression never loops forever).
// ---------------------------------------------------------------------------
bool wait_running(DecodeTaskManager &mgr, int timeout_ms = 3000) {
    const auto start = std::chrono::steady_clock::now();
    while (!mgr.has_running_tasks()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count() > timeout_ms)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

bool wait_idle(DecodeTaskManager &mgr, int timeout_ms = 3000) {
    const auto start = std::chrono::steady_clock::now();
    while (mgr.has_running_tasks()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count() > timeout_ms)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Lazily-resolved libsigrokdecode decoder (PDs are Python-backed here;
// if the Python runtime / i2c decoder cannot be loaded the stack-based tests
// are skipped rather than failing).
struct srd_decoder *g_dec = nullptr;

} // namespace

class TestDecodeTaskManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void AddDecodeTaskRunsCompletesAndDequeues();
    void AddNullStackIsNoOp();
    void DeduplicatedAddWhileRegistered();
    void StopClearsRunningSetAndIsIdempotent();
    void ClearAllDecodeTaskEmptiesAndResetsIndex();
    void cleanupTestCase();
};

void TestDecodeTaskManager::initTestCase() {
    g_dec = nullptr;
    // The test executable lives in build.dir, right beside the deployed
    // `decoders/` (Python PDs) + `decoders/c_decoders/` (C DLLs) trees. This
    // fork of libsigrokdecode requires an explicit srd_init(<search-path>) +
    // srd_decoder_load(<id>) (matching decoder_test.c of libsigrokdecode); the
    // upstream-style srd_init(nullptr) alone never registers any decoder, so
    // g_dec would stay null and every DecoderStack test would QSKIP.
    const QString decodersDir =
        QCoreApplication::applicationDirPath() + QStringLiteral("/decoders");
    if (srd_init(qPrintable(decodersDir)) != SRD_OK) {
        qWarning() << "libsigrokdecode srd_init failed for" << decodersDir
                   << "; DecoderStack-based tests will be skipped.";
        return;
    }
    srd_decoder_load("i2c");
    g_dec = srd_decoder_get_by_id("i2c");
    if (!g_dec)
        qWarning() << "libsigrokdecode 'i2c' decoder unavailable; "
                      "DecoderStack-based tests will be skipped.";
}

void TestDecodeTaskManager::cleanupTestCase() {
    srd_exit();
}

std::shared_ptr<DecoderStack> make_stack(StubHost &host)
{
    if (!g_dec)
        return nullptr;
    return std::make_shared<DecoderStack>(&host, g_dec,
                                          new DecoderStatus());
}

void TestDecodeTaskManager::AddDecodeTaskRunsCompletesAndDequeues() {
    if (!g_dec)
        QSKIP("libsigrokdecode decoder unavailable");

    StubHost host;
    auto stack = make_stack(host);
    QVERIFY(stack);

    auto disp = std::make_unique<FakeDispatcher>();
    FakeDispatcher *disp_raw = disp.get();
    EventBus bus(std::move(disp));
    StubSession state;
    DocumentRegistry reg(&bus, &state, &state);
    state.set_doc_reg(&reg);
    DecodeTaskManager mgr(&bus, &state, &state);

    QVERIFY(!mgr.has_running_tasks());

    // add_decode_task registers the stack and submits it to the worker pool.
    // NOTE: we do NOT assert is_task_running(stack) immediately after — the
    // quick-return decode path can finish a few microseconds later, before the
    // assertion runs, which would make it flaky. The deterministic observable
    // is the add -> run -> dequeue -> completion lifecycle below.
    mgr.add_decode_task(stack);

    // ...the worker thread runs decode_single_task -> begin_decode_work (quick
    // return path) -> dequeue -> completion. Exactly one completion fires.
    QVERIFY(wait_idle(mgr));
    QVERIFY(!mgr.has_running_tasks());
    QVERIFY(!mgr.is_task_running(stack));
    QCOMPARE(state.decode_complete_count, 1);
    // view_data is null -> the decode_end() leg is skipped, so no crash there.
    // Async DecodeDone was posted to the (fake) dispatcher.
    QVERIFY(disp_raw->post_count >= 1);
}

void TestDecodeTaskManager::AddNullStackIsNoOp() {
    auto disp = std::make_unique<FakeDispatcher>();
    EventBus bus(std::move(disp));
    StubSession state;
    DocumentRegistry reg(&bus, &state, &state);
    state.set_doc_reg(&reg);
    DecodeTaskManager mgr(&bus, &state, &state);

    // attach_data_to_signal(nullptr) short-circuits, and the null stack must
    // not be pushed into _running_tasks.
    mgr.add_decode_task(nullptr);
    QVERIFY(!mgr.has_running_tasks());
    QVERIFY(state.data_updated_count == 0); // attach returned before notify
}

void TestDecodeTaskManager::DeduplicatedAddWhileRegistered() {
    if (!g_dec)
        QSKIP("libsigrokdecode decoder unavailable");

    StubHost host;
    auto stack = make_stack(host);
    QVERIFY(stack);

    auto disp = std::make_unique<FakeDispatcher>();
    EventBus bus(std::move(disp));
    StubSession state;
    DocumentRegistry reg(&bus, &state, &state);
    state.set_doc_reg(&reg);
    DecodeTaskManager mgr(&bus, &state, &state);

    // Empty the decoder list and flip _options_changed so do_decode_work()
    // walks past its early return and blocks on host.signal_models_mutex().
    // Holding that mutex parks the worker inside decode_single_task, keeping
    // `stack` registered in _running_tasks for the deterministic duplicate add.
    stack->stack().clear();
    stack->set_options_changed(true);

    {
        // Exclusive lock: the worker's shared_lock in do_decode_work() blocks
        // here, so the stack stays in _running_tasks until we release it.
        std::unique_lock<std::shared_mutex> lk(host.sig_mutex);

        mgr.add_decode_task(stack);
        QVERIFY(wait_running(mgr));
        QVERIFY(mgr.is_task_running(stack));

        // Duplicate add: the running-set guard must see `stack` and skip the
        // second submission (stays registered, single registration preserved).
        mgr.add_decode_task(stack);
        QVERIFY(mgr.is_task_running(stack));
        QVERIFY(mgr.has_running_tasks());
    } // release the mutex -> the parked worker resumes and finishes

    QVERIFY(wait_idle(mgr));
    QVERIFY(!mgr.has_running_tasks());
    QVERIFY(!mgr.is_task_running(stack));
    QCOMPARE(state.decode_complete_count, 1);

    // NOTE: the duplicate submission count is not directly observable through
    // the public API (the pool task queue / _running_tasks haven't got a
    // public size). The above asserts that the duplicated add under a
    // registered stack is a harmless no-op that leaves a single healthy run,
    // which is the observable contract of the de-dup guard.
}

void TestDecodeTaskManager::StopClearsRunningSetAndIsIdempotent() {
    if (!g_dec)
        QSKIP("libsigrokdecode decoder unavailable");

    StubHost host;
    auto stack = make_stack(host);
    QVERIFY(stack);

    auto disp = std::make_unique<FakeDispatcher>();
    EventBus bus(std::move(disp));
    StubSession state;
    DocumentRegistry reg(&bus, &state, &state);
    state.set_doc_reg(&reg);
    DecodeTaskManager mgr(&bus, &state, &state);

    // (a) stop() while a task is registered: requests stop on it, joins the
    // worker, and clears the running set.
    mgr.add_decode_task(stack);
    // The worker may already have finished; either way the set ends empty.
    mgr.stop();
    QVERIFY(!mgr.has_running_tasks());
    QVERIFY(!mgr.is_task_running(stack));

    // (b) stop() with nothing running and (c) a second stop(): both no-op.
    mgr.stop();
    mgr.stop();
    QVERIFY(!mgr.has_running_tasks());
}

void TestDecodeTaskManager::ClearAllDecodeTaskEmptiesAndResetsIndex() {
    if (!g_dec)
        QSKIP("libsigrokdecode decoder unavailable");

    StubHost host;
    auto stack = make_stack(host);
    QVERIFY(stack);

    auto disp = std::make_unique<FakeDispatcher>();
    EventBus bus(std::move(disp));
    StubSession state;
    DocumentRegistry reg(&bus, &state, &state);
    state.set_doc_reg(&reg);
    DecodeTaskManager mgr(&bus, &state, &state);

    // Empty registry: iterates zero documents, resets index to -1, waits for
    // the (empty) pool, clears _running_tasks. Must neither throw nor run.
    int dex = -7;
    mgr.clear_all_decode_task(dex);
    QCOMPARE(dex, -1);
    QVERIFY(!mgr.has_running_tasks());

    // With a task queued, clear_all stops it and empties the running set.
    mgr.add_decode_task(stack);
    dex = 123;
    mgr.clear_all_decode_task(dex);
    QCOMPARE(dex, -1);
    QVERIFY(!mgr.has_running_tasks());
    QVERIFY(!mgr.is_task_running(stack));

    // clear_all_decode_task2() forwards to the 1-arg form without crashing.
    mgr.clear_all_decode_task2();
    QVERIFY(!mgr.has_running_tasks());
}

QTEST_MAIN(TestDecodeTaskManager)
#include "test_decode_task_manager.moc"