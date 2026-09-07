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
    _borrow_doc.reset();   // 借用随 tab 死亡解除（不延长被借用文档寿命）
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

// 数据模型重构步骤6：unpin_document / invalidate_device / owns_document 已
// 删除——所有权不可变，文档不在 tab 间流动，借用由 release_borrow() 解除。

// ---- 文档换绑（仅用于"本 tab 自己的文档被销毁/失效"的重建路径）----

void TabContext::rebind_document(std::shared_ptr<data::SessionDocument> doc,
                                 size_t doc_index)
{
    // 不再 pin 旧绑定：本 tab 始终只有一个自己的文档，旧文档要么随 tab 死
    // 亡（release_document），要么由 registry 强引用保活。
    _doc_ref = std::move(doc);
    _document = _doc_ref.get();
    _doc_index = doc_index;
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
    pxv_info("TabContext::activate() bind(ctx) doc=%p handle=%llu%s",
             (void *)_document, (unsigned long long)_device_handle,
             is_borrowing() ? " (borrowing)" : "");
    restore_device_for_this_tab();   // 1) 恢复本 tab 表达的设备（含借用）
    claim_active_document();         // 2) 认领 active document（所有权）+
                                     //    渲染文档（借用时=借用文档）
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
    // 数据模型重构步骤6：借用时表达的是【借用文档的设备】，但本 tab 的身份
    // （_device_handle）不变——恢复失败只解除借用，绝不篡改所有权身份。
    ds_device_handle target = effective_device_handle();
    if (target != NULL_HANDLE && !_session->is_working()) {
        if (_session->get_device()->handle() != target) {
            pxv_info("TabContext::activate() restoring device handle %llu for this tab",
                     (unsigned long long)target);
            if (!_session->set_device(target,
                                      interface::DeviceChangeReason::TabSwitch)) {
                // Device is gone (e.g. closed from the device list). Drop the
                // stale handle so we stop trying to restore it.
                pxv_warn("TabContext::activate() failed to restore device handle %llu",
                         (unsigned long long)target);
                if (is_borrowing() && _borrow_device_handle == target) {
                    // 借用的设备没了（其属主 tab 关闭/文件设备注销）→ 解除
                    // 借用，本 tab 回落自己的数据。
                    pxv_warn("TabContext: borrowed device gone, releasing borrow");
                    release_borrow();
                } else {
                    _device_handle = NULL_HANDLE;
                }
            }
        }
    }
}

// 2) R6: 工作中（采集/copy 进行中）跳过 set_active_document，避免覆盖
// capture_owner_document 导致数据归属错乱。END_COLLECT_WORK 时由
// MainWindow 显式调用 set_active_document 恢复当前 tab 归属。
// 数据模型重构步骤6：active document 表达【所有权】（采集数据落在自己的
// 文档，绝不能落进借用的文件池槽）；渲染文档表达"画什么"，供模型访问器
// 解析（见 SessionStateContext::active_document_models）。
void TabContext::claim_active_document()
{
    if (!_session->is_working()) {
        // 借用态下"当前正在操作的会话"就是被借用的文件池槽（渲染/模型/采集
        // 归属三者一致，例如借用 pxl 时按运行 = 重放该文件、数据落回该池
        // 槽）；本 tab 自己的文档仍然完整保留，解除借用即回到它。
        data::SessionDocument *rd = render_document();
        _session->set_active_document(rd);
        _session->set_render_document(rd);
    }
}

