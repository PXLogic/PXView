/*
 * test_cursor_registry.cpp — Unit tests for CursorRegistry
 *
 * Verifies add/remove/position logic of the Core-layer cursor position store:
 * - add_cursor returns correct positional index
 * - get_cursors returns entries with correct indices
 * - remove_cursor shifts subsequent entries down
 * - set_cursor_position updates the position
 * - clear empties the registry
 * - Invalid index handling
 *
 * CursorRegistry is in pv/core/cursorregistry.h (header) and
 * pv/core/cursorregistry.cpp (implementation). The implementation has no
 * Qt dependencies — only <vector> and <cstdint>.
 */

#include <gtest/gtest.h>

#include "pv/core/cursorregistry.h"

using pv::core::CursorEntry;
using pv::core::CursorRegistry;

// ---- Empty registry ----

TEST(CursorRegistry, EmptyRegistryHasSizeZero) {
    CursorRegistry reg;
    EXPECT_EQ(reg.size(), 0u);
}

TEST(CursorRegistry, EmptyRegistryGetCursorsReturnsEmpty) {
    CursorRegistry reg;
    auto cursors = reg.get_cursors();
    EXPECT_TRUE(cursors.empty());
}

// ---- add_cursor ----

TEST(CursorRegistry, AddCursorReturnsZeroIndexForFirst) {
    CursorRegistry reg;
    int idx = reg.add_cursor(1000);
    EXPECT_EQ(idx, 0);
}

TEST(CursorRegistry, AddCursorReturnsIncrementalIndices) {
    CursorRegistry reg;
    EXPECT_EQ(reg.add_cursor(100), 0);
    EXPECT_EQ(reg.add_cursor(200), 1);
    EXPECT_EQ(reg.add_cursor(300), 2);
}

TEST(CursorRegistry, AddCursorIncreasesSize) {
    CursorRegistry reg;
    reg.add_cursor(100);
    EXPECT_EQ(reg.size(), 1u);
    reg.add_cursor(200);
    EXPECT_EQ(reg.size(), 2u);
    reg.add_cursor(300);
    EXPECT_EQ(reg.size(), 3u);
}

TEST(CursorRegistry, AddedCursorHasCorrectPosition) {
    CursorRegistry reg;
    reg.add_cursor(5000);
    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 1u);
    EXPECT_EQ(cursors[0].sample_position, 5000u);
}

TEST(CursorRegistry, AddedCursorIsVisibleByDefault) {
    CursorRegistry reg;
    reg.add_cursor(100);
    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 1u);
    EXPECT_TRUE(cursors[0].visible);
}

TEST(CursorRegistry, AddCursorWithZeroPosition) {
    CursorRegistry reg;
    reg.add_cursor(0);
    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 1u);
    EXPECT_EQ(cursors[0].sample_position, 0u);
}

// ---- get_cursors (index field) ----

TEST(CursorRegistry, GetCursorsReturnsCorrectIndices) {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.add_cursor(200);
    reg.add_cursor(300);

    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 3u);
    EXPECT_EQ(cursors[0].index, 0);
    EXPECT_EQ(cursors[1].index, 1);
    EXPECT_EQ(cursors[2].index, 2);
}

TEST(CursorRegistry, GetCursorsReturnsCopyNotReference) {
    CursorRegistry reg;
    reg.add_cursor(100);
    auto cursors1 = reg.get_cursors();
    reg.add_cursor(200);
    auto cursors2 = reg.get_cursors();

    // cursors1 should still have 1 entry (it's a copy)
    EXPECT_EQ(cursors1.size(), 1u);
    EXPECT_EQ(cursors2.size(), 2u);
}

// ---- remove_cursor ----

TEST(CursorRegistry, RemoveCursorReturnsTrueForValidIndex) {
    CursorRegistry reg;
    reg.add_cursor(100);
    EXPECT_TRUE(reg.remove_cursor(0));
}

TEST(CursorRegistry, RemoveCursorReturnsFalseForInvalidIndex) {
    CursorRegistry reg;
    reg.add_cursor(100);
    EXPECT_FALSE(reg.remove_cursor(1));
    EXPECT_FALSE(reg.remove_cursor(-1));
}

TEST(CursorRegistry, RemoveCursorReturnsFalseForEmptyRegistry) {
    CursorRegistry reg;
    EXPECT_FALSE(reg.remove_cursor(0));
}

TEST(CursorRegistry, RemoveCursorDecreasesSize) {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.add_cursor(200);
    EXPECT_EQ(reg.size(), 2u);
    reg.remove_cursor(0);
    EXPECT_EQ(reg.size(), 1u);
}

