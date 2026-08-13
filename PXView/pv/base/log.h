/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef _PXV_LOG_H_
#define _PXV_LOG_H_

#include <QString>
#include <assert.h>
#include <log/xlog.h>

extern xlog_writer *pxv_log;

void pxv_log_init();
void pxv_log_uninit();

xlog_context *pxv_log_context();
void pxv_log_level(int l);

void pxv_log_enalbe_logfile(bool append);
void pxv_remove_log_file();
void pxv_clear_log_file();
void pxv_set_log_file_enable(bool flag);

QString get_pxv_log_path();

#define LOG_PREFIX ""
#define pxv_err(fmt, args...) xlog_err(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_warn(fmt, args...) xlog_warn(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_info(fmt, args...) xlog_info(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_dbg(fmt, args...) xlog_dbg(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_detail(fmt, args...) xlog_detail(pxv_log, LOG_PREFIX fmt, ## args)

// ============================================================================
// pxv_assert: 不弹 Windows 模态对话框的 assert 替代。
//
// 背景: Windows C Runtime 的 assert() 在失败时会弹出模态对话框
// (Abort/Retry/Ignore),该对话框运行自己的消息泵,会强制推进 qApp 事件循环,
// 把排队的 broadcast_async<T> 事件强行派发,造成 EventBus _broadcast_depth
// 护栏被打穿,形成"assert 弹窗 → 消息泵重入 → EventBus 嵌套 → 又一个 assert
// 弹窗"的死循环,最终导致状态不一致与 SIGSEGV。
//
// 行为:
// - Release (NDEBUG): pxv_err 记录 + no-op(符合 project_memory.md 规则:
//   "assert 在 Release 应为 no-op,所有指针检查必须前置显式 if(!ptr) 检查")
// - Debug + Windows:
//   * 挂载调试器(IsDebuggerPresent): pxv_err + __debugbreak()(SIGTRAP,
//     gdb 捕获,不弹窗)
//   * 未挂调试器: pxv_err + abort()(直接终止,不弹窗)
// - Debug + 非 Windows: pxv_err + abort()
//
// 关键: 不调用 C Runtime 的 assert(),避免触发 Windows 模态对话框。
// ============================================================================

#ifdef NDEBUG
  #define pxv_assert(cond, fmt, args...) \
    do { if (!(cond)) { pxv_err(fmt, ## args); } } while (0)
#else
  #ifdef _WIN32
    extern "C" __declspec(dllimport) int __stdcall IsDebuggerPresent(void);
    #define pxv_assert(cond, fmt, args...) \
      do { if (!(cond)) { pxv_err(fmt, ## args); \
           if (IsDebuggerPresent()) { __debugbreak(); } \
           else { abort(); } \
      } } while (0)
  #else
    #define pxv_assert(cond, fmt, args...) \
      do { if (!(cond)) { pxv_err(fmt, ## args); abort(); } } while (0)
  #endif
#endif

// ============================================================================
// 重定义 assert 宏: 不弹 Windows 模态对话框,改为 pxv_assert。
// 避免 assert 弹窗的消息泵重入打穿 EventBus 护栏。
// project_memory.md 规则: assert 在 Release 应为 no-op。
// 所有 include "log.h" 的文件中的 assert() 自动变为不弹窗版本。
// ============================================================================
#undef assert
#define assert(cond) pxv_assert(cond, "Assertion failed: %s, file %s, line %d", #cond, __FILE__, __LINE__)

// Verbose repeat-mode logging gate. Set to 1 to enable per-frame repeat
// diagnostics (capture restart, decode done, analog trigger evaluation).
#ifndef PXV_VERBOSE_REPEAT_LOG
#define PXV_VERBOSE_REPEAT_LOG 0
#endif

#endif
