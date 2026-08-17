/*
 * test_signal_model.cpp — QTest unit tests for pv::data::SignalModel and
 *                         pv::data::SignalConfigStore.
 *
 * Coverage:
 *   - SignalModel field set/get round-trips (index/name/type/enabled/color/
 *     vdiv/coupling/vfactor/map_default/trig_type/trig_value/vertical_offset/
 *     zero_offset/hw_offset/glitch_filter_enabled/glitch_filter_width/
 *     signal_invert_enabled) using real SR_CHANNEL_* constants.
 *   - Snapshot association (shared_ptr identity + nullptr reset).
 *   - Device write-back path via MockDeviceConfigPort (set_probe_enabled /
 *     set_probe_offset / set_probe_factor / set_trigger_value / commit_trig /
 *     commit_to_device) asserting the exact IDeviceConfigPort calls
 *     (set_config_bool / set_config_uint16 / set_config_uint64 /
 *     set_config_int32) with correct keys and arguments.
 *   - SignalConfigStore save / JSON serialization round-trip / apply /
 *     apply_pending with a fake device port (get_channels stubbed).
 *
 * The fake IDeviceConfigPort lives in this file — the interface header
 * explicitly documents that a ~30-line fake drives data-layer unit tests
 * without any real device connection.
 */

#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>

// ---- Standard library headers (before any macro tricks) ----
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <libsigrok/libsigrok.h>

// ---- xlog stub: signalmodel.cpp / signalconfigstore.cpp include log.h ----
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *, const char *, ...) { return 0; }
int xlog_warn(xlog_writer *, const char *, ...) { return 0; }
int xlog_info(xlog_writer *, const char *, ...) { return 0; }
int xlog_dbg(xlog_writer *, const char *, ...) { return 0; }
int xlog_detail(xlog_writer *, const char *, ...) { return 0; }
}

#include "pv/base/pxvdef.h"
#include "pv/data/idevice_config_port.h"
#include "pv/data/model/signalconfigstore.h"
#include "pv/data/model/signalmodel.h"
#include "pv/data/snapshot/snapshot.h"

using pv::data::ChannelConfig;
using pv::data::ChannelLayoutState;
using pv::data::IDeviceConfigPort;
using pv::data::SignalConfig;
using pv::data::SignalConfigStore;
using pv::data::SignalModel;
using pv::data::Snapshot;

// ---------------------------------------------------------------------------
// Minimal concrete Snapshot subclass (Snapshot is abstract).
// ---------------------------------------------------------------------------
class TestSnapshot : public Snapshot {
public:
    TestSnapshot() : Snapshot(1, 0, 1) {}
    void clear() override {}
    void init() override {}
    bool has_data(int) override { return false; }
    int get_block_num() override { return 0; }
    uint64_t get_block_size(int) override { return 0; }
};

// ---------------------------------------------------------------------------
// MockDeviceConfigPort — fake pv::data::IDeviceConfigPort.
//
// Records every call; returns configurable stub values. get_channels() can
// return nullptr (default) or an injected GSList of sr_channel for tests that
// need the real save/apply iteration path. Channel names must be nullptr or
// string literals (read-only) — apply_signal_config() writes names back via
// g_strdup, so tests that run apply keep the names empty to avoid ownership
// surprises in the mock.
// ---------------------------------------------------------------------------
class MockDeviceConfigPort : public IDeviceConfigPort {
public:
    // ---- Configurable stub values ----
    bool have_instance_val = true;
    int work_mode_val = LOGIC;
    bool is_demo_val = false;
    QString demo_operation_mode_val;
    QString operation_mode_val;
    QString channel_mode_val;

    // ---- Injected device channels (owned by the mock) ----
    std::vector<sr_channel> channels;
    std::vector<GSList> nodes;

    // ---- Recorded calls ----
    int set_work_mode_calls = 0;
    std::vector<int> set_work_mode_values;
    std::vector<std::tuple<int, bool, const sr_channel *>> set_config_bool_calls;
    std::vector<std::tuple<int, int, const sr_channel *>> set_config_int32_calls;
    std::vector<std::tuple<int, int, const sr_channel *>> set_config_uint16_calls;
    std::vector<std::tuple<int, uint64_t, const sr_channel *>> set_config_uint64_calls;
    std::vector<std::tuple<int, std::string, const sr_channel *>> set_config_string_calls;
    std::vector<std::pair<const sr_channel *, bool>> enable_probe_calls;

    void set_channels(std::vector<sr_channel> chs) {
        channels = std::move(chs);
        rebuild_nodes();
    }

