/*
 * test_session_document.cpp — QTest unit tests for the data-layer "document
 * trio": SessionData / SessionDocument / SessionSnapshot.
 *
 * Covers:
 *   - SessionData: default-empty snapshots, clear() re-creation, shared/raw
 *     pointer identity, public filter/state field access.
 *   - SessionDocument data surface: samplerate / samplelimits / trigger_pos /
 *     sampletime / has_data / empty.
 *   - Zero-copy sharing (share_from_*) + deferred release (_pending_*)
 *     semantics.
 *   - DataSource empty-implementation surface (get_signal_models /
 *     get_spectrum_stacks / get_math_stack / get_lissajous_model / cur_* /
 *     is_running_status falling back to the DataSource default).
 *   - decoder_stack container add/remove (real DecoderStack; QSKIP when the
 *     libsigrokdecode "uart" decoder is not deployed).
 *   - trigger_config set/get + signal_config_to_json / signal_config_from_json
 *     round trip (mock IDeviceConfigPort).
 *   - clear().
 *   - SessionSnapshot basics (set_samplerate lazily allocates snapshots,
 *     copy_from_* deep copy, get_snapshot type routing).
 */

#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QString>

// ── Standard headers must be included before the xlog stub ──
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include <libsigrok/libsigrok.h>   // sr_datafeed_logic / sr_channel / GSList
#include <libsigrokdecode.h>       // srd_init / srd_decoder_get_by_id

// pxv_log is provided by pxview-core's log.cpp (which this test links via the
// pxview-core static lib), and xlog_* by common/log/xlog.c compiled into this
// target — so they are NOT re-defined here (a duplicate symbol would otherwise
// arise). Identical pattern to test_capture_manager / test_decode_task_manager.

// ---- Link shims for QColor::fromString / QColor::lightness ----
// Identical to test_decode_task_manager: pxview-config's AppConfig theme helpers
// reference these Qt6Gui imports via __imp__ indirection, but pxview-config.a
// is scanned AFTER the Qt6Gui import library in the link order, so the __imp__
// slots stay undefined under MinGW. These null slots are never dereferenced by
// the offline paths under test.
void *__imp__ZN6QColor10fromStringE14QAnyStringView = nullptr;
void *__imp__ZNK6QColor10lightnessFEv = nullptr;

#include "pv/data/idevice_config_port.h"
#include "pv/data/document/sessiondata.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/document/sessionsnapshot.h"
#include "pv/data/isession_host.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/data/stack/decoderstack.h"

using pv::data::AnalogSnapshot;
using pv::data::ChannelConfig;
using pv::data::DecoderStack;
using pv::data::DsoSnapshot;
using pv::data::IDeviceConfigPort;
using pv::data::ISessionHost;
using pv::data::LogicSnapshot;
using pv::SessionData;
using pv::data::SessionDocument;
using pv::data::SessionSnapshot;
using pv::data::SignalModel;
using pv::data::TriggerConfig;

// ---------------------------------------------------------------------------
// Minimal IDeviceConfigPort implementation — SessionDocument's constructor
// requires one; SignalConfigStore reaches the device config API through it.
// Every pure virtual returns a benign stub value (no device attached).
// ---------------------------------------------------------------------------
class MockDeviceConfigPort : public IDeviceConfigPort {
public:
    int work_mode = 0;

    // --- Instance / mode ---
    bool have_instance() const override { return true; }
    int get_work_mode() override { return work_mode; }
    void set_work_mode(int mode) override { work_mode = mode; }
    bool is_demo() const override { return false; }
    QString get_demo_operation_mode() override { return QString(); }

    // --- Channel enumeration / enable ---
    GSList *get_channels() override { return nullptr; }
    bool enable_probe(const sr_channel *probe, bool enable) override {
        (void)probe; (void)enable; return true;
    }