// 4) 数据绑定裁决（数据模型重构步骤4 收敛）：文档有数据→绑文档（唯一
// 真相）；会话有【实时】数据且归属匹配（采集中/拷贝窗口）→绑会话缓冲；
// 否则清空绑定。停止态的历史数据一律走文档分支——步骤1 已保证"展示过的
// 数据切走前归档进文档"，停止态借用分支已删除。
void TabContext::restore_view_data()
{
    // 数据模型重构步骤6：绑定【渲染文档】（借用时=借用的文件池槽）。
    data::SessionDocument *rd = render_document();
    if (rd && rd->has_data()) {
        _view->set_data_document(rd);
        auto &sigs = _view->get_own_signals();
        for (auto &sig : sigs) {
            auto s = sig.get();
            if (s && s->model()) {
                // Signal::set_enabled() already writes back to SignalModel and sr_channel
                s->model()->set_enabled(s->enabled());
            }
        }
        // 文档→显示层状态同步：本 ctx 数据完整且未在采集 = "可显示"。
        // 显式置 ST_STOPPED 让 View 绘制管线（viewport_painter 等仍读全局
        // is_stopped_status 的 28 个读取点）走 paintSignals 分支绘制文档
        // 快照。步骤4 已保证 TabSwitch 不再把 ST_STOPPED 打回 ST_INIT，
        // 此处是首绑/导入（VCD import 结束、RevEndPacket 前的窗口）等路径
        // 的兜底同步点；待 View 全面读 per-doc SessionState 后移除。
        if (!_session->is_working()) {
            _session->set_stopped_status();
            // 阶段3a：per-tab 状态机同步（渲染文档 = 被画的那份数据）。
            rd->set_state(data::SessionDocument::SessionState::Stopped);
        }
    } else if (_session->have_view_data() &&
               (_session->is_working() || _session->is_copy_in_progress()) &&
               (!_session->get_capture_owner_document() ||
                _session->get_capture_owner_document() == _document) &&
               // 数据模型重构步骤1：借用加身份门槛 —— 本 tab 无设备身份，
               // 或全局当前设备就是本 tab 的设备。防止设备切换后缓冲残留
               // 上一设备的数据被借给本 tab。
               (_device_handle == NULL_HANDLE ||
                _session->get_device()->handle() == _device_handle)) {
        // Document has no data yet, but the session has LIVE data.
        // Bind signals to session data instead of clearing them.
        //
        // This covers two scenarios (the old third — post-capture/stopped
        // gap — was removed in step 4: stopped historical data always binds
        // via the document branch, since step 1 archives displayed data
        // into the tab's document on switch-away):
        // 1. Active capture (is_working) — waveforms update in real-time.
        // 2. Background copy (is_copy_in_progress) — data is being copied
        //    to the document; show session data in the meantime.
        //
        // The capture owner check is relaxed to also match when no
        // capture owner is set (nullptr) — this happens for VCD imports
        // which don't call start_capture(), so the capture owner is
        // never assigned. When the owner IS set, it must match _document
        // to avoid binding another tab's data to the wrong view.
        _view->set_signal_data_from_source(_session);
        // 拷贝窗口的借用立即归档进文档（采集在途时归档由 RevEndPacket 的
        // owner-copy 负责）。
        if (!_session->is_working())
            archive_session_data_if_owned();
    } else {
        pxv_info("TabContext::activate() no data, clearing signal data bindings");
        _view->clear_signal_data();
        // 步骤4 对称面：无任何可显示数据 → 显示层回到"待采集"（ST_INIT）
        // 语义。TabSwitch 不再无条件复位 ST_INIT 后，由这里显式维护；采集
        // 进行中（借用失败但状态是 RUNNING）绝不能扰动显示状态。
        if (!_session->is_working())
            _session->set_init_status();
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
// 命名等）→ 广播 GUI 刷新。
// 数据模型重构步骤2：模型列表已归本 tab 文档所有，且 activate 第 2 段
// （claim_active_document）已把本文档设为活动文档——全局 signal_models
// 访问器现在解析到的就是本文档的列表，"stash/restore 原位恢复"语义天然
// 成立，无需搬运。设备失效（restore 失败置 NULL_HANDLE，如文件设备被
// 关闭）时才走 reload 全量重建。
// 会话工作中（采集/拷贝）时改为暂存 pending config，待工作结束再应用。
void TabContext::apply_device_intent()
{
    // 数据模型重构步骤6：意图应用于【渲染文档】——借用时应用被借用文件池槽
    // 的通道配置与模型（画的是它的数据），本 tab 自己的文档不受影响。
    data::SessionDocument *rd = render_document();
    if (!rd || !rd->has_signal_config())
        return;

    if (!_session->is_working()) {
        pxv_info("TabContext::apply_device_intent() work_mode=%d ch_count=%d%s",
            rd->get_signal_config().work_mode,
            (int)rd->get_signal_config().channels.size(),
            is_borrowing() ? " (borrowed doc)" : "");
        rd->apply_signal_config();
        // 数据模型重构步骤2 修正：原位恢复的前提是【本文档真的持有模型】。
        // 新建 tab / 首次激活的文档列表为空（旧机制靠 stash 缺失回退到
        // reload 来构建），必须走 reload 构建，否则核心模型为 0 —— 采集被
        // capturemanager 判空拒绝、dock/表头无通道，只剩 View 按 config 造的
        // 临时信号（表象："新建标签一个通道也没有"）。
        if (effective_device_handle() != NULL_HANDLE &&
            !rd->signal_models().empty()) {
            // 模型对象随文档保活（零重建）。仅非文件设备池槽需要重绑当前
            // view_data 快照（解码/测量数据源恢复）——池槽的模型自带本槽
            // 快照（VCD/pxl 数据），绝不能绑全局执行缓冲（别的设备的数据）。
            if (!rd->is_file_device_slot())
                _session->attach_data_to_current_view_buffer();
            // R3 演进：skip_model_reload 置位让 GUI 消费方照常刷新但不触发
            // 二次全量重建。
            _session->broadcast_async<interface::DeviceOptionsUpdated>({true});
        } else {
            _session->reload();
            // R2: reload 重建 SignalModel 后，从 _signal_config 恢复 trig_type。
            // （转发语义下 reload 的 old_model 查找读到的就是本渲染文档旧列表，
            // 但设备已失效重建，仍以文档配置为准覆盖。）
            for (const auto &ch : rd->get_channels()) {
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
        rd->set_pending_config(rd->get_signal_config());
    }
    _view->rebuild_signals_from_config(rd->get_signal_config());
    pxv_info("TabContext::apply_device_intent() rebuild done, own_signals=%d",
        (int)_view->get_own_signals().size());
}

void TabContext::deactivate()
{
    pxv_info("TabContext::deactivate() doc=%p", _document);
    // 架构重构 Phase 3：设备意图协议 —— 收割阶段（详见 harvest_device_state）。
    harvest_device_state();
    // 数据模型重构步骤1（per-doc 数据存档）：切走前把"属于本 tab 设备、且
    // 文档尚未持有"的会话缓冲快照零拷贝共享进文档。此后本 tab 切回时
    // restore_view_data 走文档绑定分支，不再依赖（易被其他设备数据覆盖的）
    // 全局会话缓冲 —— demo tab 切到文件 tab 再切回不再空白。
    archive_session_data_if_owned();
    _state = HISTORICAL;
}

// --- 渲染借用（数据模型重构步骤6）---

void TabContext::borrow_document(std::shared_ptr<data::SessionDocument> doc,
                                 ds_device_handle handle)
{
    if (!doc || doc.get() == _document) {
        // 借用自己 = 解除借用（回到本 tab 数据）。
        release_borrow();
        return;
    }
    _borrow_doc = std::move(doc);
    _borrow_device_handle = handle;
    pxv_info("TabContext: borrowing doc=%p (device %llu) for tab doc=%p",
             (void *)_borrow_doc.get(), (unsigned long long)handle,
             (void *)_document);
}

void TabContext::release_borrow()
{
    if (!_borrow_doc)
        return;
    pxv_info("TabContext: releasing borrow of doc=%p, back to own doc=%p",
             (void *)_borrow_doc.get(), (void *)_document);
    _borrow_doc.reset();
    _borrow_device_handle = NULL_HANDLE;
    _borrow_label.clear();
}

void TabContext::archive_session_data_if_owned()
{
    // 借用态下会话缓冲属于被借用的设备，本 tab 不归档（也不该污染它）。
    if (is_borrowing())
        return;
    if (!_document || _document->is_file_device_slot())
        return;   // 文件池槽的数据来自回放/导入，绝不接收会话缓冲
    if (_document->has_data())
        return;   // RevEndPacket 的 owner-copy 已归档（或本就是文件槽数据）
    if (_device_handle == NULL_HANDLE)
        return;   // 无设备身份，缓冲无法归属
    if (_session->is_working() || _session->is_copy_in_progress())
        return;   // 采集/拷贝在途 —— 等事件路径的 owner-copy，不抢半程数据
    if (!_session->have_view_data())
        return;   // 会话缓冲无数据
    // 归属裁决：仅当当前全局设备仍是本 tab 的设备（切走前未发生设备切换）
    // 时，缓冲数据才可能由本 tab 的设备产生。
    if (_session->get_device()->handle() != _device_handle)
        return;
    pxv_info("TabContext: archiving session buffer into doc=%p "
             "(device handle %llu, zero-copy)",
             (void *)_document, (unsigned long long)_device_handle);
    _session->copy_data_to_document(_document);
    _document->set_state(data::SessionDocument::SessionState::Stopped);
}

// 架构重构 Phase 3：设备意图协议 —— 收割阶段。
// 从 View 层收集通道 UI 布局（view_index/v_offset/own_height，标签页布局的
// 单一真相来源；Task 7: visible 已不再是 Core 序列化字段），连同当前
// SignalModel 通道状态（enabled/名称/vfactor/trig_type 等）回写本标签页的
// _document 意图存储。切走后全局设备可被其他标签页自由切换，本标签页状态
// 完整保存于文档中，activate() 时经 apply_device_intent() 原样恢复。
void TabContext::harvest_device_state()
{
    // 数据模型重构步骤6：借用态下当前设备/模型都属于被借用的文档，收割会把
    // 文件池槽的通道状态写进本 tab 文档（污染），且本 tab 自己的模型本来就
    // 完整保存在自己的文档上——直接跳过。
    if (!_document || is_borrowing())
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
    // 数据模型重构步骤2：不再 stash——模型列表本来就归本文档所有
    // （全局访问器转发到活动文档，本 tab 仍是活动文档），切走后对象随文档
    // 保活，activate 时经转发直接可见（零搬运、零空窗期）。
}

} // namespace pv
