/*
 * test_cursor_registry.cpp — QTest unit tests for CursorRegistry
 *
 * Migrated from GTest to QTest (P0).
 */

#include <QtTest>
#include "pv/core/cursorregistry.h"

using pv::core::CursorEntry;
using pv::core::CursorRegistry;

class TestCursorRegistry : public QObject {
    Q_OBJECT
private slots:
    void EmptyRegistryHasSizeZero();
    void EmptyRegistryGetCursorsReturnsEmpty();
    void AddCursorReturnsZeroIndexForFirst();
    void AddCursorReturnsIncrementalIndices();
    void AddCursorIncreasesSize();
    void AddedCursorHasCorrectPosition();
    void AddedCursorIsVisibleByDefault();
    void AddCursorWithZeroPosition();
    void GetCursorsReturnsCorrectIndices();
    void GetCursorsReturnsCopyNotReference();
    void RemoveCursorReturnsTrueForValidIndex();
    void RemoveCursorReturnsFalseForInvalidIndex();
    void RemoveCursorReturnsFalseForEmptyRegistry();
    void RemoveCursorDecreasesSize();
    void RemoveCursorShiftsSubsequentEntriesDown();
    void RemoveCursorFromMiddle();
    void RemoveLastCursor();
    void SetCursorPositionReturnsTrueForValidIndex();
    void SetCursorPositionReturnsFalseForInvalidIndex();
    void SetCursorPositionUpdatesPosition();
    void SetCursorPositionDoesNotAffectOtherCursors();
    void SetCursorPositionToZero();
    void ClearEmptiesRegistry();
    void ClearOnEmptyRegistryIsNoOp();
    void AddRemoveAddSequence();
    void RemoveAllThenAdd();
    void LargeNumberOfCursors();
};

