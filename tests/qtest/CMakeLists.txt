#===============================================================================
#= PXView QTest Unit Tests
#-------------------------------------------------------------------------------
# Qt6 Test framework + CTest integration.
# Each test file compiles as an independent executable, auto-registered with CTest.
#
# This directory is only added when -DENABLE_TESTS=ON is passed to CMake.
# Tests are compiled as part of the default build (no EXCLUDE_FROM_ALL),
# so `ninja` builds them and `ninja test` / `ctest` runs them.
#
#   # Configure with tests:
#   cmake -DENABLE_TESTS=ON -B build -G Ninja
#
#   # Build (tests are included in default target):
#   cmake --build build
#
#   # Run all tests:
#   cd build && ctest --output-on-failure
#
#   # Run a single test:
#   cd build && ctest -R test_cursor_registry --output-on-failure
#
# Directory structure:
#   tests/qtest/
#   ├── CMakeLists.txt           (this file)
#   ├── core/
#   │   └── test_cursor_registry.cpp    (29 tests)
#   ├── data/
#   │   └── test_trigger_config.cpp     (27 tests)
#   ├── api/
#   │   ├── test_result_types.cpp       (15 tests)
#   │   └── test_binary_codec.cpp       (26 tests)
#   └── view/
#       ├── test_view_layout.cpp        (30 tests)
#       └── test_trace_visitor.cpp      (18 tests)
#===============================================================================

find_package(Qt6 REQUIRED COMPONENTS Test Core Gui Widgets)

set(PXVIEW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/PXView")

# ---- Helper: add a QTest executable with optional extra source files ----
# Usage:
#   pv_add_qtest(target_name source_file [extra_source1 ...])
function(pv_add_qtest target_name)
    set(_sources ${ARGN})
    add_executable(${target_name} ${_sources})
    # Enable AUTOMOC so Q_OBJECT classes in .cpp files get .moc generated
    set_target_properties(${target_name} PROPERTIES AUTOMOC ON)
    target_include_directories(${target_name} PRIVATE
        ${PXVIEW_INCLUDE_DIR}
    )
    # Link Qt6::Widgets + Qt6::Gui because the parent project globally defines
    # QT_WIDGETS_LIB / QT_GUI_LIB via add_definitions(), which makes QTEST_MAIN
    # expand to QApplication(argc, argv) — requiring Widgets.
    target_link_libraries(${target_name} PRIVATE
        Qt6::Test
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )
    # C++23 for std::expected (used by api/types.h Result<T>)
    target_compile_features(${target_name} PRIVATE cxx_std_23)
    # Force console subsystem on MinGW (otherwise linker looks for WinMain)
    target_link_options(${target_name} PRIVATE -mconsole)
    # Register with CTest
    add_test(NAME ${target_name} COMMAND ${target_name})
    # Offscreen platform for headless CI
    set_tests_properties(${target_name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endfunction()

# ==== Core layer tests ====
pv_add_qtest(test_cursor_registry
    core/test_cursor_registry.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/core/cursorregistry.cpp
)

# ==== Data layer tests ====
pv_add_qtest(test_trigger_config
    data/test_trigger_config.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/triggerconfig.cpp
)

# ==== API layer tests ====
pv_add_qtest(test_result_types
    api/test_result_types.cpp
)

pv_add_qtest(test_binary_codec
    api/test_binary_codec.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/api/binary_codec.cpp
)

# ==== View layer tests (header-only mocks) ====
pv_add_qtest(test_view_layout
    view/test_view_layout.cpp
)

# SignalFactory change-event classification — pure header-only function
# (change_event.h), zero View-layer concrete dependencies.
pv_add_qtest(test_change_event
    view/test_change_event.cpp
)

pv_add_qtest(test_trace_visitor
    view/test_trace_visitor.cpp
)

# ==== LogicSnapshot raw 存储路径 (P5 diff+ctz 毛刺滤波) ====
# 直接编译 raw 数据层源文件 (不链接 pxview-data), 内部 stub xlog.
# 依赖: snapshot / logicsnapshot / diskcache_writer / glitch_filter / mmap_allocator
#       + leaf_block_pool (header-only) + libsigrok 头 + common (xlog.h).
pv_add_qtest(test_logic_snapshot_raw
    data/test_logic_snapshot_raw.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_logic_snapshot_raw PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)
