#===============================================================================
#= PXView C++ Unit Tests (QTest + CTest)
#-------------------------------------------------------------------------------
# This directory is added via `add_subdirectory(tests)` from the top-level
# CMakeLists.txt when ENABLE_TESTS is ON.
#
# 历史说明（2026-09-16）：本目录曾注册 MCP E2E pytest 测试（mcp_e2e_all /
# mcp_e2e_p0/p1/p2 + pytest_venv 自定义目标）。这些用例依赖运行中的 PXView
# MCP 端口（10110）与 pytest 虚拟环境，在常规 ctest 流程中恒为 Not Run，
# 已从 CTest 移除。MCP 端到端验证改用独立脚本：
#   - tests/check_mcp_gui_sync.py   交互式检查表（--no-send 可仅打印）
#   - tests/suites/                 pytest 套件（手动启动 headless 后运行）
# CTest 仅保留纯 C++/Qt 单元测试 —— 无 Python 依赖，无环境门槛。
#===============================================================================

#===============================================================================
#= C++ unit tests (QTest + CTest)
#-------------------------------------------------------------------------------
# QTest-based unit tests for Core/Data/API/View layers.
# Test files live in tests/qtest/ with subdirectories: core/, data/, api/, view/.
#===============================================================================
add_subdirectory(qtest)
