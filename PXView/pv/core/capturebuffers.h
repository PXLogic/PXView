/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_CORE_CAPTUREBUFFERS_H
#define PXVIEW_PV_CORE_CAPTUREBUFFERS_H

#include <atomic>
#include <memory>
#include <vector>

#include "pv/data/document/sessiondata.h"

namespace pv {
namespace core {

// Session-Centric 阶段6：执行缓冲所有权对象。
//
// 把原 SessionStateContext 的 _data_list/_view_data/_capture_data
// 物理迁移到独立的 CaptureBuffers，由 SigSession 构造早期创建，注入：
//   * CaptureManager —— 执行层所有者（对应参考项目"数据内嵌执行单元，
//     无全局缓冲"：PulseView Session::logic_data_ / ATK Session::Segment）；
//   * SessionStateContext —— 仅作同名转发（几十处 _state->view_data()
//     调用面零改动）。
//
// 语义重定义：这不是"会话数据"——数据真相是 per-tab SessionDocument 的
// 快照（阶段3a 起零拷贝共享直达 ctx）；CaptureBuffers 只是 CaptureEngine
// 执行层的 per-capture-frame 工作缓冲（view/capture 双缓冲供 repeat 帧交
// 换），生命周期与任何 tab 无关。
class CaptureBuffers {
public:
    CaptureBuffers()
    {
        // Track B1: data buffers owned via unique_ptr（沿用原双缓冲语义）
        _data_list.push_back(std::make_unique<SessionData>());
        _data_list.push_back(std::make_unique<SessionData>());
        _view_data = _data_list[0].get();
        _capture_data = _data_list[0].get();
    }

    ~CaptureBuffers()
    {
        // unique_ptr auto-releases SessionData objects.
        // Still call clear() on each to release snapshot data first.
        for (auto &p : _data_list) {
            if (p)
                p->clear();
        }
        _data_list.clear();
    }

    CaptureBuffers(const CaptureBuffers &) = delete;
    CaptureBuffers &operator=(const CaptureBuffers &) = delete;

    // --- 线程模型（沿用原 SessionStateContext 语义） ---
    // Writers (main thread) use store(); readers (decode thread, data feed
    // thread) use load() — atomic 指针保证跨线程安全。
    SessionData *view_data() const
    {
        return _view_data.load(std::memory_order_acquire);
    }
    void set_view_data(SessionData *d)
    {
        _view_data.store(d, std::memory_order_release);
    }
    SessionData *capture_data() const
    {
        return _capture_data.load(std::memory_order_acquire);
    }
    void set_capture_data(SessionData *d) { _capture_data = d; }

    std::vector<std::unique_ptr<SessionData>> &data_list() { return _data_list; }
    bool is_single_buffer() const
    {
        return _view_data.load() == _capture_data.load();
    }

private:
    std::vector<std::unique_ptr<SessionData>> _data_list;
    std::atomic<SessionData *> _view_data{nullptr};
    std::atomic<SessionData *> _capture_data{nullptr};
};

} // namespace core
} // namespace pv

#endif // PXVIEW_PV_CORE_CAPTUREBUFFERS_H
