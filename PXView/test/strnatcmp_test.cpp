/*
 * This file is part of the PXView project.
 *
 * Unit tests for natural string comparison (strnatcmp).
 */

#include "test.h"
#include "pv/base/strnatcmp.h"

using namespace pv::base;

// --- strnatcmp tests ---

PXV_TEST(strnatcmp_basic_ordering) {
    // Natural: CH1 < CH2 < CH10
    PXV_TEST_ASSERT(strnatcmp("CH1", "CH2") < 0);
    PXV_TEST_ASSERT(strnatcmp("CH2", "CH10") < 0);
    PXV_TEST_ASSERT(strnatcmp("CH1", "CH10") < 0);
}

PXV_TEST(strnatcmp_lexicographic_differs) {
    // strcmp would give: CH10 < CH2 (because '1' < '2')
    // strnatcmp gives: CH2 < CH10 (because 2 < 10 numerically)
    PXV_TEST_ASSERT(strnatcmp("CH2", "CH10") < 0);
}

PXV_TEST(strnatcmp_equal) {
    PXV_TEST_ASSERT(strnatcmp("CH1", "CH1") == 0);
    PXV_TEST_ASSERT(strnatcmp("hello", "hello") == 0);
}

PXV_TEST(strnatcmp_string_overload) {
    std::string a = "CH2";
    std::string b = "CH10";
    PXV_TEST_ASSERT(strnatcmp(a, b) < 0);
}

PXV_TEST(strnatcasecmp_case_insensitive) {
    PXV_TEST_ASSERT(strnatcasecmp("ch2", "CH2") == 0);
    PXV_TEST_ASSERT(strnatcasecmp("CH2", "ch10") < 0);
}

PXV_TEST(natural_compare_functor) {
    std::vector<std::string> names = {"CH10", "CH2", "CH1", "CH3"};
    std::sort(names.begin(), names.end(), NaturalCompare());
    PXV_TEST_ASSERT_EQ(names[0], std::string("CH1"));
    PXV_TEST_ASSERT_EQ(names[1], std::string("CH2"));
    PXV_TEST_ASSERT_EQ(names[2], std::string("CH3"));
    PXV_TEST_ASSERT_EQ(names[3], std::string("CH10"));
}