    // --- Typed config get/set (subset used by data layer) ---
    bool get_config_string(int, QString &, const sr_channel *,
                           const sr_channel_group *) override { return false; }
    bool set_config_string(int, const char *, const sr_channel *,
                           const sr_channel_group *) override { return true; }
    bool get_config_bool(int, bool &, const sr_channel *,
                         const sr_channel_group *) override { return false; }
    bool set_config_bool(int, bool, const sr_channel *,
                         const sr_channel_group *) override { return true; }
    bool set_config_uint16(int, int, const sr_channel *,
                           const sr_channel_group *) override { return true; }
    bool set_config_int32(int, int, const sr_channel *,
                          const sr_channel_group *) override { return true; }
    bool set_config_uint64(int, uint64_t, const sr_channel *,
                           const sr_channel_group *) override { return true; }
};

// ---------------------------------------------------------------------------
// Minimal ISessionHost — needed only to construct a real DecoderStack for the
// container add/remove test (the stack never runs a decode session).
// ---------------------------------------------------------------------------
class StubHost : public ISessionHost {
public:
    std::shared_mutex sig_mutex;
    std::vector<std::shared_ptr<SignalModel>> sig_models;

    void event_bus_post(std::function<void()>) override {}
    std::shared_mutex &signal_models_mutex() override { return sig_mutex; }
    std::vector<std::shared_ptr<SignalModel>> &get_signal_models() override {
        return sig_models;
    }
    bool is_realtime_refresh() override { return false; }
    bool is_closed() override { return false; }
    int64_t get_ring_sample_count() override { return 0; }
    void request_decode_notify(std::weak_ptr<DecoderStack>) override {}
};

namespace {

// Feed interleaved (LA_SPLIT_DATA, unitsize=1) samples into a LogicSnapshot so
// it is non-empty. Mirrors the Fixture in test_logic_snapshot_raw.cpp.
struct LogicFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    std::vector<uint8_t> data;
    sr_datafeed_logic logic{};

    LogicFixture(size_t ch_count, size_t total_samples)
        : chs(ch_count), nodes(ch_count) {
        for (size_t i = 0; i < ch_count; ++i) {
            chs[i].index = (int)i;
            chs[i].type = SR_CHANNEL_LOGIC;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < ch_count) ? &nodes[i + 1] : nullptr;
        }
        data.assign(total_samples, 0);
        for (size_t s = 0; s < total_samples; ++s)
            data[s] = (uint8_t)(s & 0x03u);
        logic.length = 0;
        logic.unitsize = (uint8_t)((ch_count + 7) / 8);
        logic.format = 0; // LA_SPLIT_DATA (interleaved)
        logic.data = nullptr;
    }

    void feed(LogicSnapshot &snap, uint64_t total_sample_count) {
        sr_datafeed_logic l = logic;
        l.length = (uint64_t)data.size();
        l.data = data.data();
        l.unitsize = 1;
        snap.first_payload(l, total_sample_count, &nodes[0], true);
        snap.append_payload(l);
        snap.capture_ended();
    }
};

std::shared_ptr<LogicSnapshot> make_logic_snapshot(uint64_t samples) {
    auto snap = std::make_shared<LogicSnapshot>();
    LogicFixture fx(2, (size_t)samples);
    fx.feed(*snap, samples);
    return snap;
}

} // anonymous namespace

class TestSessionDocument : public QObject {
    Q_OBJECT

private slots:
    // --- SessionData ---
    void SessionDataDefaultEmpty();
    void SessionDataClearCreatesNewSnapshots();
    void SessionDataSharedMatchesRaw();
    void SessionDataFieldsAccessible();

    // --- SessionDocument data surface ---
    void DocumentInitiallyEmpty();
    void DocumentSamplerateRoundTrip();
    void DocumentSamplelimitsRoundTrip();
    void DocumentTriggerPosRoundTrip();
    void DocumentSampletimeComputed();

    // --- zero-copy sharing + deferred release ---
    void ActivePointersNullInitially();
    void ShareFromLogicIsZeroCopy();
    void ShareFromAnalogAndDso();
    void ShareDefersOldSnapshot();

    // --- DataSource empty-implementation surface ---
    void EmptyStubCollections();
    void CurSnapValuesMatchSetters();
    void RunningStatusDefaultsFalse();

    // --- decoder stack container ---
    void DecoderStackContainerAddRemove();

    // --- trigger config + JSON round trip ---
    void TriggerConfigRoundTrip();
    void SignalConfigJsonRoundTrip();

    // --- clear() ---
    void ClearResetsEverything();

