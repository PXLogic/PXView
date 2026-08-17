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
}

TabContext::~TabContext()
{
    // phase 2: document ownership is held by DocumentRegistry. Release the
    // slot (marked deletion — frees the document, keeps index stable) instead
    // of delete. Safe to call with SIZE_MAX / nullptr registry (no-op).
    if (_doc_registry && _doc_index != SIZE_MAX)
        _doc_registry->release_document(_doc_index);
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
    fprintf(stderr, "DBG TabContext::activate() doc=%p has_config=%d has_data=%d is_working=%d\n",
        _document,
        _document ? _document->has_signal_config() : 0,
        _document ? _document->has_data() : 0,
        _session->is_working());
    fflush(stderr);

    // R6: 工作中（采集/copy 进行中）跳过 set_active_document，避免覆盖
    // capture_owner_document 导致数据归属错乱。END_COLLECT_WORK 时由
    // MainWindow 显式调用 set_active_document 恢复当前 tab 归属。
    if (!_session->is_working()) {
        _session->set_active_document(_document);
    }
    _state = LIVE;
    if (_document && _document->has_signal_config()) {
        if (!_session->is_working()) {
            pxv_info("TabContext::activate() applying signal config, work_mode=%d ch_count=%d",
                _document->get_signal_config().work_mode,
                (int)_document->get_signal_config().channels.size());
            _document->apply_signal_config();
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
            _session->broadcast_async<interface::DeviceOptionsUpdated>({});
        } else {
            pxv_info("TabContext::activate() session working, saving pending config");
            _document->set_pending_config(_document->get_signal_config());
        }
        _view->rebuild_signals_from_config(_document->get_signal_config());
        pxv_info("TabContext::activate() rebuild_signals_from_config done, own_signals=%d",
            (int)_view->get_own_signals().size());
    }
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
            pxv_info("TabContext::activate() restored STOPPED status for data doc");
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
    _view->update_scale_offset();
    _view->signals_changed(nullptr);
    pxv_info("TabContext::activate() completed, signals_with_data=%d",
        _view->data_sync_delegate()->count_signals_with_data());
}

void TabContext::deactivate()
{
    pxv_info("TabContext::deactivate() doc=%p", _document);
    if (_document) {
        // Collect UI layout state from the View layer so that
        // save_signal_config can persist view_index/v_offset/own_height as the
        // single source of truth for channel layout. (Task 7: visible is no
        // longer a Core-serialized field; it will be owned by View-layer
        // DockUiState in a later task.)
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
    }
    _state = HISTORICAL;
}

} // namespace pv
