#===============================================================================
#= PXView C++ Unit Tests
#-------------------------------------------------------------------------------
# Header-only unit tests for Core/View layer logic that can be tested without
# a full Qt widget stack or running PXView instance.
#
# Test targets:
#   test_view_layout    — MockViewLayout / IViewLayout interface contract
#   test_cursor_registry — CursorRegistry add/remove/position logic
#   test_trace_visitor   — TraceVisitor type dispatch pattern
#
# All tests use Google Test (fetched via FetchContent). The test source files
# include project headers directly (no library linking required because the
# classes under test are either header-only or have inline implementations).
#===============================================================================

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
# For Windows: prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Common include paths — point to the project root so test files can include
# headers like "pv/view/iview_delegates.h", "pv/core/cursorregistry.h", etc.
set(PXVIEW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/PXView")

# ---- test_view_layout: header-only (MockViewLayout in iview_delegates.h) ----
add_executable(test_view_layout test_view_layout.cpp)
target_include_directories(test_view_layout PRIVATE ${PXVIEW_INCLUDE_DIR})
target_link_libraries(test_view_layout PRIVATE GTest::gtest_main)
target_compile_features(test_view_layout PRIVATE cxx_std_17)
add_test(NAME test_view_layout COMMAND test_view_layout)

# ---- test_cursor_registry: compiles cursorregistry.cpp (no Qt deps) ----
add_executable(test_cursor_registry
    test_cursor_registry.cpp
    ${PXVIEW_INCLUDE_DIR}/pv/core/cursorregistry.cpp
)
target_include_directories(test_cursor_registry PRIVATE ${PXVIEW_INCLUDE_DIR})
target_link_libraries(test_cursor_registry PRIVATE GTest::gtest_main)
target_compile_features(test_cursor_registry PRIVATE cxx_std_17)
add_test(NAME test_cursor_registry COMMAND test_cursor_registry)

# ---- test_trace_visitor: header-only (TraceVisitor in trace_visitor.h) ----
add_executable(test_trace_visitor test_trace_visitor.cpp)
target_include_directories(test_trace_visitor PRIVATE ${PXVIEW_INCLUDE_DIR})
target_link_libraries(test_trace_visitor PRIVATE GTest::gtest_main)
target_compile_features(test_trace_visitor PRIVATE cxx_std_17)
add_test(NAME test_trace_visitor COMMAND test_trace_visitor)