    // --- SessionSnapshot (basic) ---
    void SessionSnapshotBasics();
    void SessionSnapshotSetSamplerateCreatesSnapshots();
    void SessionSnapshotCopyFromLogic();

    void initTestCase();
    void cleanupTestCase();

private:
    const srd_decoder *g_dec = nullptr;
    bool _srd_inited = false;
};

// ---- libsigrokdecode setup for the DecoderStack container test ----
// The test binary must be deployed next to the `decoders/` tree (Python PDs)
// + `decoders/c_decoders/` (C DLLs); srd_init + srd_decoder_load are required
// by this libsigrokdecode fork (matching test_decode_task_manager.cpp).
void TestSessionDocument::initTestCase() {
    const QString decodersDir =
        QCoreApplication::applicationDirPath() + QStringLiteral("/decoders");
    if (srd_init(qPrintable(decodersDir)) != SRD_OK) {
        qWarning() << "libsigrokdecode srd_init failed for" << decodersDir
                   << "; DecoderStack-based tests will be skipped.";
        return;
    }
    _srd_inited = true;
    srd_decoder_load("uart");
    g_dec = srd_decoder_get_by_id("uart");
    if (!g_dec)
        qWarning() << "libsigrokdecode 'uart' decoder unavailable; "
                      "DecoderStack-based tests will be skipped.";
}

void TestSessionDocument::cleanupTestCase() {
    if (_srd_inited)
        srd_exit();
}

// ---------------------------------------------------------------------------
// SessionData
// ---------------------------------------------------------------------------

void TestSessionDocument::SessionDataDefaultEmpty() {
    SessionData data;
    // Fresh snapshots exist but carry no samples.
    QVERIFY(data.get_logic() != nullptr);
    QVERIFY(data.get_analog() != nullptr);
    QVERIFY(data.get_dso() != nullptr);
    QVERIFY(data.get_logic()->empty());
    QVERIFY(data.get_analog()->empty());
    QVERIFY(data.get_dso()->empty());
    // Numeric/state fields default to zero/false.
    QCOMPARE(data._cur_snap_samplerate, uint64_t(0));
    QCOMPARE(data._cur_samplelimits, uint64_t(0));
    QCOMPARE(data._trig_pos, uint64_t(0));
    QVERIFY(!data._glitch_filter_active);
    QVERIFY(!data._signal_invert_active);
    QVERIFY(data._logic_backup == nullptr);
}

void TestSessionDocument::SessionDataClearCreatesNewSnapshots() {
    SessionData data;
    auto *a = data.get_logic();
    QVERIFY(a != nullptr);
    data.clear();
    auto *b = data.get_logic();
    QVERIFY(b != nullptr);
    QVERIFY(a != b); // fresh instance after clear()
    data.clear();
    auto *c = data.get_logic();
    QVERIFY(c != nullptr);
    QVERIFY(b != c); // yet another fresh instance
    // analog/dso are also re-created
    QVERIFY(data.get_analog() != nullptr);
    QVERIFY(data.get_dso() != nullptr);
}

void TestSessionDocument::SessionDataSharedMatchesRaw() {
    SessionData data;
    QVERIFY(data.logic_shared().get() == data.get_logic());
    QVERIFY(data.analog_shared().get() == data.get_analog());
    QVERIFY(data.dso_shared().get() == data.get_dso());
    // A copied shared_ptr keeps the same object alive.
    auto sp = data.logic_shared();
    QVERIFY(sp.get() == data.get_logic());
}

void TestSessionDocument::SessionDataFieldsAccessible() {
    SessionData data;
    data._cur_snap_samplerate = 1000000;
    data._cur_samplelimits = 8000;
    data._trig_pos = 4321;
    data._glitch_filter_active = true;
    data._signal_invert_active = true;
    data._glitch_filter_thresholds[0] = 3;
    QCOMPARE(data._cur_snap_samplerate, uint64_t(1000000));
    QCOMPARE(data._cur_samplelimits, uint64_t(8000));
    QCOMPARE(data._trig_pos, uint64_t(4321));
    QVERIFY(data._glitch_filter_active);
    QVERIFY(data._signal_invert_active);
    QCOMPARE(data._glitch_filter_thresholds[0], uint32_t(3));

    // clear() injects the current samplerate into the fresh snapshots, resets
    // trig_pos / active flags, and keeps the (config-like) samplerate fields.
    data.clear();
    QVERIFY(data.get_logic()->samplerate() == 1000000.0);
    QVERIFY(data.get_analog()->samplerate() == 1000000.0);
    QVERIFY(data.get_dso()->samplerate() == 1000000.0);
    QCOMPARE(data._cur_snap_samplerate, uint64_t(1000000)); // preserved
    QCOMPARE(data._trig_pos, uint64_t(0));                  // reset
    QVERIFY(!data._glitch_filter_active);
    QVERIFY(!data._signal_invert_active);
}

