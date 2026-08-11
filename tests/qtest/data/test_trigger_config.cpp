/*
 * test_trigger_config.cpp — QTest unit tests for TriggerConfig
 *
 * New tests for P0 (data layer coverage).
 */

#include <QtTest>
#include <QJsonObject>
#include <QJsonArray>
#include "pv/data/triggerconfig.h"

using pv::data::TriggerConfig;

class TestTriggerConfig : public QObject {
    Q_OBJECT
private slots:
    void DefaultModeIsSimple();
    void DefaultTriggerPosIsZero();
    void DefaultStageCountIsZero();
    void DefaultStagesIsEmpty();
    void DefaultAdvEnabledIsFalse();
    void DefaultAdvTabIndexIsZero();
    void DefaultSerialDataChannelIsZero();
    void DefaultSerialBitsIsZero();
    void DefaultSerialValueIsEmpty();
    void SetModeToAdv();
    void SetModeToSerial();
    void SetTriggerPos();
    void SetStageCount();
    void SetStages();
    void SetAdvEnabled();
    void SetAdvTabIndex();
    void SetSerialDataChannel();
    void SetSerialBits();
    void SetSerialValue();
    void JsonRoundTripSimpleMode();
    void JsonRoundTripAdvMode();
    void FromJsonStaticFactory();
    void CopyConstructor();
    void StageFieldsDefault();
    void StageFieldsSet();
};

