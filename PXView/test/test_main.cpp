/*
 * This file is part of the PXView project.
 *
 * Test runner main — parses --run-tests flag and executes all registered tests.
 */

#include "test.h"

int main(int argc, char *argv[])
{
    bool run_tests = false;
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--run-tests") {
            run_tests = true;
            break;
        }
    }

    if (!run_tests)
        return 0;

    std::cout << "=== Running PXView unit tests ===" << std::endl;
    int failures = pv::test::TestRegistry::instance().run_all();
    std::cout << "=== " << (failures == 0 ? "ALL PASSED" : "SOME FAILED")
              << " ===" << std::endl;
    return failures;
}