TEST(CursorRegistry, RemoveCursorShiftsSubsequentEntriesDown) {
    CursorRegistry reg;
    reg.add_cursor(100);  // index 0
    reg.add_cursor(200);  // index 1
    reg.add_cursor(300);  // index 2

    reg.remove_cursor(0);  // remove first

    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 2u);
    // Entry that was at index 1 is now at index 0
    EXPECT_EQ(cursors[0].index, 0);
    EXPECT_EQ(cursors[0].sample_position, 200u);
    // Entry that was at index 2 is now at index 1
    EXPECT_EQ(cursors[1].index, 1);
    EXPECT_EQ(cursors[1].sample_position, 300u);
}

TEST(CursorRegistry, RemoveCursorFromMiddle) {
    CursorRegistry reg;
    reg.add_cursor(100);  // index 0
    reg.add_cursor(200);  // index 1
    reg.add_cursor(300);  // index 2

    reg.remove_cursor(1);  // remove middle

    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 2u);
    EXPECT_EQ(cursors[0].sample_position, 100u);
    EXPECT_EQ(cursors[1].sample_position, 300u);
}

TEST(CursorRegistry, RemoveLastCursor) {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.add_cursor(200);

    EXPECT_TRUE(reg.remove_cursor(1));
    EXPECT_EQ(reg.size(), 1u);
    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 1u);
    EXPECT_EQ(cursors[0].sample_position, 100u);
}

// ---- set_cursor_position ----

TEST(CursorRegistry, SetCursorPositionReturnsTrueForValidIndex) {
    CursorRegistry reg;
    reg.add_cursor(100);
    EXPECT_TRUE(reg.set_cursor_position(0, 999));
}

TEST(CursorRegistry, SetCursorPositionReturnsFalseForInvalidIndex) {
    CursorRegistry reg;
    reg.add_cursor(100);
    EXPECT_FALSE(reg.set_cursor_position(1, 999));
    EXPECT_FALSE(reg.set_cursor_position(-1, 999));
}

TEST(CursorRegistry, SetCursorPositionUpdatesPosition) {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.set_cursor_position(0, 5000);
    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 1u);
    EXPECT_EQ(cursors[0].sample_position, 5000u);
}

TEST(CursorRegistry, SetCursorPositionDoesNotAffectOtherCursors) {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.add_cursor(200);
    reg.add_cursor(300);

    reg.set_cursor_position(1, 999);

    auto cursors = reg.get_cursors();
    EXPECT_EQ(cursors[0].sample_position, 100u);
    EXPECT_EQ(cursors[1].sample_position, 999u);
    EXPECT_EQ(cursors[2].sample_position, 300u);
}

TEST(CursorRegistry, SetCursorPositionToZero) {
    CursorRegistry reg;
    reg.add_cursor(500);
    reg.set_cursor_position(0, 0);
    auto cursors = reg.get_cursors();
    EXPECT_EQ(cursors[0].sample_position, 0u);
}

// ---- clear ----

TEST(CursorRegistry, ClearEmptiesRegistry) {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.add_cursor(200);
    reg.add_cursor(300);
    EXPECT_EQ(reg.size(), 3u);

    reg.clear();

    EXPECT_EQ(reg.size(), 0u);
    EXPECT_TRUE(reg.get_cursors().empty());
}

TEST(CursorRegistry, ClearOnEmptyRegistryIsNoOp) {
    CursorRegistry reg;
    reg.clear();
    EXPECT_EQ(reg.size(), 0u);
}

// ---- Complex sequences ----

TEST(CursorRegistry, AddRemoveAddSequence) {
    CursorRegistry reg;
    reg.add_cursor(100);  // index 0
    reg.add_cursor(200);  // index 1
    reg.remove_cursor(0); // remove index 0, 200 shifts to 0
    reg.add_cursor(300);  // index 1

    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 2u);
    EXPECT_EQ(cursors[0].sample_position, 200u);
    EXPECT_EQ(cursors[1].sample_position, 300u);
}

TEST(CursorRegistry, RemoveAllThenAdd) {
    CursorRegistry reg;
    reg.add_cursor(100);
    reg.add_cursor(200);
    reg.remove_cursor(0);
    reg.remove_cursor(0);  // was index 1, now index 0 after shift
    EXPECT_EQ(reg.size(), 0u);

    int idx = reg.add_cursor(300);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(reg.size(), 1u);
}

TEST(CursorRegistry, LargeNumberOfCursors) {
    CursorRegistry reg;
    for (int i = 0; i < 100; ++i) {
        int idx = reg.add_cursor(i * 1000);
        EXPECT_EQ(idx, i);
    }
    EXPECT_EQ(reg.size(), 100u);

    auto cursors = reg.get_cursors();
    ASSERT_EQ(cursors.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(cursors[i].index, i);
        EXPECT_EQ(cursors[i].sample_position, static_cast<uint64_t>(i * 1000));
    }
}
