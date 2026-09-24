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
        ${MIMALLOC_LIB}
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

# ==== P1-d DSO 电压换算因子契约（含已知缺陷表征）====
# 审计结论见 devdoc/P0-P1-P2架构设计与实施计划.md §4.9:
# 4 处 V/div 换算不是重复实现; convert_voltage 里 vfactor 被乘了两次(已知缺陷)。
# 依赖: measure_format.cpp (纯函数) + pxvdef.h (kDsoVdivs 等常量)。
pv_add_qtest(test_voltage_scale_factors
    core/test_voltage_scale_factors.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/core/measure_format.cpp
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

# ==== FilterProcessor 离线并发/排队单测（缺口审计③）====
# 覆盖此前只有手工 MCP pytest 套件才碰得到、CTest 零覆盖的 Core 逻辑：
# 清除排队（park 在 _edit_mutex）、_edit_intent_seq 最新写者优先、
# 同步清除等待真实还原、无活动时空操作。外壳同 test_decode_task_manager：
# FakeDispatcher 排队异步事件（测试内 drain 显式投递，不需要 Qt 事件循环），
# StubSession 提供真实 SessionData（内含真实 LogicSnapshot）。
pv_add_qtest(test_filter_processor
    core/test_filter_processor.cpp
    ${_CM_TEST_COMMON_SOURCES}
)
target_include_directories(test_filter_processor PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/libsigrokdecode
)
target_sources(test_filter_processor PRIVATE
    ${PXVIEW_INCLUDE_DIR}/pv/base/dstimer.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/stack/decoderstack.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/model/signalmodel.h
)
target_link_libraries(test_filter_processor PRIVATE pxview-core Qt6::Gui)
target_link_options(test_filter_processor PRIVATE -Wl,--no-as-needed)
if(WIN32)
    target_link_libraries(test_filter_processor PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
elseif(NOT APPLE)
    target_link_libraries(test_filter_processor PRIVATE
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
# test_result_types.cpp includes pv/api/types.h, which transitively pulls in
# pv/interface/event_notification.h -> <nlohmann/json.hpp>. On MSYS2/Windows
# nlohmann_json is fetched via FetchContent; its include path is only exposed
# through the nlohmann_json::nlohmann_json imported target (linked INTERFACE by
# pxview-interface). Without this link the MinGW CI build fails with
# "fatal error: nlohmann/json.hpp: No such file or directory".
target_link_libraries(test_result_types PRIVATE pxview-interface)

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

# ==== 波形光栅化纯函数直测 (modernize-thread-model Task 2.6) ====
# 直接编译 rasterize.cpp + 三个快照数据层源文件, 内部 stub xlog。
# 无 View/QWidget 依赖: 直接构造 snapshot + 值参数, 用 QImage 承载 QPainter
# 调 rasterize_logic_channel / rasterize_dso_channel / rasterize_analog_channel
# 并断言像素 (logic 波形/glitch live-preview overlay/DSO+Analog 双模式)。
pv_add_qtest(test_rasterize
    view/test_rasterize.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/view/renderer/rasterize.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/dsosnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/analogsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_rasterize PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
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

# ==== P2 决策门禁: 采集入队 staging 拷贝成本量化 ====
# 量化 logicsnapshot_diskcache_writer.cpp:140 那笔唯一整包拷贝的 CPU/内存代价,
# 对比 现状 / A3 池化 / A1 零拷贝 三个变体, 为"是否改动 libsigrok 数据包契约"
# 提供数据。依赖链同 test_logic_snapshot_cross_throughput。
pv_add_qtest(test_feed_staging_copy_cost
    data/test_feed_staging_copy_cost.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_feed_staging_copy_cost PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== P1-a 解码 chunk 对齐契约回归 ====
# 锁定 decoderstack.cpp P1-a 修复依赖的两条不变式:
#   1) 迭代器指针按字节向下取整 (非 8 对齐起点 == 对齐起点同指针)
#   2) get_iterator_valid_length() 精确到 leaf block 末尾, 窗口不跨块
# 覆盖 2 个 leaf block + 非 8 对齐起点。依赖链同 test_logic_snapshot_query。
pv_add_qtest(test_decode_chunk_alignment
    data/test_decode_chunk_alignment.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_decode_chunk_alignment PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== P1-b mmap 区域生命周期契约回归 ====
# 锁定 mmap_region() 的引用计数语义: 读者钉住的映射在释放前不被解除;
# 几何变更重建 allocator 而旧映射存活; 几何一致复用同一 allocator。
# 依赖链同 test_logic_snapshot_query。
pv_add_qtest(test_mmap_region_lifetime
    data/test_mmap_region_lifetime.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_mmap_region_lifetime PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)

# ==== P1-c 统一读取抽象 SampleSpan 契约回归 ====
# 锁定三种存储布局（logic 位打包块树 / dso 通道平面 / analog 交织）经同一
# SampleSpan 结构暴露时的语义: start_sample 向下取整、contiguous_samples 硬上界、
# stride / bits_per_sample 正确, 并与旧 get_sample(s) 对拍。
pv_add_qtest(test_sample_span
    data/test_sample_span.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/snapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_glitch_filter.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_pattern_search.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/logicsnapshot_edge_scan.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/dsosnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/snapshot/analogsnapshot.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/data/cache/mmap_allocator.cpp
)
target_include_directories(test_sample_span PRIVATE
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
    # measure_format.cpp: 测试用 core::convert_voltage 验证"测量档位上下文
    # (measure_voltage_factor + measure_probe_factor + data_scale) -> 伏特"
    # 的端到端量纲，与 test_voltage_scale_factors 共用同一实现。
    ${PXVIEW_INCLUDE_DIR}/pv/core/measure_format.cpp
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

# ==== 多 Tab (Session-Centric) 集成契约测试 ====
# 锁定 Session-Centric 演进引入的契约：per-tab 状态机、SignalModel
# stash/restore、CaptureBuffers 执行缓冲、DeviceManager handle 身份与
# 稳定性（原"关闭文件重开 handle 错位"回归保护）。
# 链接桩外壳同 test_session_document：复用 pxview-core 预编译对象 +
# libsigrokdecode/common 直编 + libsigrok --whole-archive。
pv_add_qtest(test_multi_tab_session
    core/test_multi_tab_session.cpp
    ${_CM_TEST_COMMON_SOURCES}
)
target_include_directories(test_multi_tab_session PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/libsigrokdecode
)
target_sources(test_multi_tab_session PRIVATE
    ${PXVIEW_INCLUDE_DIR}/pv/base/dstimer.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/stack/decoderstack.h
    ${PXVIEW_INCLUDE_DIR}/pv/data/model/signalmodel.h
)
target_link_libraries(test_multi_tab_session PRIVATE pxview-core Qt6::Gui)
target_link_options(test_multi_tab_session PRIVATE -Wl,--no-as-needed)
if(WIN32)
    target_link_libraries(test_multi_tab_session PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
elseif(NOT APPLE)
    target_link_libraries(test_multi_tab_session PRIVATE
        "-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
endif()

# ==== DiskCache 子系统 (config/mmap) ====
# P0 (零拷贝架构): DiskBufferManager / DiskWriteThread / DiskReadCache 三件套已删除
# —— 它们是被 MmapAllocator 取代的第二套（拷贝式）磁盘缓存实现，生产代码零调用，
# 设计方向与零拷贝相反（submit 内部 malloc 拷贝、load malloc 物化整块）。
# 保留的 cache 层只有 mmap_allocator.cpp（唯一存储后端，已由各 snapshot 测试覆盖）
# 与 disk_cache_config.h（MmapAllocator 的配置结构）。

# ==== PopupDlgList 遍历安全（重入 erase / 悬挂条目）====
# 直接编译 popupdlglist.cpp（含 pv/base/log.h，测试内 stub xlog）。
# 覆盖 0xc0000374 那一类生命周期 bug 中的"容器重入"分支：
#   * close() 经 closeEvent → ~PxDialog/~DSMessageBox → RemoveDlgFromList() 重入改同一个
#     vector —— 旧实现拿 begin()+i 跨过 close() 再 erase，会删掉旁观者甚至越界；
#   * 对话框被控件树销毁而没走 RemoveDlgFromList 时，裸指针条目会悬挂，
#     现已改 QPointer<QWidget> 自动置空并丢弃。
pv_add_qtest(test_popup_dlg_list
    ui/test_popup_dlg_list.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/ui/popupdlglist.cpp
)
target_include_directories(test_popup_dlg_list PRIVATE
    ${CMAKE_SOURCE_DIR}/common
)

# ==== csv 输出模块的通道计数 / 采样数契约（"导出 0 字节"回归）====
# 只走 libsigrok 公开 API 造一个假设备（sr_dev_inst_user_new +
# sr_dev_inst_channel_add / sr_dev_inst_channels_get / sr_dev_channel_enable），
# 再直接驱动 csv 输出模块，锁住两件事：
#   * 设备上还有本模块消费不了的类型（DSO）时，channel_count 与 channels_seen
#     必须同口径，否则每个数据包都被 "Discarding partial packet" 丢掉；
#   * 末尾短包必须按实际样本数输出，不能按上一包的 num_samples 补脏行。
# 旧实现在这些用例上确定性失败（24 行只写出 16 行 / 19 行变成 24 行）。
#
# 注意：这里**故意不**用 -Wl,--whole-archive（其它几个测试用）。本测试只用公开
# API，而 output.c 的 output_module_list[] 直接取 &output_csv 的地址，链接
# sr_output_find 就会把 csv.o 拉进来，无需全量保留。
# 反过来，本机 binutils（GNU ld 2.47.20260726）在 whole-archive 且存在未解析
# 符号时**静默失败**（collect2 只报 "ld returned 5 exit status"，零诊断），
# 而其它测试能过是因为它们额外编入了 common/ + libsigrokdecode/ 对象并链接
# pxview-core/pxview-data 等（已用最小对照实验定位）。
pv_add_qtest(test_csv_export_channels
    data/test_csv_export_channels.cpp
)
target_include_directories(test_csv_export_channels PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)
# 普通链接（非 whole-archive）：只要被引用到的成员。
target_link_libraries(test_csv_export_channels PRIVATE libsigrok)

# ==== 导入/导出模块选项 + 导出格式白名单回归（".srzip 为 0 字节"）====
# 锁定三件事：
#   * 导出格式白名单：枚举真实的 sr_output_list()，srzip/null 仍在列表里但必须
#     被拒绝（旧代码"枚举全部"就会把它们列出来 —— 0 字节文件的入口），
#     csv/binary/vcd 等仍可用，非逻辑数据只允许 csv；
#   * 选项值类型契约：make_variant() 必须按 sr_option::def 的类型产出，
#     错型返回 nullptr —— sr_input_new() 对混型/未知键是整单拒绝的；
#   * 端到端：binary 模块 + numchannels=16 → 实例 16 通道（模块默认是 8）。
# 只走 libsigrok 公开 API；链接方式同 test_csv_export_channels（普通链接）。
pv_add_qtest(test_sr_options
    data/test_sr_options.cpp
)
target_include_directories(test_sr_options PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(test_sr_options PRIVATE libsigrok)

# ==== 「打开 .pxl / .sr / .srzip」可读性回归 ====
# 文件 → 打开 的过滤串新增了 *.sr / *.srzip（加载器 sr_session_load_file_device
# 本来就支持 upstream sigrok 的 version/metadata + logic-N 容器）。本测试把
# srzip 输出模块（唯一的 SR_OUTPUT_INTERNAL_IO_HANDLING 模块，自己写文件）产出的
# 归档用**同一个 API** 读回来，锁住通道数与采样率的往返一致——任何让这两个后缀
# 变成"能选但读不了"的改动都会在 CI 里失败。
pv_add_qtest(test_srzip_load
    data/test_srzip_load.cpp
)
target_include_directories(test_srzip_load PRIVATE
    ${CMAKE_SOURCE_DIR}/libsigrok/include
    ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(test_srzip_load PRIVATE libsigrok)

# ==== pv::prop::Int 编辑器的取值范围契约（uint64 被钉死在 0 的回归）====
# 只编 property.cpp + int.cpp + dsspinbox.cpp，Qt Widgets 环境（ctest 已设
# QT_QPA_PLATFORM=offscreen），不链接 libsigrok。
pv_add_qtest(test_prop_int_widget
    ui/test_prop_int_widget.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/prop/property.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/prop/int.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/ui/dsspinbox.cpp
)
# GVariant 用到 glib（该目标不经 libsigrok，需显式链接；link_directories 已在
# deps.cmake 全局设置，与 test_signal_model/test_rowdata_batch 同法）。
target_link_libraries(test_prop_int_widget PRIVATE -lglib-2.0)

# ==== raw binary 文件名的参数提示（通道数/采样率）====
# 约定 "<base>-<N>ch-<rate>Hz.binary"：导出时写入、导入时填入选项对话框。
# 纯逻辑（只用 QRegularExpression），不链接 libsigrok。
pv_add_qtest(test_binary_name_hints
    data/test_binary_name_hints.cpp
)