    // ---- IDeviceConfigPort ----
    bool have_instance() const override { return have_instance_val; }
    int get_work_mode() override { return work_mode_val; }
    void set_work_mode(int mode) override {
        ++set_work_mode_calls;
        set_work_mode_values.push_back(mode);
    }
    bool is_demo() const override { return is_demo_val; }
    QString get_demo_operation_mode() override { return demo_operation_mode_val; }
    GSList *get_channels() override {
        return nodes.empty() ? nullptr : &nodes[0];
    }
    bool enable_probe(const sr_channel *probe, bool enable) override {
        enable_probe_calls.emplace_back(probe, enable);
        for (sr_channel &c : channels) {
            if (&c == probe) {
                c.enabled = enable ? TRUE : FALSE;
                break;
            }
        }
        return true;
    }
    bool get_config_string(int key, QString &value, const sr_channel *,
                           const sr_channel_group *) override {
        if (key == SR_CONF_OPERATION_MODE)
            value = operation_mode_val;
        else if (key == SR_CONF_CHANNEL_MODE)
            value = channel_mode_val;
        return true;
    }
    bool set_config_string(int key, const char *value, const sr_channel *ch,
                           const sr_channel_group *) override {
        set_config_string_calls.emplace_back(
            key, value ? std::string(value) : std::string(), ch);
        return true;
    }
    bool get_config_bool(int, bool &value, const sr_channel *,
                         const sr_channel_group *) override {
        value = true;
        return true;
    }
    bool set_config_bool(int key, bool value, const sr_channel *ch,
                         const sr_channel_group *) override {
        set_config_bool_calls.emplace_back(key, value, ch);
        return true;
    }
    bool set_config_uint16(int key, int value, const sr_channel *ch,
                           const sr_channel_group *) override {
        set_config_uint16_calls.emplace_back(key, value, ch);
        return true;
    }
    bool set_config_int32(int key, int value, const sr_channel *ch,
                          const sr_channel_group *) override {
        set_config_int32_calls.emplace_back(key, value, ch);
        return true;
    }
    bool set_config_uint64(int key, uint64_t value, const sr_channel *ch,
                           const sr_channel_group *) override {
        set_config_uint64_calls.emplace_back(key, value, ch);
        return true;
    }

private:
    void rebuild_nodes() {
        nodes.resize(channels.size());
        for (size_t i = 0; i < channels.size(); ++i) {
            nodes[i].data = &channels[i];
            nodes[i].next = (i + 1 < channels.size()) ? &nodes[i + 1] : nullptr;
        }
    }
};

// ---------------------------------------------------------------------------
// Test class
// ---------------------------------------------------------------------------
class TestSignalModel : public QObject {
    Q_OBJECT
private slots:
    // ---- SignalModel ----
    void Defaults();
    void SetGetRoundTrip();
    void TypeUsesSrChannelConstants();
    void SnapshotAssociation();
    void SignalsEmitted();
    void SetProbeEnabledWritesDevice();
    void SetProbeOffsetWritesDevice();
    void SetProbeFactorWritesDevice();
    void SetTriggerValueWritesDevice();
    void CommitTrig();
    void SettersTargetSrChannelDevice();
    void CommitToDeviceSyncsAll();
    void HeadlessNoDeviceNoCrash();
    void NoDeviceInstanceSkipsWrites();
    // ---- SignalConfigStore ----
    void SaveWithModelsSetsValid();
    void JsonHasChannelKeyFields();
    void JsonRoundTripPreservesChannels();
    void ApplySignalConfigWritesDevice();
    void ApplyPendingConfig();
    void ApplyWithNoInstanceIsNoOp();
};

// ===========================================================================
// SignalModel tests
// ===========================================================================
void TestSignalModel::Defaults() {
    SignalModel m;
    QCOMPARE(m.index(), 0);
    QCOMPARE(QString::fromStdString(m.name()), QString());
    QCOMPARE(m.type(), (int)SR_CHANNEL_LOGIC);
    QVERIFY(!m.enabled());
    QCOMPARE(QString::fromStdString(m.color()), QString());
    QCOMPARE(m.vdiv(), 0.0);
    QCOMPARE(m.coupling(), 0);
    QCOMPARE(m.vfactor(), 1.0);
    QVERIFY(m.map_default());
    QCOMPARE(m.trig_type(), (int)SignalModel::NONTRIG);
    QCOMPARE(m.trig_value(), 0.0);
    QCOMPARE(m.vertical_offset(), 0.0);
    QCOMPARE(m.zero_offset(), 0.0);
    QCOMPARE(m.hw_offset(), 0.0);
    QVERIFY(!m.glitch_filter_enabled());
    QCOMPARE(m.glitch_filter_width(), 0);
    QVERIFY(!m.signal_invert_enabled());
    QVERIFY(!m.snapshot());
    QVERIFY(m.sr_channel_handle() == nullptr);
}

