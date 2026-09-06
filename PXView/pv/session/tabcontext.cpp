/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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
 * Foundation, Inc., 51 Franklin St, Boston, MA  02110-1301 USA
 */

#include "pv/session/tabcontext.h"
#include "pv/session/sigsession.h"
#include "pv/core/documentregistry.h"
#include "pv/view/view.h"
#include "pv/view/signal/signal.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/session/deviceagent.h"
#include "pv/base/log.h"
#include <QDebug>
#include <algorithm>

namespace pv {

int TabContext::_next_session_id = 1;

TabContext::TabContext(view::View *view, SigSession *session, data::SessionDocument *doc,
                       size_t doc_index, core::DocumentRegistry *registry) :
    _view(view),
    _session(session),
    _document(doc),
    _doc_index(doc_index),
    _doc_registry(registry),
    _title(QString("Session %1").arg(_next_session_id)),
    _file_path(""),
    _state(LIVE),
    _timestamp(QDateTime::currentDateTime())
{
    _next_session_id++;
    if (_doc_index != SIZE_MAX)
        _owned_doc_indices.push_back(_doc_index);
}

TabContext::~TabContext()
{
    // phase 2 + rebind model v3: drop the REGISTRY reference for documents
    // created by this tab. Strong references held by other tabs keep a
    // document alive regardless of ordering — there is no detach-before-
    // destroy requirement anymore; a document dies when the last holder
    // (registry or tab) drops it.
    if (_doc_registry) {
        for (size_t idx : _owned_doc_indices)
            _doc_registry->release_document(idx);
    }
    _pinned_docs.clear();
    _doc_ref.reset();
    _document = nullptr;
    _doc_index = SIZE_MAX;
}

// ---- Shared-reference ownership bookkeeping (rebind model v3) ----

bool TabContext::owns_doc_index(size_t idx) const
{
    return idx != SIZE_MAX &&
           std::find(_owned_doc_indices.begin(), _owned_doc_indices.end(),
                     idx) != _owned_doc_indices.end();
}

void TabContext::mark_document_owned(size_t idx)
{
    if (idx != SIZE_MAX && !owns_doc_index(idx))
        _owned_doc_indices.push_back(idx);
}

void TabContext::unpin_document(size_t idx)
{
    if (!_doc_registry)
        return;
    _pinned_docs.erase(
        std::remove_if(_pinned_docs.begin(), _pinned_docs.end(),
                       [&](const std::shared_ptr<data::SessionDocument> &p) {
                           return p && _doc_registry->index_of_document(
                                           p.get()) == idx;
                       }),
        _pinned_docs.end());
}

void TabContext::invalidate_device(ds_device_handle handle)
{
    // Unified device-identity invalidation: a file device was closed — drop
    // every trace of it here. A dead slot as the CURRENT binding must be
    // rebound by the caller (needs a fresh document + view detach).
    if (handle == NULL_HANDLE)
        return;
    if (_device_handle == handle)
        _device_handle = NULL_HANDLE;
    _pinned_docs.erase(
        std::remove_if(_pinned_docs.begin(), _pinned_docs.end(),
                       [handle](const std::shared_ptr<data::SessionDocument>
                                    &p) {
                           return p && p->is_file_device_slot() &&
                                  p->device_handle() == handle;
                       }),
        _pinned_docs.end());
}

// ---- Device-keyed data pool: tab ↔ slot rebinding (rebind model) ----

void TabContext::rebind_document(std::shared_ptr<data::SessionDocument> doc,
                                 size_t doc_index)
{
    // Unpin the target slot if it was pinned — it becomes the current binding.
    if (doc) {
        auto it = std::find_if(
            _pinned_docs.begin(), _pinned_docs.end(),
            [&doc](const std::shared_ptr<data::SessionDocument> &p) {
                return p.get() == doc.get();
            });
        if (it != _pinned_docs.end())
            _pinned_docs.erase(it);
    }
    // Pin the outgoing binding (unless trivially the same slot / none). The
    // outgoing document stays alive via the pinned strong reference.
    if (_doc_ref && _doc_ref.get() != doc.get() &&
        std::find_if(_pinned_docs.begin(), _pinned_docs.end(),
                     [this](const std::shared_ptr<data::SessionDocument> &p) {
                         return p.get() == _doc_ref.get();
                     }) == _pinned_docs.end()) {
        _pinned_docs.push_back(_doc_ref);
    }
    _doc_ref = std::move(doc);
    _document = _doc_ref.get();
    _doc_index = doc_index;
}

bool TabContext::owns_document(const data::SessionDocument *doc) const
{
    if (!doc)
        return false;
    if (doc == _document)
        return true;
    for (const auto &p : _pinned_docs) {
        if (p.get() == doc)
            return true;
    }
    return false;
}

void TabContext::make_live()
{
    _state = LIVE;
}

bool TabContext::has_data()
{
    return _document && _document->has_data();
}

void TabContext::activate()
{
    // Session-Centric 阶段8：bind(ctx) 语义链。
    // 本函数即"View 换绑到本 tab 的 SessionContext"的完整语义，五段依次
    // 执行（行为与重组前完全一致，仅显式化命名阶段）。阶段1 起 TabSwitch
    // 不再走全局摧毁管线，全程零全局副作用。
    pxv_info("TabContext::activate() bind(ctx) doc=%p handle=%llu",
             (void *)_document, (unsigned long long)_device_handle);
    restore_device_for_this_tab();   // 1) 恢复本 tab 设备
    claim_active_document();         // 2) 认领 active document
    _state = LIVE;
    apply_device_intent();           // 3) 应用设备意图（配置→模型→布局）
    restore_view_data();             // 4) 数据绑定裁决
    finalize_view();                 // 5) 视图收尾
}

// ---- bind(ctx) 语义链实现（自 activate() 逐段迁移） ----

// 1) Restore this tab's device BEFORE any config/data work below.
//
// DeviceAgent holds exactly ONE active device, shared by every tab. A tab
// that opened a file (.pxl, or an imported VCD/CSV/...) owns a virtual
// device; tabbing away switches the global device to the other tab's.
// apply_signal_config() / reload() further down both read and write the
// CURRENT device's sr_channels, so running them against a foreign device
// writes this tab's channel config onto the wrong channels and reads back
// the wrong channel count — the "tabbing back shows 2 channels with
// default names" bug.
void TabContext::restore_device_for_this_tab()
{
    if (_device_handle != NULL_HANDLE && !_session->is_working()) {
        if (_session->get_device()->handle() != _device_handle) {
            pxv_info("TabContext::activate() restoring device handle %llu for this tab",
                     (unsigned long long)_device_handle);
            if (!_session->set_device(_device_handle,
                                      interface::DeviceChangeReason::TabSwitch)) {
                // Device is gone (e.g. closed from the device list). Drop the
                // stale handle so we stop trying to restore it.
                pxv_warn("TabContext::activate() failed to restore device handle %llu",
                         (unsigned long long)_device_handle);
                _device_handle = NULL_HANDLE;
            }
        }
    }
}

// 2) R6: 工作中（采集/copy 进行中）跳过 set_active_document，避免覆盖
// capture_owner_document 导致数据归属错乱。END_COLLECT_WORK 时由
// MainWindow 显式调用 set_active_document 恢复当前 tab 归属。
void TabContext::claim_active_document()
{
    if (!_session->is_working()) {
        _session->set_active_document(_document);
    }
}

// 4) 数据绑定裁决：文档有数据→绑文档（唯一真相）；会话有数据且归属匹配
// （采集/拷贝/停止窗口）→绑会话实时缓冲；否则清空绑定。
void TabContext::restore_view_data()
{
    if (_document && _document->has_data()) {
        _view->set_data_document(_document);
        auto &sigs = _view->get_own_signals();
        for (auto &sig : sigs) {
            auto s = sig.get();
            if (s && s->model()) {
                // Signal::set_enabled() already writes back to SignalModel and sr_channel
                s->model()->set_enabled(s->enabled());
            }
        }
        // 修复（切回旧 tab 波形消失）：文档有历史数据且未在采集时，设备 status
        // 仍停留在 set_device 设置的 ST_INIT，doPaint 会走 paintCursors 分支而
        // 不调用 paintSignals，波形不渲染。显式恢复 ST_STOPPED，让视图绘制
        // 已绑定的文档快照。
        if (!_session->is_working()) {
            _session->set_stopped_status();
            // 阶段3a：per-tab 状态机同步——本 ctx 数据完整（有历史数据且未在
            // 采集），表达"可显示"语义，供 View 层逐步替代全局 ST_* 判断。
            _document->set_state(data::SessionDocument::SessionState::Stopped);
        }
    } else if (_session->have_view_data() &&
               (_session->is_working() || _session->is_copy_in_progress() ||
                _session->is_stopped_status()) &&
               (!_session->get_capture_owner_document() ||
                _session->get_capture_owner_document() == _document)) {
        // Document has no data yet, but session has data.
        // Bind signals to session data instead of clearing them.
        //
        // This covers three scenarios:
        // 1. Active capture (is_working) — waveforms update in real-time.
        // 2. Background copy (is_copy_in_progress) — data is being copied
        //    to the document; show session data in the meantime.
        // 3. Post-capture gap (is_stopped_status) — e.g., after VCD import
        //    where SR_DF_END has been received (device ST_STOPPED) and
        //    capture_ended() populated the snapshot, but the async
        //    RevEndPacket event hasn't been processed yet (so
        //    is_copy_in_progress is still false). Without this branch,
        //    clear_signal_data() would null out all signal data pointers,
        //    leaving the viewport blank until RevEndPacket fires and
        //    re-attaches the data.
        //
        // The capture owner check is relaxed to also match when no
        // capture owner is set (nullptr) — this happens for VCD imports
        // which don't call start_capture(), so the capture owner is
        // never assigned. When the owner IS set, it must match _document
        // to avoid binding another tab's data to the wrong view.
        _view->set_signal_data_from_source(_session);
    } else {
        pxv_info("TabContext::activate() no data, clearing signal data bindings");
        _view->clear_signal_data();
    }
}

// 5) 视图收尾：缩放/偏移更新 + 布局刷新通知。
void TabContext::finalize_view()
{
    _view->update_scale_offset();
    _view->signals_changed(nullptr);
}

// 架构重构 Phase 3：设备意图协议 —— 应用阶段。
// 把本标签页持久化的设备/通道意图（_document 的 SignalConfigStore）应用回
// 全局设备与 Core 模型：apply_signal_config 写设备（CHANNEL_MODE、通道启用/
// 命名等）→ reload 重建 SignalModel → 恢复 trig_type → 广播 GUI 刷新。
// 会话工作中（采集/拷贝）时改为暂存 pending config，待工作结束再应用。
void TabContext::apply_device_intent()
{
    if (!_document || !_document->has_signal_config())
        return;

    if (!_session->is_working()) {
        pxv_info("TabContext::apply_device_intent() work_mode=%d ch_count=%d",
            _document->get_signal_config().work_mode,
            (int)_document->get_signal_config().channels.size());
        _document->apply_signal_config();
        // 阶段11：同设备上下文（本 tab 设备恢复成功，handle 有效）时，
        // SignalModel 列表从文档 stash 原位恢复——零重建，R2 的 trig_type
        // 回填也不再需要（状态就在模型对象上）。设备已失效（restore 失败
        // 置 NULL_HANDLE，如文件设备被关闭）或首开无 stash 时，走原 reload
        // 重建路径。
        if (_device_handle != NULL_HANDLE &&
            _session->restore_signal_models_from(_document)) {
            // stash 期间模型脱离全局执行缓冲：重新绑定当前 view_data 快照
            // （与原 reload 后状态等价——解码/测量数据源恢复）。
            // Rebind model 例外：文件设备池槽的 stash 模型自带本槽快照
            // （VCD/pxl 数据），绝不能绑全局执行缓冲（那是别的设备的数据）。
            if (!_document->is_file_device_slot())
                _session->attach_data_to_current_view_buffer();
            // R3 演进：意图应用路径已显式恢复模型，skip_model_reload 置位
            // 让 GUI 消费方照常刷新但不触发二次全量重建。
            _session->broadcast_async<interface::DeviceOptionsUpdated>({true});
        } else {
            _session->reload();
            // R2: reload 重建 SignalModel 后，从 _signal_config 恢复 trig_type。
            // reload 内部虽从 old_model 保留 trig_type (sigsession.cpp:1141)，
            // 但 old_model 是上一个 tab 的，需覆盖为当前 tab 的配置。
            for (const auto &ch : _document->get_channels()) {
                auto m = _session->get_signal_by_index(ch.index);
                if (m)
                    m->set_trig_type(ch.trig_type);
            }
            // R3: 通道配置已修改 Core (probe->enabled 等)，广播通知其他 GUI
            // 组件刷新。MainWindow::on_event 会调 rebuild_signals 重建 view::Signal，
            // SigSession::on_event 会调 reload (二次 reload 从 old_model 保留 trig_type，
            // 不丢失)。tab 切换低频，二次重建开销可接受。
            // 演进（事件瀑布收敛）：意图应用路径已显式 reload()，skip_model_reload
            // 置位让 SigSession 跳过其订阅 handler 中的二次全量重建；GUI 消费方
            // （通道名/布局刷新等）不受影响，照常执行。
            _session->broadcast_async<interface::DeviceOptionsUpdated>({true});
        }
    } else {
        pxv_info("TabContext::apply_device_intent() session working, "
                 "saving pending config");
        _document->set_pending_config(_document->get_signal_config());
    }
    _view->rebuild_signals_from_config(_document->get_signal_config());
    pxv_info("TabContext::apply_device_intent() rebuild done, own_signals=%d",
        (int)_view->get_own_signals().size());
}

void TabContext::deactivate()
{
    pxv_info("TabContext::deactivate() doc=%p", _document);
    // 架构重构 Phase 3：设备意图协议 —— 收割阶段（详见 harvest_device_state）。
    harvest_device_state();
    _state = HISTORICAL;
}

// 架构重构 Phase 3：设备意图协议 —— 收割阶段。
// 从 View 层收集通道 UI 布局（view_index/v_offset/own_height，标签页布局的
// 单一真相来源；Task 7: visible 已不再是 Core 序列化字段），连同当前
// SignalModel 通道状态（enabled/名称/vfactor/trig_type 等）回写本标签页的
// _document 意图存储。切走后全局设备可被其他标签页自由切换，本标签页状态
// 完整保存于文档中，activate() 时经 apply_device_intent() 原样恢复。
void TabContext::harvest_device_state()
{
    if (!_document)
        return;

    std::map<int, data::ChannelLayoutState> channel_layout;
    if (_view) {
        for (auto &sig : _view->get_own_signals()) {
            data::ChannelLayoutState layout;
            layout.view_index = sig->get_view_index();
            layout.v_offset = sig->get_v_offset();
            layout.own_height = sig->get_own_height();
            channel_layout[sig->get_index()] = layout;
        }
    }
    // R2: 传入 SignalModel 列表，保存 Logic 通道 trig_type
    // UI 布局状态经 channel_layout 持久化到 ChannelConfig
    _document->save_signal_config(_session->get_signal_models_snapshot(),
                                  channel_layout);
    // 阶段11：模型 stash——本 tab 的 SignalModel 列表移入文档暂存，
    // activate 时由 apply_device_intent 原位恢复（零重建）。模型对象跨
    // tab 保活，enabled/名称/trig_type 等状态天然随行（R2 恢复逻辑的
    // 更优替代——状态就在对象上，无需从 config 回填）。
    _session->stash_signal_models_to(_document);
}

} // namespace pv
