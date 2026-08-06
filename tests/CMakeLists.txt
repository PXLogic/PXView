#===============================================================================
#= PXView MCP E2E Test Suite
#-------------------------------------------------------------------------------
# This directory is added via `add_subdirectory(tests)` from the top-level
# CMakeLists.txt when ENABLE_TESTS is ON.
#
# Tests are implemented as Python pytest files in suites/.
# They communicate with PXView via MCP (HTTP JSON-RPC 2.0 on port 10110).
# Requires PXView to be running in --headless mode.
#===============================================================================

find_package(Python3 COMPONENTS Interpreter)

if(NOT Python3_FOUND)
    message(STATUS "Python3 not found — MCP E2E tests will be skipped")
    return()
endif()

# ---- Install pytest + requests into the build tree ----
# This ensures the test runner has its dependencies available.
set(PYTEST_VENV_DIR "${CMAKE_BINARY_DIR}/pytest_venv")

if(WIN32)
    set(PYTEST_PYTHON "${PYTEST_VENV_DIR}/Scripts/python.exe")
    set(PYTEST_PIP "${PYTEST_VENV_DIR}/Scripts/pip.exe")
else()
    set(PYTEST_PYTHON "${PYTEST_VENV_DIR}/bin/python")
    set(PYTEST_PIP "${PYTEST_VENV_DIR}/bin/pip")
endif()

# Create venv and install deps (only if not already done)
add_custom_command(
    OUTPUT "${PYTEST_PYTHON}"
    COMMAND ${Python3_EXECUTABLE} -m venv "${PYTEST_VENV_DIR}"
    COMMAND "${PYTEST_PIP}" install --quiet pytest pytest-html requests
    COMMENT "Creating pytest virtual environment"
)
add_custom_target(pytest_venv DEPENDS "${PYTEST_PYTHON}")

# ---- Register CTest entries ----

# Option 1: Run all tests as a single CTest case
add_test(
    NAME "mcp_e2e_all"
    COMMAND "${PYTEST_PYTHON}" -m pytest
            "${CMAKE_CURRENT_SOURCE_DIR}/suites/"
            -v
            --tb=short
            --html="${CMAKE_BINARY_DIR}/test_report.html"
            --junitxml="${CMAKE_BINARY_DIR}/test_report.xml"
            --self-contained-html
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
set_tests_properties("mcp_e2e_all" PROPERTIES
    TIMEOUT 600
    DEPENDS pytest_venv
    ENVIRONMENT "PXVIEW_MCP_URL=http://127.0.0.1:10110/mcp"
)

# Option 2: Run by priority level (for faster feedback)
foreach(level p0 p1 p2)
    add_test(
        NAME "mcp_e2e_${level}"
        COMMAND "${PYTEST_PYTHON}" -m pytest
                "${CMAKE_CURRENT_SOURCE_DIR}/suites/"
                -v
                --tb=short
                -m "${level}"
                --junitxml="${CMAKE_BINARY_DIR}/test_report_${level}.xml"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    set_tests_properties("mcp_e2e_${level}" PROPERTIES
        TIMEOUT 300
        DEPENDS pytest_venv
        ENVIRONMENT "PXVIEW_MCP_URL=http://127.0.0.1:10110/mcp"
    )
endforeach()

#===============================================================================
#= C++ unit tests (Google Test)
#-------------------------------------------------------------------------------
# Header-only tests that verify Core/View layer logic without requiring a full
# Qt widget stack. Uses MockViewLayout (from iview_delegates.h), CursorRegistry
# (from core/cursorregistry.h), and TraceVisitor (from view/trace_visitor.h).
#===============================================================================
add_subdirectory(cpp)