// ---------------------------------------------------------------------------
// SessionDocument data surface
// ---------------------------------------------------------------------------

void TestSessionDocument::DocumentInitiallyEmpty() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    QVERIFY(doc.empty());
    QVERIFY(!doc.has_data());
    QCOMPARE(doc.get_samplerate(), uint64_t(0));
    QCOMPARE(doc.get_samplelimits(), uint64_t(0));
    QCOMPARE(doc.get_trigger_pos(), uint64_t(0));
    QCOMPARE(doc.get_sampletime(), 0.0);
}

void TestSessionDocument::DocumentSamplerateRoundTrip() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    doc.set_samplerate(2000000);
    QCOMPARE(doc.get_samplerate(), uint64_t(2000000));
    doc.set_samplerate(0);
    QCOMPARE(doc.get_samplerate(), uint64_t(0));
}

void TestSessionDocument::DocumentSamplelimitsRoundTrip() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    doc.set_samplelimits(8192);
    QCOMPARE(doc.get_samplelimits(), uint64_t(8192));
}

void TestSessionDocument::DocumentTriggerPosRoundTrip() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    doc.set_trigger_pos(12345);
    QCOMPARE(doc.get_trigger_pos(), uint64_t(12345));
}

void TestSessionDocument::DocumentSampletimeComputed() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    // samplerate == 0 → sampletime 0
    doc.set_samplerate(0);
    QCOMPARE(doc.get_sampletime(), 0.0);
    // sampletime = samplelimits / samplerate
    doc.set_samplerate(1000);
    doc.set_samplelimits(500);
    QCOMPARE(doc.get_sampletime(), 0.5);
}

// ---------------------------------------------------------------------------
// Zero-copy sharing + deferred release
// ---------------------------------------------------------------------------

void TestSessionDocument::ActivePointersNullInitially() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    QVERIFY(doc.get_active_logic() == nullptr);
    QVERIFY(doc.get_active_analog() == nullptr);
    QVERIFY(doc.get_active_dso() == nullptr);
    // DataSource typed accessors route through the same nulls.
    QVERIFY(doc.get_logic_snapshot() == nullptr);
    QVERIFY(doc.get_analog_snapshot() == nullptr);
    QVERIFY(doc.get_dso_snapshot() == nullptr);
}

void TestSessionDocument::ShareFromLogicIsZeroCopy() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    auto snap = make_logic_snapshot(4096);
    QVERIFY(snap->get_sample_count() > 0);
    QVERIFY(!snap->empty());

    doc.set_samplerate(1000000);
    doc.share_from_logic(snap);
    // Zero-copy: document points at the very same object.
    QVERIFY(doc.get_active_logic() == snap.get());
    QVERIFY(doc.has_data());
    QVERIFY(!doc.empty());
    // The shared snapshot is stamped with the document samplerate.
    QVERIFY(doc.get_active_logic()->samplerate() == 1000000.0);
    // get_snapshot(SR_CHANNEL_LOGIC) routes to the same snapshot.
    QVERIFY(doc.get_snapshot(SR_CHANNEL_LOGIC) == snap.get());

    // clear_pending_release with no previous snapshot is a no-op.
    doc.clear_pending_release();
    QVERIFY(doc.get_active_logic() == snap.get());
    QVERIFY(doc.has_data());
}

void TestSessionDocument::ShareFromAnalogAndDso() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    auto a = std::make_shared<AnalogSnapshot>();
    auto d = std::make_shared<DsoSnapshot>();
    doc.share_from_analog(a);
    doc.share_from_dso(d);
    QVERIFY(doc.get_active_analog() == a.get());
    QVERIFY(doc.get_active_dso() == d.get());
    // Empty analog/dso snapshots do not satisfy has_data().
    QVERIFY(!doc.has_data());
    QVERIFY(doc.empty());
}

