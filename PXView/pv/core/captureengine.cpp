/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/core/captureengine.h"

#include "pv/core/capturemanager.h"

namespace pv {
namespace core {

bool CaptureEngine::submit(const CaptureIntent &intent)
{
    std::lock_guard<std::mutex> lock(_exec_mutex);
    switch (_policy) {
    case ExecutionPolicy::SerialInline:
        return _mgr.start_capture(intent.instant, intent.owner);
    case ExecutionPolicy::SerialQueue:
        // 预留：届时由 engine 自有线程串行消费提交队列，submit 变为入队 +
        // 应答回投（经 EventBus）。启用前回退 inline 并告警，保证行为与
        // 既有 GUI 同步语义完全一致。
        pxv_warn("CaptureEngine: SerialQueue policy not enabled yet; "
                 "executing inline (submit owner=%p instant=%d)",
                 (void *)intent.owner, (int)intent.instant);
        return _mgr.start_capture(intent.instant, intent.owner);
    }
    return false;
}

bool CaptureEngine::stop()
{
    std::lock_guard<std::mutex> lock(_exec_mutex);
    return _mgr.stop_capture();
}

} // namespace core
} // namespace pv