void TestSignalModel::SetGetRoundTrip() {
    SignalModel m;
    m.set_index(3);
    m.set_name("CH3");
    m.set_type(SR_CHANNEL_ANALOG);
    m.set_enabled(true);
    m.set_color("#112233");
    m.set_vdiv(0.5);
    m.set_coupling(2);
    m.set_vfactor(10.0);
    m.set_map_default(false);
    m.set_trig_type(SignalModel::POSTRIG);
    m.set_trig_value(1.5);
    m.set_vertical_offset(4.0);
    m.set_zero_offset(5.0);
    m.set_hw_offset(6.0);
    m.set_glitch_filter_enabled(true);
    m.set_glitch_filter_width(7);
    m.set_signal_invert_enabled(true);

    QCOMPARE(m.index(), 3);
    QCOMPARE(QString::fromStdString(m.name()), QStringLiteral("CH3"));
    QCOMPARE(m.type(), (int)SR_CHANNEL_ANALOG);
    QVERIFY(m.enabled());
    QCOMPARE(QString::fromStdString(m.color()), QStringLiteral("#112233"));
    QCOMPARE(m.vdiv(), 0.5);
    QCOMPARE(m.coupling(), 2);
    QCOMPARE(m.vfactor(), 10.0);
    QVERIFY(!m.map_default());
    QCOMPARE(m.trig_type(), (int)SignalModel::POSTRIG);
    QCOMPARE(m.trig_value(), 1.5);
    QCOMPARE(m.vertical_offset(), 4.0);
    QCOMPARE(m.zero_offset(), 5.0);
    QCOMPARE(m.hw_offset(), 6.0);
    QVERIFY(m.glitch_filter_enabled());
    QCOMPARE(m.glitch_filter_width(), 7);
    QVERIFY(m.signal_invert_enabled());
}

void TestSignalModel::TypeUsesSrChannelConstants() {
    // Real upstream libsigrok channel type values (libsigrok/include/
    // libsigrok/libsigrok.h, enum sr_channeltype: LOGIC=10000, ANALOG=10001,
    // DSO=10002).
    QCOMPARE((int)SR_CHANNEL_LOGIC, 10000);
    QCOMPARE((int)SR_CHANNEL_ANALOG, 10001);
    QCOMPARE((int)SR_CHANNEL_DSO, 10002);

    SignalModel m;
    m.set_type(SR_CHANNEL_LOGIC);
    QCOMPARE(m.type(), (int)SR_CHANNEL_LOGIC);
    m.set_type(SR_CHANNEL_DSO);
    QCOMPARE(m.type(), (int)SR_CHANNEL_DSO);
    m.set_type(SR_CHANNEL_ANALOG);
    QCOMPARE(m.type(), (int)SR_CHANNEL_ANALOG);
}

void TestSignalModel::SnapshotAssociation() {
    SignalModel m;
    QVERIFY(!m.snapshot());

    auto snap = std::make_shared<TestSnapshot>();
    m.set_snapshot(snap);
    QVERIFY(m.snapshot() == snap);            // shared_ptr identity
    QVERIFY(m.snapshot().get() == snap.get());

    m.set_snapshot(nullptr);
    QVERIFY(!m.snapshot());
}

void TestSignalModel::SignalsEmitted() {
    SignalModel m;
    QSignalSpy spyAppearance(&m, &SignalModel::appearance_changed);
    QSignalSpy spyVisibility(&m, &SignalModel::visibility_changed);
    QSignalSpy spyTrig(&m, &SignalModel::trig_type_changed);

    m.set_color("#00ff00");
    QCOMPARE(spyAppearance.count(), 1);

    m.set_enabled(true);
    QCOMPARE(spyVisibility.count(), 1);

    m.set_trig_type(SignalModel::EDGTRIG);
    QCOMPARE(spyTrig.count(), 1);
    QCOMPARE(spyTrig.takeFirst().at(0).toInt(), (int)SignalModel::EDGTRIG);

    // Unchanged values must not re-emit.
    m.set_trig_type(SignalModel::EDGTRIG);
    QCOMPARE(spyTrig.count(), 0);
    spyVisibility.clear();
    spyAppearance.clear();
    m.set_enabled(true);
    QCOMPARE(spyVisibility.count(), 0);
    m.set_color("#00ff00");
    QCOMPARE(spyAppearance.count(), 0);
}

void TestSignalModel::SetProbeEnabledWritesDevice() {
    MockDeviceConfigPort mock;
    SignalModel m;
    m.set_device_config_port(&mock);
    sr_channel ch{};

    // Default _enabled=false, so the first call fires a device write.
    m.set_probe_enabled(true, &ch);
    QVERIFY(m.enabled());
    QCOMPARE((int)mock.set_config_bool_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_bool_calls[0]), (int)SR_CONF_PROBE_EN);
    QVERIFY(std::get<1>(mock.set_config_bool_calls[0]));
    QVERIFY(std::get<2>(mock.set_config_bool_calls[0]) == &ch);

    // Same value again → guarded by _enabled != enabled, no device write.
    mock.set_config_bool_calls.clear();
    m.set_probe_enabled(true, &ch);
    QCOMPARE((int)mock.set_config_bool_calls.size(), 0);
}

