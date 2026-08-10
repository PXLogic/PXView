/*
 * This file is part of the PXView project.
 * PXView is based on PulseView.
 *
 * Unit test framework — lightweight assertion-based tests, no external
 * dependency (no gtest/boost.test required). Run with:
 *   PXView --run-tests
 *
 * Ported from PulseView's test/ framework.
 */

#ifndef PXVIEW_TEST_TEST_H
#define PXVIEW_TEST_TEST_H

#include <functional>
#include <string>
#include <vector>
#include <iostream>
#include <cstdint>

namespace pv {
namespace test {

/// A single test case.
struct TestCase {
    std::string name;
    std::function<void()> fn;
};

/// Test registry — singleton that collects all test cases.
class TestRegistry {
public:
    static TestRegistry &instance() {
        static TestRegistry r;
        return r;
    }

    void add(const std::string &name, std::function<void()> fn) {
        cases_.push_back({name, std::move(fn)});
    }

    int run_all() {
        int passed = 0;
        int failed = 0;

        for (const auto &tc : cases_) {
            std::cout << "  [ RUN      ] " << tc.name << std::endl;
            try {
                tc.fn();
                std::cout << "  [       OK ] " << tc.name << std::endl;
                passed++;
            } catch (const std::exception &e) {
                std::cout << "  [  FAILED  ] " << tc.name
                          << ": " << e.what() << std::endl;
                failed++;
            } catch (...) {
                std::cout << "  [  FAILED  ] " << tc.name
                          << ": unknown exception" << std::endl;
                failed++;
            }
        }

        std::cout << "\n  Passed: " << passed
                  << ", Failed: " << failed << std::endl;
        return failed;
    }

private:
    std::vector<TestCase> cases_;
};

/// Auto-registration helper.
struct TestRegistrar {
    TestRegistrar(const char *name, std::function<void()> fn) {
        TestRegistry::instance().add(name, std::move(fn));
    }
};

// Assertion macros
#define PXV_TEST_ASSERT(cond) \
    do { if (!(cond)) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        ": assertion failed: " #cond); } while(0)

#define PXV_TEST_ASSERT_EQ(a, b) \
    do { if (!((a) == (b))) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        ": assertion failed: " #a " == " #b); } while(0)

#define PXV_TEST_ASSERT_NE(a, b) \
    do { if (((a) == (b))) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        ": assertion failed: " #a " != " #b); } while(0)

#define PXV_TEST(name) \
    static void test_##name(); \
    static pv::test::TestRegistrar \
        reg_##name(#name, test_##name); \
    static void test_##name()

} // namespace test
} // namespace pv

#endif // PXVIEW_TEST_TEST_H