void TestCursorRegistry::EmptyRegistryHasSizeZero() {
    CursorRegistry reg;
    QCOMPARE(reg.size(), 0u);
}
void TestCursorRegistry::EmptyRegistryGetCursorsReturnsEmpty() {
    CursorRegistry reg;
    QVERIFY(reg.get_cursors().empty());
}
void TestCursorRegistry::AddCursorReturnsZeroIndexForFirst() {
    CursorRegistry reg;
    QCOMPARE(reg.add_cursor(1000), 0);
}
void TestCursorRegistry::AddCursorReturnsIncrementalIndices() {
    CursorRegistry reg;
    QCOMPARE(reg.add_cursor(100), 0);
    QCOMPARE(reg.add_cursor(200), 1);
    QCOMPARE(reg.add_cursor(300), 2);
}
void TestCursorRegistry::AddCursorIncreasesSize() {
    CursorRegistry reg;
    reg.add_cursor(100); QCOMPARE(reg.size(), 1u);
    reg.add_cursor(200); QCOMPARE(reg.size(), 2u);
    reg.add_cursor(300); QCOMPARE(reg.size(), 3u);
}
void TestCursorRegistry::AddedCursorHasCorrectPosition() {
    CursorRegistry reg;
    reg.add_cursor(5000);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 1u);
    QCOMPARE(c[0].sample_position, 5000u);
}
void TestCursorRegistry::AddedCursorIsVisibleByDefault() {
    CursorRegistry reg;
    reg.add_cursor(100);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 1u);
    QVERIFY(c[0].visible);
}
void TestCursorRegistry::AddCursorWithZeroPosition() {
    CursorRegistry reg;
    reg.add_cursor(0);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 1u);
    QCOMPARE(c[0].sample_position, 0u);
}
void TestCursorRegistry::GetCursorsReturnsCorrectIndices() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200); reg.add_cursor(300);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 3u);
    QCOMPARE(c[0].index, 0);
    QCOMPARE(c[1].index, 1);
    QCOMPARE(c[2].index, 2);
}
void TestCursorRegistry::GetCursorsReturnsCopyNotReference() {
    CursorRegistry reg;
    reg.add_cursor(100);
    auto c1 = reg.get_cursors();
    reg.add_cursor(200);
    auto c2 = reg.get_cursors();
    QCOMPARE(c1.size(), 1u);
    QCOMPARE(c2.size(), 2u);
}
void TestCursorRegistry::RemoveCursorReturnsTrueForValidIndex() {
    CursorRegistry reg;
    reg.add_cursor(100);
    QVERIFY(reg.remove_cursor(0));
}
void TestCursorRegistry::RemoveCursorReturnsFalseForInvalidIndex() {
    CursorRegistry reg;
    reg.add_cursor(100);
    QVERIFY(!reg.remove_cursor(1));
    QVERIFY(!reg.remove_cursor(-1));
}
void TestCursorRegistry::RemoveCursorReturnsFalseForEmptyRegistry() {
    CursorRegistry reg;
    QVERIFY(!reg.remove_cursor(0));
}
void TestCursorRegistry::RemoveCursorDecreasesSize() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200);
    QCOMPARE(reg.size(), 2u);
    reg.remove_cursor(0);
    QCOMPARE(reg.size(), 1u);
}
void TestCursorRegistry::RemoveCursorShiftsSubsequentEntriesDown() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200); reg.add_cursor(300);
    reg.remove_cursor(0);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 2u);
    QCOMPARE(c[0].index, 0);
    QCOMPARE(c[0].sample_position, 200u);
    QCOMPARE(c[1].index, 1);
    QCOMPARE(c[1].sample_position, 300u);
}
void TestCursorRegistry::RemoveCursorFromMiddle() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200); reg.add_cursor(300);
    reg.remove_cursor(1);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 2u);
    QCOMPARE(c[0].sample_position, 100u);
    QCOMPARE(c[1].sample_position, 300u);
}
void TestCursorRegistry::RemoveLastCursor() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200);
    QVERIFY(reg.remove_cursor(1));
    QCOMPARE(reg.size(), 1u);
    QCOMPARE(reg.get_cursors()[0].sample_position, 100u);
}
void TestCursorRegistry::SetCursorPositionReturnsTrueForValidIndex() {
    CursorRegistry reg;
    reg.add_cursor(100);
    QVERIFY(reg.set_cursor_position(0, 999));
}
void TestCursorRegistry::SetCursorPositionReturnsFalseForInvalidIndex() {
    CursorRegistry reg;
    reg.add_cursor(100);
    QVERIFY(!reg.set_cursor_position(1, 999));
    QVERIFY(!reg.set_cursor_position(-1, 999));
}
void TestCursorRegistry::SetCursorPositionUpdatesPosition() {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.set_cursor_position(0, 5000);
    QCOMPARE(reg.get_cursors()[0].sample_position, 5000u);
}
void TestCursorRegistry::SetCursorPositionDoesNotAffectOtherCursors() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200); reg.add_cursor(300);
    reg.set_cursor_position(1, 999);
    auto c = reg.get_cursors();
    QCOMPARE(c[0].sample_position, 100u);
    QCOMPARE(c[1].sample_position, 999u);
    QCOMPARE(c[2].sample_position, 300u);
}
void TestCursorRegistry::SetCursorPositionToZero() {
    CursorRegistry reg;
    reg.add_cursor(500);
    reg.set_cursor_position(0, 0);
    QCOMPARE(reg.get_cursors()[0].sample_position, 0u);
}
void TestCursorRegistry::ClearEmptiesRegistry() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200); reg.add_cursor(300);
    QCOMPARE(reg.size(), 3u);
    reg.clear();
    QCOMPARE(reg.size(), 0u);
    QVERIFY(reg.get_cursors().empty());
}
void TestCursorRegistry::ClearOnEmptyRegistryIsNoOp() {
    CursorRegistry reg;
    reg.clear();
    QCOMPARE(reg.size(), 0u);
}
void TestCursorRegistry::AddRemoveAddSequence() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200);
    reg.remove_cursor(0);
    reg.add_cursor(300);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 2u);
    QCOMPARE(c[0].sample_position, 200u);
    QCOMPARE(c[1].sample_position, 300u);
}
void TestCursorRegistry::RemoveAllThenAdd() {
    CursorRegistry reg;
    reg.add_cursor(100); reg.add_cursor(200);
    reg.remove_cursor(0); reg.remove_cursor(0);
    QCOMPARE(reg.size(), 0u);
    QCOMPARE(reg.add_cursor(300), 0);
    QCOMPARE(reg.size(), 1u);
}
void TestCursorRegistry::LargeNumberOfCursors() {
    CursorRegistry reg;
    for (int i = 0; i < 100; ++i) {
        QCOMPARE(reg.add_cursor(i * 1000), i);
    }
    QCOMPARE(reg.size(), 100u);
    auto c = reg.get_cursors();
    QCOMPARE(c.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        QCOMPARE(c[i].index, i);
        QCOMPARE(c[i].sample_position, static_cast<uint64_t>(i * 1000));
    }
}

QTEST_MAIN(TestCursorRegistry)
#include "test_cursor_registry.moc"