void TestSignalModel::SetProbeOffsetWritesDevice() {
    MockDeviceConfigPort mock;
    SignalModel m;
    m.set_device_config_port(&mock);
    sr_channel ch{};

    m.set_probe_offset(123, &ch);
    QCOMPARE((int)mock.set_config_uint16_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_uint16_calls[0]),
             (int)SR_CONF_PROBE_OFFSET);
    QCOMPARE(std::get<1>(mock.set_config_uint16_calls[0]), 123);
    QVERIFY(std::get<2>(mock.set_config_uint16_calls[0]) == &ch);
}

void TestSignalModel::SetProbeFactorWritesDevice() {
    MockDeviceConfigPort mock;
    SignalModel m;
    m.set_device_config_port(&mock);
    sr_channel ch{};

    m.set_probe_factor(1000ULL, &ch);
    QCOMPARE((int)mock.set_config_uint64_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_uint64_calls[0]),
             (int)SR_CONF_PROBE_FACTOR);
    QCOMPARE(std::get<1>(mock.set_config_uint64_calls[0]), 1000ULL);
    QVERIFY(std::get<2>(mock.set_config_uint64_calls[0]) == &ch);
}

void TestSignalModel::SetTriggerValueWritesDevice() {
    MockDeviceConfigPort mock;
    SignalModel m;
    m.set_device_config_port(&mock);
    sr_channel ch{};

    m.set_trigger_value(2.0, &ch);
    QCOMPARE(m.trig_value(), 2.0);
    QCOMPARE((int)mock.set_config_int32_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_int32_calls[0]),
             (int)SR_CONF_TRIGGER_VALUE);
    QCOMPARE(std::get<1>(mock.set_config_int32_calls[0]), 2);
    QVERIFY(std::get<2>(mock.set_config_int32_calls[0]) == &ch);
}

void TestSignalModel::CommitTrig() {
    SignalModel m;
    m.set_trig_type(SignalModel::NONTRIG);
    QVERIFY(!m.commit_trig());
    m.set_trig_type(SignalModel::POSTRIG);
    QVERIFY(m.commit_trig());
}

void TestSignalModel::SettersTargetSrChannelDevice() {
    MockDeviceConfigPort mock;
    SignalModel m;
    m.set_device_config_port(&mock);
    sr_channel ch{};
    m.set_sr_channel(&ch);

    m.set_coupling(2);
    m.set_vfactor(10.0);
    m.set_map_default(false);
    m.set_zero_offset(5.0);
    m.set_hw_offset(6.0);

    // set_coupling → set_config_int32(SR_CONF_PROBE_COUPLING, 2, &ch)
    QCOMPARE((int)mock.set_config_int32_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_int32_calls[0]),
             (int)SR_CONF_PROBE_COUPLING);
    QCOMPARE(std::get<1>(mock.set_config_int32_calls[0]), 2);
    QVERIFY(std::get<2>(mock.set_config_int32_calls[0]) == &ch);

    // set_vfactor → set_config_uint64(SR_CONF_PROBE_FACTOR, 10, &ch)
    QCOMPARE((int)mock.set_config_uint64_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_uint64_calls[0]),
             (int)SR_CONF_PROBE_FACTOR);
    QCOMPARE(std::get<1>(mock.set_config_uint64_calls[0]), 10ULL);
    QVERIFY(std::get<2>(mock.set_config_uint64_calls[0]) == &ch);

    // set_map_default → set_config_bool(SR_CONF_PROBE_MAP_DEFAULT, false, &ch)
    QCOMPARE((int)mock.set_config_bool_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_bool_calls[0]),
             (int)SR_CONF_PROBE_MAP_DEFAULT);
    QVERIFY(!std::get<1>(mock.set_config_bool_calls[0]));
    QVERIFY(std::get<2>(mock.set_config_bool_calls[0]) == &ch);

    // set_zero_offset → set_config_uint16(SR_CONF_PROBE_OFFSET, 5, &ch)
    // set_hw_offset    → set_config_uint16(SR_CONF_PROBE_HW_OFFSET, 6, &ch)
    QCOMPARE((int)mock.set_config_uint16_calls.size(), 2);
    QCOMPARE(std::get<0>(mock.set_config_uint16_calls[0]),
             (int)SR_CONF_PROBE_OFFSET);
    QCOMPARE(std::get<1>(mock.set_config_uint16_calls[0]), 5);
    QCOMPARE(std::get<0>(mock.set_config_uint16_calls[1]),
             (int)SR_CONF_PROBE_HW_OFFSET);
    QCOMPARE(std::get<1>(mock.set_config_uint16_calls[1]), 6);

    // Model fields updated alongside the device writes.
    QCOMPARE(m.coupling(), 2);
    QCOMPARE(m.vfactor(), 10.0);
    QVERIFY(!m.map_default());
    QCOMPARE(m.zero_offset(), 5.0);
    QCOMPARE(m.hw_offset(), 6.0);
}

