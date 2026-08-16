/*
 * test_change_event.cpp — SignalFactory 变化判定纯函数单测。
 *
 * compute_change_event_pure 是 header-only 纯函数（change_event.h），
 * 零 View 层具体类依赖，直接以索引集合 + 指针集合驱动：
 *   - 空→非空：AllReplaced（首次创建）
 *   - 非空→空：AllReplaced（全部移除）
 *   - 空→空：Modified（安全回退）
 *   - 索引相同：Modified
 *   - 纯超集：Added / Removed
 *   - 混合增删：AllReplaced（保守回退）
 */

#include <QtTest/QtTest>

#include <set>

#include "pv/view/signal/change_event.h"

using pv::view::ChangeEventKind;
using pv::view::compute_change_event_pure;

class TestChangeEvent : public QObject
{
    Q_OBJECT

private slots:
    void testEmptyToNonEmptyAllReplaced();
    void testNonEmptyToEmptyAllReplaced();
    void testBothEmptyModified();
    void testSameIndicesModified();
    void testPureSupersetAdded();
    void testPureSubsetRemoved();
    void testMixedAddedRemovedAllReplaced();
};

void TestChangeEvent::testEmptyToNonEmptyAllReplaced()
{
    std::set<int> current;
    std::set<int> models{0, 1, 2};
    QCOMPARE(compute_change_event_pure(current, models, {}),
             ChangeEventKind::AllReplaced);
}

void TestChangeEvent::testNonEmptyToEmptyAllReplaced()
{
    std::set<int> current{0, 1};
    std::set<int> models;
    QCOMPARE(compute_change_event_pure(current, models, {}),
             ChangeEventKind::AllReplaced);
}

void TestChangeEvent::testBothEmptyModified()
{
    std::set<int> current;
    std::set<int> models;
    QCOMPARE(compute_change_event_pure(current, models, {}),
             ChangeEventKind::Modified);
}

void TestChangeEvent::testSameIndicesModified()
{
    std::set<int> current{0, 1, 2};
    std::set<int> models{0, 1, 2};
    QCOMPARE(compute_change_event_pure(current, models, {}),
             ChangeEventKind::Modified);
}

void TestChangeEvent::testPureSupersetAdded()
{
    // current = {0,1}, models = {0,1,2} → Added
    std::set<int> current{0, 1};
    std::set<int> models{0, 1, 2};
    QCOMPARE(compute_change_event_pure(current, models, {}),
             ChangeEventKind::Added);

    // current = {0}, models = {0,1,2} → Added (multiple new)
    std::set<int> current2{0};
    std::set<int> models2{0, 1, 2};
    QCOMPARE(compute_change_event_pure(current2, models2, {}),
             ChangeEventKind::Added);
}

void TestChangeEvent::testPureSubsetRemoved()
{
    // current = {0,1,2}, models = {0,1} → Removed
    std::set<int> current{0, 1, 2};
    std::set<int> models{0, 1};
    QCOMPARE(compute_change_event_pure(current, models, {}),
             ChangeEventKind::Removed);
}

void TestChangeEvent::testMixedAddedRemovedAllReplaced()
{
    // current = {0,1}, models = {1,2} → mixed add+remove → AllReplaced
    std::set<int> current{0, 1};
    std::set<int> models{1, 2};
    QCOMPARE(compute_change_event_pure(current, models, {}),
             ChangeEventKind::AllReplaced);

    // current = {0,1,2}, models = {1,3} → mixed → AllReplaced
    std::set<int> current2{0, 1, 2};
    std::set<int> models2{1, 3};
    QCOMPARE(compute_change_event_pure(current2, models2, {}),
             ChangeEventKind::AllReplaced);
}

QTEST_MAIN(TestChangeEvent)
#include "test_change_event.moc"