void TestSessionDocument::ShareDefersOldSnapshot() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    auto old = make_logic_snapshot(1024);
    auto fresh = make_logic_snapshot(2048);
    doc.set_samplerate(1000);

    doc.share_from_logic(old);
    QVERIFY(doc.get_active_logic() == old.get());
    // Ref counts: 1 (test) + 1 (document _logic).
    QVERIFY(old.use_count() == 2);

    doc.share_from_logic(fresh);
    QVERIFY(doc.get_active_logic() == fresh.get());
    // The OLD snapshot is moved into _pending_logic (deferred release), so it
    // stays alive (count still 2: test + pending) and its data remains valid.
    QVERIFY(old.use_count() == 2);
    QVERIFY(old->get_sample_count() == 1024); // still readable before release

    doc.clear_pending_release();
    // After release, only the test holds `old`; `fresh` is held by test + doc.
    QVERIFY(old.use_count() == 1);
    QVERIFY(fresh.use_count() == 2);
    QVERIFY(doc.get_active_logic() == fresh.get());
}

// ---------------------------------------------------------------------------
// DataSource empty-implementation surface
// ---------------------------------------------------------------------------

void TestSessionDocument::EmptyStubCollections() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    QVERIFY(doc.get_signal_models().empty());
    QVERIFY(doc.get_spectrum_stacks().empty());
    QVERIFY(doc.get_math_stack() == nullptr);
    QVERIFY(doc.get_lissajous_model() == nullptr);
    // get_snapshot returns null for every type when no data is shared.
    QVERIFY(doc.get_snapshot(SR_CHANNEL_LOGIC) == nullptr);
    QVERIFY(doc.get_snapshot(SR_CHANNEL_ANALOG) == nullptr);
    QVERIFY(doc.get_snapshot(SR_CHANNEL_DSO) == nullptr);
    QVERIFY(doc.get_snapshot(999) == nullptr);
}

void TestSessionDocument::CurSnapValuesMatchSetters() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    doc.set_samplerate(2500000);
    doc.set_samplelimits(10000);
    QCOMPARE(doc.cur_snap_samplerate(), uint64_t(2500000));
    QCOMPARE(doc.cur_samplelimits(), uint64_t(10000));
    QCOMPARE(doc.cur_sampletime(), doc.get_sampletime());
    QCOMPARE(doc.cur_snap_sampletime(), doc.get_sampletime());
    QCOMPARE(doc.cur_sampletime(), 10000.0 / 2500000.0);
    QCOMPARE(doc.cur_view_time(), doc.get_sampletime());
}

void TestSessionDocument::RunningStatusDefaultsFalse() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    // SessionDocument does not override is_running_status → DataSource default.
    QVERIFY(!doc.is_running_status());
    QVERIFY(!doc.is_working());
    QVERIFY(!doc.is_repeating());
    QVERIFY(!doc.is_instant());
    QCOMPARE(doc.get_map_zoom(), 0);
}

// ---------------------------------------------------------------------------
// decoder_stack container
// ---------------------------------------------------------------------------

void TestSessionDocument::DecoderStackContainerAddRemove() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    auto &stacks = doc.get_decoder_stacks();
    QVERIFY(stacks.empty());

    // add_decoder_stack ignores null.
    doc.add_decoder_stack(nullptr);
    QVERIFY(stacks.empty());

    if (!g_dec)
        QSKIP("libsigrokdecode 'uart' decoder unavailable");

    StubHost host;
    auto s1 = std::make_shared<DecoderStack>(&host, g_dec, new DecoderStatus());
    auto s2 = std::make_shared<DecoderStack>(&host, g_dec, new DecoderStatus());

    doc.add_decoder_stack(s1);
    QCOMPARE(stacks.size(), std::size_t(1));
    doc.add_decoder_stack(s2);
    QCOMPARE(stacks.size(), std::size_t(2));
    // Plain push_back — no de-duplication.
    doc.add_decoder_stack(s1);
    QCOMPARE(stacks.size(), std::size_t(3));

    doc.remove_decoder_stack(s1);
    QCOMPARE(stacks.size(), std::size_t(2)); // [s2, s1] — first occurrence removed
    // s1 was added twice above, so the second removal matches the remaining
    // duplicate (remove_decoder_stack drops one occurrence per call) → [s2].
    doc.remove_decoder_stack(s1);
    QCOMPARE(stacks.size(), std::size_t(1));
    // Removing a stack that is no longer present is a no-op:
    doc.remove_decoder_stack(s2);
    doc.remove_decoder_stack(s2);
    QVERIFY(stacks.empty());
}