void TestSignalModel::CommitToDeviceSyncsAll() {
    MockDeviceConfigPort mock;
    SignalModel m;
    sr_channel ch{};
    m.set_sr_channel(&ch);
    m.set_device_config_port(&mock);

    m.set_name("D0");
    m.set_enabled(true);
    m.set_vfactor(10.0);
    m.set_zero_offset(5.0);
    m.set_hw_offset(6.0);
    m.set_map_default(false);

    // Clear the setter-driven calls; assert only commit_to_device output.
    mock.set_config_bool_calls.clear();
    mock.set_config_uint16_calls.clear();
    mock.set_config_uint64_calls.clear();

    m.commit_to_device();

    // Direct sr_channel struct fields synced by commit_to_device.
    QCOMPARE(ch.enabled, (gboolean)TRUE);
    QVERIFY(ch.name != nullptr);
    QCOMPARE(std::string(ch.name), std::string("D0"));

    // commit_to_device → 5 device writes:
    //   set_config_bool   (PROBE_EN, PROBE_MAP_DEFAULT)
    //   set_config_uint64 (PROBE_FACTOR)
    //   set_config_uint16 (PROBE_OFFSET, PROBE_HW_OFFSET)
    QCOMPARE((int)mock.set_config_bool_calls.size(), 2);
    QCOMPARE((int)mock.set_config_uint64_calls.size(), 1);
    QCOMPARE((int)mock.set_config_uint16_calls.size(), 2);

    QCOMPARE(std::get<0>(mock.set_config_bool_calls[0]),
             (int)SR_CONF_PROBE_EN);
    QVERIFY(std::get<1>(mock.set_config_bool_calls[0]));
    QVERIFY(std::get<2>(mock.set_config_bool_calls[0]) == &ch);
    QCOMPARE(std::get<0>(mock.set_config_bool_calls[1]),
             (int)SR_CONF_PROBE_MAP_DEFAULT);
    QVERIFY(!std::get<1>(mock.set_config_bool_calls[1]));

    QCOMPARE(std::get<0>(mock.set_config_uint64_calls[0]),
             (int)SR_CONF_PROBE_FACTOR);
    QCOMPARE(std::get<1>(mock.set_config_uint64_calls[0]), 10ULL);

    QCOMPARE(std::get<0>(mock.set_config_uint16_calls[0]),
             (int)SR_CONF_PROBE_OFFSET);
    QCOMPARE(std::get<1>(mock.set_config_uint16_calls[0]), 5);
    QCOMPARE(std::get<0>(mock.set_config_uint16_calls[1]),
             (int)SR_CONF_PROBE_HW_OFFSET);
    QCOMPARE(std::get<1>(mock.set_config_uint16_calls[1]), 6);

    g_free(ch.name);   // free the g_strdup'd name from commit_to_device
    ch.name = nullptr;
}

void TestSignalModel::HeadlessNoDeviceNoCrash() {
    // No device port, no sr_channel — every setter must be a safe no-op.
    SignalModel m;
    m.set_probe_enabled(true);
    m.set_probe_offset(1);
    m.set_probe_factor(2);
    m.set_trigger_value(3.0);
    m.commit_trig();
    m.commit_to_device();
    m.set_coupling(1);
    m.set_vfactor(2.0);
    m.set_map_default(false);
    m.set_zero_offset(4.0);
    m.set_hw_offset(5.0);
    m.set_name("x");   // no _sr_channel → no g_strdup
    m.set_snapshot(std::make_shared<TestSnapshot>());

    QVERIFY(m.enabled());
    QCOMPARE(m.trig_value(), 3.0);
    QVERIFY(m.snapshot());
}

void TestSignalModel::NoDeviceInstanceSkipsWrites() {
    MockDeviceConfigPort mock;
    mock.have_instance_val = false;
    SignalModel m;
    m.set_device_config_port(&mock);
    sr_channel ch{};
    m.set_sr_channel(&ch);

    m.set_probe_enabled(true, &ch);
    m.set_probe_offset(1, &ch);
    m.set_probe_factor(2, &ch);
    m.set_trigger_value(3.0, &ch);
    m.commit_to_device();

    QCOMPARE((int)mock.set_config_bool_calls.size(), 0);
    QCOMPARE((int)mock.set_config_uint16_calls.size(), 0);
    QCOMPARE((int)mock.set_config_uint64_calls.size(), 0);
    QCOMPARE((int)mock.set_config_int32_calls.size(), 0);

    // commit_to_device still syncs the sr_channel struct fields directly.
    QCOMPARE(ch.enabled, (gboolean)TRUE);
    g_free(ch.name);
    ch.name = nullptr;
}

