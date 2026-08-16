/*
 * view_context.cpp — ViewContext 实现
 *
 * 目前为空。所有 ViewContext 方法都是 inline 在 view_context.h 中。
 *
 * from_view() 工厂方法需要 View 的完整定义，
 * 其实现放在 view_data_sync.cpp 中（主程序的一部分），
 * 以避免测试链接时需要 View 的完整实现。
 *
 * 此文件仍然存在于构建系统中，作为未来非 inline 方法的扩展点。
 */

#include "view_context.h"

// 所有方法已 inline 在 view_context.h 中。
// from_view() 的实现在 view_data_sync.cpp 中。