// ---------------------------------------------------------------------------
// trigger config + JSON round trip
// ---------------------------------------------------------------------------

void TestSessionDocument::TriggerConfigRoundTrip() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    TriggerConfig cfg;
    cfg.set_mode(TriggerConfig::Adv);
    cfg.set_adv_enabled(true);
    cfg.set_stage_count(2);
    cfg.set_trigger_pos(77);
    std::vector<TriggerConfig::Stage> stages = {
        {QStringLiteral("1 0"), QStringLiteral("0 1"), 0, 0, 0, 5, 0},
        {QStringLiteral("X 1"), QStringLiteral("1 X"), 1, 0, 0, 0, 0}
    };
    cfg.set_stages(stages);
    doc.set_trigger_config(cfg);
    QCOMPARE(doc.trigger_config().mode(), TriggerConfig::Adv);
    QVERIFY(doc.trigger_config().adv_enabled());
    QCOMPARE(doc.trigger_config().trigger_pos(), 77);
    QCOMPARE(doc.trigger_config().stages().size(), std::size_t(2));
}

void TestSessionDocument::SignalConfigJsonRoundTrip() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);

    // Seed the SignalConfigStore directly (the mock port reports no device).
    auto &cfg = doc.signal_config_store()->get_signal_config();
    cfg.work_mode = 1;
    cfg.operation_mode = QStringLiteral("Buffer Mode");
    cfg.is_demo = false;
    ChannelConfig ch;
    ch.index = 3;
    ch.enabled = true;
    ch.name = "D0";
    ch.colour = "#FF0000";
    cfg.channels.push_back(ch);

    // Seed trigger config so the merged round trip covers both halves.
    TriggerConfig tc;
    tc.set_mode(TriggerConfig::Serial);
    tc.set_trigger_pos(321);
    doc.set_trigger_config(tc);

    QJsonObject json = doc.signal_config_to_json();
    QCOMPARE(json["work_mode"].toInt(), 1);
    QCOMPARE(json["operation_mode"].toString(), QStringLiteral("Buffer Mode"));
    QCOMPARE(json["channels"].toArray().size(), 1);
    QVERIFY(json.contains("triggerConfig"));
    QCOMPARE(json["triggerConfig"].toObject()["trigger_pos"].toInt(), 321);

    doc.signal_config_from_json(json);
    QCOMPARE(doc.signal_config_store()->get_signal_config().work_mode, 1);
    QCOMPARE(doc.signal_config_store()->get_signal_config().channels.size(),
             std::size_t(1));
    QCOMPARE(doc.signal_config_store()->get_signal_config().channels[0].index, 3);
    QVERIFY(doc.signal_config_store()->has_signal_config());
    QCOMPARE(doc.trigger_config().mode(), TriggerConfig::Serial);
    QCOMPARE(doc.trigger_config().trigger_pos(), 321);
}

// ---------------------------------------------------------------------------
// clear()
// ---------------------------------------------------------------------------

void TestSessionDocument::ClearResetsEverything() {
    MockDeviceConfigPort port;
    SessionDocument doc(&port);
    auto snap = make_logic_snapshot(2048);
    doc.set_samplerate(1000);
    doc.set_samplelimits(1000);
    doc.set_trigger_pos(55);
    doc.share_from_logic(snap);
    QVERIFY(doc.has_data());
    QVERIFY(doc.get_active_logic() == snap.get());

    doc.clear();
    QVERIFY(doc.get_active_logic() == nullptr);
    QVERIFY(doc.get_active_analog() == nullptr);
    QVERIFY(doc.get_active_dso() == nullptr);
    QVERIFY(!doc.has_data());
    QVERIFY(doc.empty());
    QCOMPARE(doc.get_samplerate(), uint64_t(0));
    QCOMPARE(doc.get_samplelimits(), uint64_t(0));
    QCOMPARE(doc.get_trigger_pos(), uint64_t(0));
}