// ===========================================================================
// SignalConfigStore tests
// ===========================================================================
void TestSignalModel::SaveWithModelsSetsValid() {
    MockDeviceConfigPort mock;
    std::vector<sr_channel> chs;
    sr_channel c0{};
    c0.index = 0;
    c0.type = SR_CHANNEL_LOGIC;
    c0.enabled = TRUE;
    c0.name = const_cast<char *>("D0");
    chs.push_back(c0);
    sr_channel c1{};
    c1.index = 1;
    c1.type = SR_CHANNEL_LOGIC;
    c1.enabled = FALSE;
    c1.name = const_cast<char *>("D1");
    chs.push_back(c1);
    mock.set_channels(std::move(chs));

    SignalConfigStore store(&mock);
    QVERIFY(!store.has_signal_config());

    auto m0 = std::make_shared<SignalModel>();
    m0->set_index(0);
    m0->set_name("D0");
    m0->set_color("#FF0000");
    m0->set_trig_type(SignalModel::POSTRIG);
    auto m1 = std::make_shared<SignalModel>();
    m1->set_index(1);
    m1->set_name("D1");
    m1->set_color("#00FF00");
    m1->set_trig_type(SignalModel::NEGTRIG);

    std::map<int, ChannelLayoutState> layout;
    layout[0] = ChannelLayoutState();
    layout[0].view_index = 0;
    layout[0].v_offset = 10;
    layout[0].own_height = 50;
    layout[1] = ChannelLayoutState();
    layout[1].view_index = 1;
    layout[1].v_offset = 20;
    layout[1].own_height = 60;

    std::map<int, std::string> colours;
    colours[0] = "#FF0000";
    colours[1] = "#00FF00";

    store.save_signal_config({m0, m1}, layout, colours);
    QVERIFY(store.has_signal_config());

    const SignalConfig &cfg = store.get_signal_config();
    QCOMPARE(cfg.work_mode, mock.work_mode_val);
    QCOMPARE((int)cfg.channels.size(), 2);

    const ChannelConfig &s0 = cfg.channels[0];
    QCOMPARE(s0.index, 0);
    QVERIFY(s0.enabled);
    QCOMPARE(s0.type, (int)SR_CHANNEL_LOGIC);
    QCOMPARE(QString::fromStdString(s0.name), QStringLiteral("D0"));
    QCOMPARE(QString::fromStdString(s0.colour), QStringLiteral("#FF0000"));
    QCOMPARE(s0.trig_type, (int)SignalModel::POSTRIG);
    QCOMPARE(s0.view_index, 0);
    QCOMPARE(s0.v_offset, 10);
    QCOMPARE(s0.own_height, 50);

    const ChannelConfig &s1 = cfg.channels[1];
    QCOMPARE(s1.index, 1);
    QVERIFY(!s1.enabled);
    QCOMPARE(QString::fromStdString(s1.colour), QStringLiteral("#00FF00"));
    QCOMPARE(s1.trig_type, (int)SignalModel::NEGTRIG);
    QCOMPARE(s1.view_index, 1);
    QCOMPARE(s1.v_offset, 20);
    QCOMPARE(s1.own_height, 60);
}

void TestSignalModel::JsonHasChannelKeyFields() {
    MockDeviceConfigPort mock;
    std::vector<sr_channel> chs;
    sr_channel c0{};
    c0.index = 0;
    c0.type = SR_CHANNEL_LOGIC;
    c0.enabled = TRUE;
    c0.name = const_cast<char *>("D0");
    chs.push_back(c0);
    mock.set_channels(std::move(chs));

    SignalConfigStore store(&mock);
    auto m0 = std::make_shared<SignalModel>();
    m0->set_index(0);
    m0->set_name("D0");
    m0->set_color("#FF0000");
    m0->set_trig_type(SignalModel::POSTRIG);

    std::map<int, std::string> colours;
    colours[0] = "#FF0000";

    store.save_signal_config({m0}, {}, colours);
    QVERIFY(store.has_signal_config());

    const QJsonObject json = store.signal_config_to_json();
    QVERIFY(json.contains("work_mode"));
    QVERIFY(json.contains("operation_mode"));
    QVERIFY(json.contains("channel_mode"));
    QVERIFY(json.contains("is_demo"));
    QVERIFY(json.contains("demo_operation_mode"));
    QVERIFY(json.contains("channels"));

    const QJsonArray arr = json["channels"].toArray();
    QCOMPARE(arr.size(), 1);
    const QJsonObject c = arr[0].toObject();
    QCOMPARE(c["index"].toInt(), 0);
    QCOMPARE(c["type"].toInt(), (int)SR_CHANNEL_LOGIC);
    QVERIFY(c["enabled"].toBool());
    QCOMPARE(c["name"].toString(), QStringLiteral("D0"));
    QCOMPARE(c["colour"].toString(), QStringLiteral("#FF0000"));
    QCOMPARE(c["trig_type"].toInt(), (int)SignalModel::POSTRIG);

    // Every serialized key is emitted, even for LOGIC channels where the
    // DSO-only fields keep their struct defaults.
    QVERIFY(c.contains("vdiv"));
    QVERIFY(c.contains("coupling"));
    QVERIFY(c.contains("map_default"));
    QVERIFY(c.contains("hw_offset"));
    QVERIFY(c.contains("offset"));
    QVERIFY(c.contains("zero_offset"));
    QVERIFY(c.contains("view_index"));
    QVERIFY(c.contains("v_offset"));
    QVERIFY(c.contains("own_height"));
    QVERIFY(c.contains("vfactor"));
    QVERIFY(c.contains("trig_value"));
    QVERIFY(c.contains("map_unit"));
    QVERIFY(c.contains("map_min"));
    QVERIFY(c.contains("map_max"));
}

