cmake_minimum_required(VERSION 3.16)

# PXView unit test target
# This is a standalone executable that links against the PV core library
# and runs all registered tests when invoked with --run-tests.

project(PXViewTests)

set(TEST_SOURCES
    test_main.cpp
    strnatcmp_test.cpp
)

add_executable(pxview_tests ${TEST_SOURCES})

target_include_directories(pxview_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/PXView
)

# Link against Qt (for QObject, QVariant, QSettings)
find_package(Qt5 COMPONENTS Core REQUIRED)
target_link_libraries(pxview_tests PRIVATE Qt5::Core)

# If the main PV library is built as a static lib, link it here:
# target_link_libraries(pxview_tests PRIVATE pv_core)

enable_testing()
add_test(NAME pxview_tests COMMAND pxview_tests --run-tests)