// ---------------------------------------------------------------------------
// SessionSnapshot (basic)
// ---------------------------------------------------------------------------

void TestSessionDocument::SessionSnapshotBasics() {
    SessionSnapshot snap;
    QVERIFY(snap.get_logic() == nullptr);
    QVERIFY(snap.get_analog() == nullptr);
    QVERIFY(snap.get_dso() == nullptr);
    QVERIFY(snap.get_signal_models().empty());
    QVERIFY(snap.get_spectrum_stacks().empty());
    QVERIFY(snap.get_math_stack() == nullptr);
    QVERIFY(snap.get_lissajous_model() == nullptr);
    // _samplerate == 0 → cur_snap_samplerate() returns 1 (implementation quirk)
    QCOMPARE(snap.cur_snap_samplerate(), uint64_t(1));
    QCOMPARE(snap.cur_samplelimits(), uint64_t(0));
    QCOMPARE(snap.cur_sampletime(), 0.0);
    QCOMPARE(snap.get_trigger_pos(), uint64_t(0));
    // A frozen snapshot reports view data and is not running.
    QVERIFY(snap.have_view_data());
    QVERIFY(!snap.is_running_status());
    // timestamp / file_path round trip
    QDateTime ts = QDateTime::currentDateTimeUtc();
    snap.set_timestamp(ts);
    QCOMPARE(snap.timestamp(), ts);
    snap.set_file_path(QStringLiteral("/tmp/example.px"));
    QCOMPARE(snap.file_path(), QStringLiteral("/tmp/example.px"));
}

void TestSessionDocument::SessionSnapshotSetSamplerateCreatesSnapshots() {
    SessionSnapshot snap;
    snap.set_samplerate(2000000);
    QVERIFY(snap.get_logic() != nullptr);
    QVERIFY(snap.get_analog() != nullptr);
    QVERIFY(snap.get_dso() != nullptr);
    QVERIFY(snap.get_logic()->samplerate() == 2000000.0);
    QCOMPARE(snap.cur_snap_samplerate(), uint64_t(2000000));
    // get_snapshot type routing
    QVERIFY(snap.get_snapshot(SR_CHANNEL_LOGIC) == snap.get_logic());
    QVERIFY(snap.get_snapshot(SR_CHANNEL_ANALOG) == snap.get_analog());
    QVERIFY(snap.get_snapshot(SR_CHANNEL_DSO) == snap.get_dso());
    QVERIFY(snap.get_snapshot(999) == nullptr);
    // limits / trigger / sampletime
    snap.set_samplelimits(4000);
    snap.set_trigger_pos(12);
    QCOMPARE(snap.cur_samplelimits(), uint64_t(4000));
    QCOMPARE(snap.get_trigger_pos(), uint64_t(12));
    QCOMPARE(snap.cur_sampletime(), 4000.0 / 2000000.0);
    // Shared getters keep the snapshots alive and point at the same objects.
    QVERIFY(snap.get_logic_snapshot_shared().get() == snap.get_logic());
}

void TestSessionDocument::SessionSnapshotCopyFromLogic() {
    SessionSnapshot snap;
    auto src = make_logic_snapshot(8192);
    snap.copy_from_logic(src.get());
    QVERIFY(snap.get_logic() != nullptr);
    QVERIFY(!snap.get_logic()->empty());
    QVERIFY(snap.get_logic()->get_sample_count() > 0);
    // Deep copy: different instance, same sample count.
    QVERIFY(snap.get_logic() != src.get());
    QVERIFY(snap.get_logic()->get_sample_count() == src->get_sample_count());

    // Empty source is a no-op (copy_from_* guards on src->empty()).
    LogicSnapshot empty;
    SessionSnapshot snap2;
    snap2.copy_from_logic(&empty);
    QVERIFY(snap2.get_logic() == nullptr);
}

QTEST_GUILESS_MAIN(TestSessionDocument)
#include "test_session_document.moc"