void TestSignalModel::JsonRoundTripPreservesChannels() {
    MockDeviceConfigPort mock;
    SignalConfigStore store(&mock);

    QJsonObject json;
    json["work_mode"] = DSO;
    json["operation_mode"] = QStringLiteral("Buffer Mode");
    json["channel_mode"] = QStringLiteral("2 Channels");
    json["is_demo"] = true;
    json["demo_operation_mode"] = QStringLiteral("Sine");

    QJsonArray arr;
    QJsonObject c0;
    c0["index"] = 0;
    c0["enabled"] = true;
    c0["vdiv"] = (qint64)1000000;
    c0["coupling"] = 1;
    c0["map_default"] = true;
    c0["hw_offset"] = 3;
    c0["offset"] = 4;
    c0["zero_offset"] = 5;
    c0["trig_type"] = 2;
    c0["view_index"] = 1;
    c0["v_offset"] = 8;
    c0["own_height"] = 40;
    c0["type"] = (int)SR_CHANNEL_DSO;
    c0["name"] = QStringLiteral("D0");
    c0["colour"] = QStringLiteral("#ABCDEF");
    c0["vfactor"] = (qint64)10;
    c0["trig_value"] = 7;
    c0["map_unit"] = QStringLiteral("V");
    c0["map_min"] = -1.0;
    c0["map_max"] = 1.0;
    arr.append(c0);
    json["channels"] = arr;

    store.signal_config_from_json(json);
    QVERIFY(store.has_signal_config());

    const SignalConfig &cfg = store.get_signal_config();
    QCOMPARE(cfg.work_mode, DSO);
    QCOMPARE(cfg.operation_mode, QStringLiteral("Buffer Mode"));
    QCOMPARE(cfg.channel_mode, QStringLiteral("2 Channels"));
    QVERIFY(cfg.is_demo);
    QCOMPARE(cfg.demo_operation_mode, QStringLiteral("Sine"));

    // get_channels() reflects the same channels as get_signal_config().
    QCOMPARE((int)store.get_channels().size(), 1);
    QCOMPARE((int)cfg.channels.size(), 1);

    const ChannelConfig &ch = cfg.channels[0];
    QCOMPARE(ch.index, 0);
    QVERIFY(ch.enabled);
    QCOMPARE((int)ch.vdiv, 1000000);
    QCOMPARE(ch.coupling, 1);
    QVERIFY(ch.map_default);
    QCOMPARE((int)ch.hw_offset, 3);
    QCOMPARE((int)ch.offset, 4);
    QCOMPARE((int)ch.zero_offset, 5);
    QCOMPARE(ch.trig_type, 2);
    QCOMPARE(ch.view_index, 1);
    QCOMPARE(ch.v_offset, 8);
    QCOMPARE(ch.own_height, 40);
    QCOMPARE(ch.type, (int)SR_CHANNEL_DSO);
    QCOMPARE(QString::fromStdString(ch.name), QStringLiteral("D0"));
    QCOMPARE(QString::fromStdString(ch.colour), QStringLiteral("#ABCDEF"));
    QCOMPARE((int)ch.vfactor, 10);
    QCOMPARE((int)ch.trig_value, 7);
    QCOMPARE(QString::fromStdString(ch.map_unit), QStringLiteral("V"));
    QCOMPARE(ch.map_min, -1.0);
    QCOMPARE(ch.map_max, 1.0);

    // to_json round-trips the same values.
    const QJsonObject json2 = store.signal_config_to_json();
    QCOMPARE(json2["work_mode"].toInt(), DSO);
    QCOMPARE(json2["operation_mode"].toString(), QStringLiteral("Buffer Mode"));
    QCOMPARE(json2["is_demo"].toBool(), true);
    const QJsonArray arr2 = json2["channels"].toArray();
    QCOMPARE(arr2.size(), 1);
    const QJsonObject c2 = arr2[0].toObject();
    QCOMPARE(c2["index"].toInt(), 0);
    QCOMPARE(c2["name"].toString(), QStringLiteral("D0"));
    QCOMPARE(c2["colour"].toString(), QStringLiteral("#ABCDEF"));
    QCOMPARE(c2["type"].toInt(), (int)SR_CHANNEL_DSO);
    QCOMPARE(c2["vfactor"].toVariant().toULongLong(), (quint64)10);
    QCOMPARE(c2["trig_value"].toInt(), 7);
    QCOMPARE(c2["map_min"].toDouble(), -1.0);
    QCOMPARE(c2["map_max"].toDouble(), 1.0);
}

