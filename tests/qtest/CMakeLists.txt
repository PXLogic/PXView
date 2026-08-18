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

# ==== Memory/UB sanitizer gate (harden-review-findings T13 / R5) ====
# Enable with: cmake .. -DPXVIEW_ENABLE_SANITIZERS=ON
# Applies -fsanitize=address,undefined to every pv_add_qtest target. Default
# OFF so normal builds are unaffected. Sources compiled directly into a test
# executable (e.g. logicsnapshot.cpp) get instrumented, exposing leaks/UAF/UB.
option(PXVIEW_ENABLE_SANITIZERS
    "Build PXView qtest targets with AddressSanitizer + UBSan" OFF)

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
    # Memory/UB sanitizer gate (see option above).
    if(PXVIEW_ENABLE_SANITIZERS)
        target_compile_options(${target_name} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target_name} PRIVATE
            -fsanitize=address,undefined)
    endif()
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

# ==== CaptureManager 离线状态机/查询单测 (Spec harden-review T7) ====
# MockSessionState 实现完整 ISessionState (pv::) + ISessionCoordination。链接方式:
#   * pxview-core 静态库 (capturemanager/eventbus/log/deviceagent + pxview-data/
#     config 传递提供) —— 复用预编译对象, 不重新枚举 core/data 源码。
#   * AUTOMOC 为 test.cpp 直接/间接包含的 QObject 头 (dstimer/decoderstack/
#     signalmodel/spectrumstack/mathstack/lissajousmodel 等) 生成 moc, 补上静态库
#     不携带的 QObject vtable/meta 符号 (静态库只带方法体, moc 在 exe 侧编译)。
#   * libsigrokdecode + common/*.c 直接编入目标, 提供 decoderstack 的 srd_* 与
#     deviceagent/驱动 的 unz_*/xlog_*。
#   * libsigrok 以 --whole-archive 链接 (对齐主 exe, deviceagent 的 sr_* 依赖)。
# GUI 的 LangResource/MsgBox 在测试内打桩 (离线路径不触达)。
# pxv_log 由 pxview-core 的 log.cpp 提供, 测试不再自行定义。
set(_CM_TEST_COMMON_SOURCES
    ${common_SOURCES}
    ${libsigrokdecode_SOURCES}
)
pv_add_qtest(test_capture_manager
    core/test_capture_manager.cpp
    ${_CM_TEST_COMMON_SOURCES}
)
target_include_directories(test_capture_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/libsigrokdecode
)
# 显式把被间接包含的 QObject 头列为源, 让 AUTOMOC 为主被拉取的 core/data 类
# (DsTimer / DecoderStack) 生成 vtable + metaobject (静态库只带方法体, 不携带 moc)。
target_sources(test_capture_manager PRIVATE
    ${PXVIEW_INCLUDE_DIR}/pv/base/dstimer.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/stack/decoderstack.h
)
# 顺序: 须把 Qt6::Gui 放在静态库之后, 否则 MinGW 的 --as-needed 会在静态库对象
# (pxview-config 的 QColor::fromString/lightness) 引用到之前就丢弃 Qt6Gui 导入库。
target_link_libraries(test_capture_manager PRIVATE pxview-core Qt6::Gui)
# 一锤定音: CMake 会把 pxview-core 的传递静态库 (config/data) 追加在显式 Qt6Gui
# 之后, 主 exe 靠自身 GUI 符号保住 Qt6Gui, 本测试自身对象很少引 QtGui → --as-needed
# 仍可能在 config 需要 QColor 前丢弃 Qt6Gui。显式关闭 --as-needed 确保 Qt6Gui 保留。
target_link_options(test_capture_manager PRIVATE -Wl,--no-as-needed)
if(WIN32)
    target_link_libraries(test_capture_manager PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
elseif(NOT APPLE)
    target_link_libraries(test_capture_manager PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
endif()

# ==== DecodeTaskManager 离线状态机/查询单测 (Spec harden-review T8) ====
# 与 test_capture_manager 同一套"链接桩外壳": 复用 pre-编译的 pxview-core +
# pxview-data + Hermes对象 (mock), 不重新枚举 core/data 源码。
#   * pxview-core 静态库 (decodetaskmanager/eventbus/documentregistry/log +
#     pxview-data/config 传递提供) —— 复用预编译对象。
#   * AUTOMOC 为被间接包含的 QObject 头 (dstimer/decoderstack) 生成 moc, 补上静态库
#     不携带的 vtable/meta 符号。
#   * libsigrokdecode + common/*.c 直接编入目标, 提供 decoderstack 的 srd_* 与
#     xlog_* (pxv_log 由 pxview-core 的 log.cpp 提供)。
#   * libsigrok 以 --whole-archive 链接 (对齐主 exe / test_capture_manager)。
pv_add_qtest(test_decode_task_manager
    core/test_decode_task_manager.cpp
    ${_CM_TEST_COMMON_SOURCES}
)
target_include_directories(test_decode_task_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/libsigrokdecode
)
target_sources(test_decode_task_manager PRIVATE
    ${PXVIEW_INCLUDE_DIR}/pv/base/dstimer.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/stack/decoderstack.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/model/signalmodel.h
)
target_link_libraries(test_decode_task_manager PRIVATE pxview-core Qt6::Gui)
target_link_options(test_decode_task_manager PRIVATE -Wl,--no-as-needed)
if(WIN32)
    target_link_libraries(test_decode_task_manager PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
elseif(NOT APPLE)
    target_link_libraries(test_decode_task_manager PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
endif()

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
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_logic_snapshot_raw PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== LogicSnapshot 查询原语对拍 (B5: get_samples / nxt/pre edge / display edges) ====
# 依赖链同 test_logic_snapshot_raw。
pv_add_qtest(test_logic_snapshot_query
    data/test_logic_snapshot_query.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_logic_snapshot_query PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== C1/C2 性能门禁 + B5 mmap round-trip ====
# CROSS 位拷贝吞吐 / mmap 持续写吞吐 / mmap 读回哈希一致。
# 依赖链同 test_logic_snapshot_raw。
pv_add_qtest(test_logic_snapshot_cross_throughput
    data/test_logic_snapshot_cross_throughput.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_logic_snapshot_cross_throughput PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== EventBus 并发/析构语义 (harden-review-findings T6 / R2) ====
# 测试文件内定义 pxv_log 桩; 链接 eventbus + QtAsyncDispatcher + xlog.c 提供 xlog_warn。
pv_add_qtest(test_eventbus
    core/test_eventbus.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/core/eventbus.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/core/qt_async_dispatcher.cpp
    ${CMAKE_SOURCE_DIR}/common/log/xlog.c
)

# ==== LogicSnapshot cross 分块对齐 (harden-review-findings T5 / R3) ====
# 依赖链同 test_logic_snapshot_raw。
pv_add_qtest(test_logic_snapshot_cross_alignment
    data/test_logic_snapshot_cross_alignment.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_logic_snapshot_cross_alignment PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== Glitch Filter 纯函数单测 (harden-review-findings T9 / R4) ====
# 依赖链同 test_logic_snapshot_raw; 覆盖 apply_glitch_filter_one_pass 阈值语义。
pv_add_qtest(test_glitch_filter
    data/test_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_glitch_filter PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== LogicSnapshot pattern 搜索子系统 + 显示边沿转发 (B5 扩展) ====
# pattern_search() / get_display_edges() / get_nxt_edge() / get_pre_edge()
# 的子系统实现 + 转发路径测试。依赖链同 test_logic_snapshot_query。
pv_add_qtest(test_logic_snapshot_pattern_search
    data/test_logic_snapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_logic_snapshot_pattern_search PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== AnalogSnapshot 数据层 (first_payload/append_payload/envelope/范围) ====
# 依赖: analogsnapshot + snapshot + libsigrok 头 + common (xlog.h)。内部 stub xlog。
pv_add_qtest(test_analog_snapshot
    data/test_analog_snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/analogsnapshot.cpp
)
target_include_directories(test_analog_snapshot PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== DsoSnapshot 数据层 (first_payload/append_payload/envelope/instant) ====
# 依赖: dsosnapshot + snapshot + libsigrok 头 + common (xlog.h)。内部 stub xlog。
pv_add_qtest(test_dso_snapshot
    data/test_dso_snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/dsosnapshot.cpp
)
target_include_directories(test_dso_snapshot PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== SignalModel / SignalConfigStore 数据层 ====
# signalmodel.cpp / signalconfigstore.cpp 使用 g_free/g_strdup (glib), 显式链接
# -lglib-2.0 (link_directories 已在 deps.cmake 全局设置)。snapshot.cpp 编译进目标
# 仅为提供 Snapshot 抽象基类的符号 (测试定义了 TestSnapshot 子类)。
pv_add_qtest(test_signal_model
    data/test_signal_model.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/model/signalmodel.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/model/signalconfigstore.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
)
target_include_directories(test_signal_model PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(test_signal_model PRIVATE -lglib-2.0)

# ==== RowData 批量/逐注解落库等价性（方案 E）====
# 直接编译 decode 数据层源文件（不链接 pxview-data），内部 stub xlog。
# 依赖: rowdata / annotation / annotationrestable / decoderstatus / annotation_heap
#       + libsigrokdecode 头 + common (xlog.h)。PXVIEW_DECODE_PERF 下 rowdata.cpp
#       经 perflog.h 引入 glib.h（全局 include/link dirs 已就绪，-lglib-2.0 显式链接）。
pv_add_qtest(test_rowdata_batch
    data/test_rowdata_batch.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/decode/rowdata.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/decode/annotation.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/decode/annotationrestable.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/decode/decoderstatus.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/decode/annotation_heap.cpp
)
target_include_directories(test_rowdata_batch PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrokdecode
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(test_rowdata_batch PRIVATE -lglib-2.0)

# ==== SessionData/SessionDocument/SessionSnapshot (data 层 document trio) ====
# 走 Route B: 链接 pxview-core 静态库 (复用预编译 core/data/config 对象), 与
# test_capture_manager / test_decode_task_manager 同一套链接桩外壳:
#   * pxv_log 由 pxview-core 的 log.cpp 提供; xlog_* 由 common/log/xlog.c 提供
#     (测试文件内不再自行定义, 否则重复符号 —— 已从 test_session_document.cpp 移除)。
#   * libsigrokdecode + common/*.c 直接编入目标, 提供 decoderstack 的 srd_* 与
#     xlog_*; libsigrok 以 --whole-archive 链接。
#   * AUTOMOC 为被间接包含的 QObject 头 (dstimer/decoderstack) 生成 moc, 补上
#     静态库不携带的 vtable/meta 符号。
pv_add_qtest(test_session_document
    data/test_session_document.cpp
    ${_CM_TEST_COMMON_SOURCES}
)
target_include_directories(test_session_document PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/libsigrokdecode
)
target_sources(test_session_document PRIVATE
    ${PXVIEW_INCLUDE_DIR}/pv/base/dstimer.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/stack/decoderstack.h
)
# 顺序: 须把 Qt6::Gui 放在静态库之后 (见 test_capture_manager 注释)。
target_link_libraries(test_session_document PRIVATE pxview-core Qt6::Gui)
target_link_options(test_session_document PRIVATE -Wl,--no-as-needed)
if(WIN32)
    target_link_libraries(test_session_document PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
elseif(NOT APPLE)
    target_link_libraries(test_session_document PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
endif()

# ==== DiskCache 子系统 (config/mmap/buffer-manager/write-thread/read-cache) ====
# 5 个 cache 类编译进目标, 内部 stub xlog。纯数据层, 无 QWidget 依赖。
pv_add_qtest(test_disk_cache
    data/test_disk_cache.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/disk_buffer_manager.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/disk_read_cache.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/disk_write_thread.cpp
)
target_include_directories(test_disk_cache PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)