void TestTriggerConfig::DefaultModeIsSimple() {
    TriggerConfig tc;
    QCOMPARE(tc.mode(), TriggerConfig::Simple);
}
void TestTriggerConfig::DefaultTriggerPosIsZero() {
    TriggerConfig tc;
    QCOMPARE(tc.trigger_pos(), 0);
}
void TestTriggerConfig::DefaultStageCountIsZero() {
    TriggerConfig tc;
    QCOMPARE(tc.stage_count(), 0);
}
void TestTriggerConfig::DefaultStagesIsEmpty() {
    TriggerConfig tc;
    QVERIFY(tc.stages().empty());
}
void TestTriggerConfig::DefaultAdvEnabledIsFalse() {
    TriggerConfig tc;
    QVERIFY(!tc.adv_enabled());
}
void TestTriggerConfig::DefaultAdvTabIndexIsZero() {
    TriggerConfig tc;
    QCOMPARE(tc.adv_tab_index(), 0);
}
void TestTriggerConfig::DefaultSerialDataChannelIsZero() {
    TriggerConfig tc;
    QCOMPARE(tc.serial_data_channel(), 0);
}
void TestTriggerConfig::DefaultSerialBitsIsZero() {
    TriggerConfig tc;
    QCOMPARE(tc.serial_bits(), 0);
}
void TestTriggerConfig::DefaultSerialValueIsEmpty() {
    TriggerConfig tc;
    QVERIFY(tc.serial_value().isEmpty());
}
void TestTriggerConfig::SetModeToAdv() {
    TriggerConfig tc;
    tc.set_mode(TriggerConfig::Adv);
    QCOMPARE(tc.mode(), TriggerConfig::Adv);
}
void TestTriggerConfig::SetModeToSerial() {
    TriggerConfig tc;
    tc.set_mode(TriggerConfig::Serial);
    QCOMPARE(tc.mode(), TriggerConfig::Serial);
}
void TestTriggerConfig::SetTriggerPos() {
    TriggerConfig tc;
    tc.set_trigger_pos(42);
    QCOMPARE(tc.trigger_pos(), 42);
}
void TestTriggerConfig::SetStageCount() {
    TriggerConfig tc;
    tc.set_stage_count(3);
    QCOMPARE(tc.stage_count(), 3);
}
void TestTriggerConfig::SetStages() {
    TriggerConfig tc;
    std::vector<TriggerConfig::Stage> stages = {
        {QStringLiteral("1 0 X"), QStringLiteral("0 1 X"), 0, 0, 0, 0, 0},
        {QStringLiteral("X X 1"), QStringLiteral("X X 0"), 1, 1, 0, 5, 0}
    };
    tc.set_stages(stages);
    QCOMPARE(tc.stages().size(), 2u);
    QCOMPARE(tc.stages()[0].value0, QStringLiteral("1 0 X"));
    QCOMPARE(tc.stages()[1].count0, 5);
}
void TestTriggerConfig::SetAdvEnabled() {
    TriggerConfig tc;
    tc.set_adv_enabled(true);
    QVERIFY(tc.adv_enabled());
    tc.set_adv_enabled(false);
    QVERIFY(!tc.adv_enabled());
}
void TestTriggerConfig::SetAdvTabIndex() {
    TriggerConfig tc;
    tc.set_adv_tab_index(1);
    QCOMPARE(tc.adv_tab_index(), 1);
}
void TestTriggerConfig::SetSerialDataChannel() {
    TriggerConfig tc;
    tc.set_serial_data_channel(3);
    QCOMPARE(tc.serial_data_channel(), 3);
}
void TestTriggerConfig::SetSerialBits() {
    TriggerConfig tc;
    tc.set_serial_bits(8);
    QCOMPARE(tc.serial_bits(), 8);
}
void TestTriggerConfig::SetSerialValue() {
    TriggerConfig tc;
    tc.set_serial_value(QStringLiteral("0xAA"));
    QCOMPARE(tc.serial_value(), QStringLiteral("0xAA"));
}
void TestTriggerConfig::JsonRoundTripSimpleMode() {
    TriggerConfig tc;
    tc.set_trigger_pos(100);
    tc.set_mode(TriggerConfig::Simple);
    auto json = tc.to_json();
    auto tc2 = TriggerConfig::from_json(json);
    QCOMPARE(tc2.trigger_pos(), 100);
    QCOMPARE(tc2.mode(), TriggerConfig::Simple);
}
void TestTriggerConfig::JsonRoundTripAdvMode() {
    TriggerConfig tc;
    tc.set_mode(TriggerConfig::Adv);
    tc.set_adv_enabled(true);
    tc.set_stage_count(2);
    std::vector<TriggerConfig::Stage> stages = {
        {QStringLiteral("1 0"), QStringLiteral("0 1"), 0, 0, 0, 0, 0},
        {QStringLiteral("X 1"), QStringLiteral("1 X"), 1, 0, 0, 0, 0}
    };
    tc.set_stages(stages);
    auto json = tc.to_json();
    auto tc2 = TriggerConfig::from_json(json);
    QCOMPARE(tc2.mode(), TriggerConfig::Adv);
    QVERIFY(tc2.adv_enabled());
    QCOMPARE(tc2.stages().size(), 2u);
    QCOMPARE(tc2.stages()[0].value0, QStringLiteral("1 0"));
    QCOMPARE(tc2.stages()[1].value0, QStringLiteral("X 1"));
}
void TestTriggerConfig::FromJsonStaticFactory() {
    QJsonObject obj;
    obj["mode"] = static_cast<int>(TriggerConfig::Serial);
    obj["trigger_pos"] = 50;
    obj["adv_enabled"] = true;
    obj["adv_tab_index"] = 1;
    obj["serial_data_channel"] = 2;
    obj["serial_bits"] = 8;
    obj["serial_value"] = QStringLiteral("0x55");
    auto tc = TriggerConfig::from_json(obj);
    QCOMPARE(tc.trigger_pos(), 50);
    QCOMPARE(tc.serial_bits(), 8);
}
void TestTriggerConfig::CopyConstructor() {
    TriggerConfig tc;
    tc.set_mode(TriggerConfig::Adv);
    tc.set_trigger_pos(77);
    tc.set_stage_count(1);
    std::vector<TriggerConfig::Stage> stages = {{QStringLiteral("1"), QStringLiteral("0"), 0, 0, 0, 0, 0}};
    tc.set_stages(stages);
    TriggerConfig tc2(tc);
    QCOMPARE(tc2.mode(), TriggerConfig::Adv);
    QCOMPARE(tc2.trigger_pos(), 77);
    QCOMPARE(tc2.stages().size(), 1u);
    QCOMPARE(tc2.stages()[0].value0, QStringLiteral("1"));
}
void TestTriggerConfig::StageFieldsDefault() {
    // Stage has no default member initializers for int fields;
    // only QString fields default-construct to empty.
    TriggerConfig::Stage s{};
    QVERIFY(s.value0.isEmpty());
    QVERIFY(s.value1.isEmpty());
}
void TestTriggerConfig::StageFieldsSet() {
    TriggerConfig::Stage s{QStringLiteral("1 0"), QStringLiteral("0 1"), 3, 1, 0, 10, 0};
    QCOMPARE(s.value0, QStringLiteral("1 0"));
    QCOMPARE(s.value1, QStringLiteral("0 1"));
    QCOMPARE(s.logic, 3);
    QCOMPARE(s.inv0, 1);
    QCOMPARE(s.inv1, 0);
    QCOMPARE(s.count0, 10);
    QCOMPARE(s.count1, 0);
}

QTEST_MAIN(TestTriggerConfig)
#include "test_trigger_config.moc"