void TestSignalModel::ApplySignalConfigWritesDevice() {
    MockDeviceConfigPort mock;
    mock.operation_mode_val = QStringLiteral("Buffer Mode");
    std::vector<sr_channel> chs;
    sr_channel c0{};
    c0.index = 0;
    c0.type = SR_CHANNEL_LOGIC;
    c0.enabled = TRUE;
    c0.name = nullptr;   // keep names empty so apply() never g_strdups into them
    chs.push_back(c0);
    sr_channel c1{};
    c1.index = 1;
    c1.type = SR_CHANNEL_LOGIC;
    c1.enabled = FALSE;
    c1.name = nullptr;
    chs.push_back(c1);
    mock.set_channels(std::move(chs));

    SignalConfigStore store(&mock);
    auto m0 = std::make_shared<SignalModel>();
    m0->set_index(0);
    auto m1 = std::make_shared<SignalModel>();
    m1->set_index(1);
    store.save_signal_config({m0, m1});
    QVERIFY(store.has_signal_config());
    QCOMPARE(store.get_signal_config().operation_mode, QStringLiteral("Buffer Mode"));

    // Device now reports a different mode → apply must switch it back.
    mock.work_mode_val = DSO;
    store.apply_signal_config();

    QCOMPARE(mock.set_work_mode_calls, 1);
    QCOMPARE(mock.set_work_mode_values[0], LOGIC);

    // Non-empty operation_mode → written back via set_config_string.
    QCOMPARE((int)mock.set_config_string_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_string_calls[0]),
             (int)SR_CONF_OPERATION_MODE);
    QCOMPARE(std::get<1>(mock.set_config_string_calls[0]),
             std::string("Buffer Mode"));

    // Per-channel enable/disable mirrored to the device.
    QCOMPARE((int)mock.enable_probe_calls.size(), 2);
    QVERIFY(mock.enable_probe_calls[0].first == &mock.channels[0]);
    QVERIFY(mock.enable_probe_calls[0].second);
    QVERIFY(mock.enable_probe_calls[1].first == &mock.channels[1]);
    QVERIFY(!mock.enable_probe_calls[1].second);
}

void TestSignalModel::ApplyPendingConfig() {
    MockDeviceConfigPort mock;
    SignalConfigStore store(&mock);
    QVERIFY(!store.has_pending_config());

    SignalConfig cfg;
    cfg.work_mode = DSO;
    cfg.operation_mode = QStringLiteral("Buffer Mode");
    cfg.is_valid = true;
    store.set_pending_config(cfg);
    QVERIFY(store.has_pending_config());

    mock.work_mode_val = LOGIC;   // current device mode differs from pending
    store.apply_pending_config();

    QVERIFY(!store.has_pending_config());
    QVERIFY(store.has_signal_config());
    QCOMPARE(store.get_signal_config().work_mode, DSO);
    QCOMPARE(mock.set_work_mode_calls, 1);
    QCOMPARE(mock.set_work_mode_values[0], DSO);
    QCOMPARE((int)mock.set_config_string_calls.size(), 1);
    QCOMPARE(std::get<0>(mock.set_config_string_calls[0]),
             (int)SR_CONF_OPERATION_MODE);
    // No device channels injected → no enable_probe calls.
    QCOMPARE((int)mock.enable_probe_calls.size(), 0);
}

void TestSignalModel::ApplyWithNoInstanceIsNoOp() {
    MockDeviceConfigPort mock;
    mock.have_instance_val = false;
    SignalConfigStore store(&mock);
    auto m0 = std::make_shared<SignalModel>();
    m0->set_index(0);

    // save guard: have_instance() false → early return, config stays invalid.
    store.save_signal_config({m0});
    QVERIFY(!store.has_signal_config());

    store.apply_signal_config();   // guard: no valid config → no-op
    store.apply_pending_config();  // guard: no pending config → no-op

    QCOMPARE(mock.set_work_mode_calls, 0);
    QCOMPARE((int)mock.enable_probe_calls.size(), 0);
    QCOMPARE((int)mock.set_config_string_calls.size(), 0);
}

QTEST_MAIN(TestSignalModel)
#include "test_signal_model.moc"
