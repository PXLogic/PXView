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

#ifndef PXVIEW_PV_TABCONTEXT_H
#define PXVIEW_PV_TABCONTEXT_H

#include <QString>
#include <QDateTime>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pv/base/pxvdef.h"   // ds_device_handle / NULL_HANDLE

namespace pv {

namespace view {
class View;
}

namespace data {
class SessionDocument;
}

namespace core {
class DocumentRegistry;
}

class SigSession;

class TabContext
{
public:
    enum State {
        LIVE,
        HISTORICAL
    };

    // modernize-core-layer-radical phase 2: TabContext now holds a WEAK
    // reference to the document (doc) plus its owning index and registry.
    // The document is owned by DocumentRegistry; TabContext::~TabContext
    // calls registry->release_document(doc_index) instead of delete.
    TabContext(view::View *view, SigSession *session, data::SessionDocument *doc,
               size_t doc_index, core::DocumentRegistry *registry);
    ~TabContext();

    inline view::View* view() { return _view; }
    inline void set_view(view::View *v) { _view = v; }
    inline data::SessionDocument* document() { return _document; }
    inline SigSession* session() { return _session; }
    inline QString title() const { return _title; }
    inline QString file_path() const { return _file_path; }
    inline State state() const { return _state; }
    inline bool is_live() const { return _state == LIVE; }
    bool has_data();
    inline QDateTime timestamp() const { return _timestamp; }

    inline void set_title(const QString &title) { _title = title; }
    inline void set_file_path(const QString &path) { _file_path = path; }

    // The device this tab's data came from. NULL_HANDLE for tabs that have
    // never been bound to a device (e.g. a fresh empty tab before any capture).
    //
    // File tabs (.pxl / imported VCD/CSV/...) MUST remember their virtual
    // device: the global DeviceAgent can only hold ONE active device, so
    // tabbing away switches it. Without this handle the tab can never get its
    // device back — its sr_channels (channel names / enabled / types) are gone
    // and the tab silently degrades to whatever device happens to be active
    // (typically Demo).
    inline ds_device_handle device_handle() const { return _device_handle; }
    inline void set_device_handle(ds_device_handle h) { _device_handle = h; }

    // --- Device-keyed data pool: tab ↔ slot rebinding (rebind model v3) ---
    // Rebind this tab to a different data slot document (strong reference —
    // the tab keeps the slot alive for as long as it binds/pins it). The
    // previously bound document is NOT released — it stays alive (registry
    // ref + this tab's pinned ref) as a file-device pool slot whose snapshots
    // + decoder stacks survive the switch; switching back rebinds to it with
    // zero data loss. If the new document is one of the pinned slots, it is
    // unpinned (it becomes the current binding again).
    void rebind_document(std::shared_ptr<data::SessionDocument> doc,
                         size_t doc_index);
    // True if doc is this tab's current binding or one of its pinned slots.
    bool owns_document(const data::SessionDocument *doc) const;
    // Pinned slot documents (strong refs). Dropped when the tab closes or
    // the slot's device is invalidated; registry refs die with the owner.
    inline const std::vector<std::shared_ptr<data::SessionDocument>> &
    pinned_docs() const {
        return _pinned_docs;
    }

    // --- Shared-reference semantics (rebind model v2/v3) ---
    // A file-device pool slot can be bound by SEVERAL tabs: the tab that
    // opened the file (owner) and any tab that switched its data to that file
    // (borrower). Ownership bookkeeping decides who drops the REGISTRY ref:
    // only documents CREATED for this tab are released in its destructor;
    // foreign slots die with their owning tab/device (close_file). Strong
    // references make any ordering safe — a doc lives while anyone binds it.
    void mark_document_owned(size_t idx);
    bool owns_doc_index(size_t idx) const;
    inline const std::vector<size_t> &owned_doc_indices() const {
        return _owned_doc_indices;
    }
    // Drop a pinned entry by registry index (the doc itself is untouched —
    // only this tab's reference goes away).
    void unpin_document(size_t idx);
    inline size_t doc_index() const { return _doc_index; }
    // Unified device-identity invalidation (rebind model v3): drop every
    // trace of a closed file device — the tab's device_handle and any pinned
    // slot belonging to that device. A dead slot as the CURRENT binding must
    // be rebound by the caller (needs a fresh document + view detach).
    void invalidate_device(ds_device_handle handle);

    // Harvest the tab's device intent (channel config + layout + model stash)
    // into the bound document. Public for the rebind model: the GUI calls it
    // in on_current_device_change_prev when the current tab LEAVES a file
    // device (same-tab leave), so the pool slot carries the final intent for
    // the zero-rebuild restore on switch-back.
    void harvest_device_state();

    void make_live();
    void activate();
    void deactivate();

    // Data model refactor step 1 (per-doc data archive): if this tab is
    // switchING away while the global session buffer holds data produced
    // under THIS tab's device and the bound document does not yet have its
    // own copy, zero-copy share the buffer into the document. Every tab that
    // ever displayed a capture generation keeps it — switching back binds the
    // document instead of relying on the (transient) session buffer.
    // Guards: no file-device slots (their data comes from replay, never the
    // buffer), no archiving while a capture/copy is in flight (the
    // RevEndPacket owner-copy is the correct landing for those), and the
    // buffer is only attributed to this tab when the CURRENT device still
    // matches the tab's device identity (no device switch since the data
    // arrived).
    void archive_session_data_if_owned();

    static int _next_session_id;

private:
    // ---- Session-Centric 阶段8：bind(ctx) 语义链 ----
    // activate() = 依次执行以下阶段，即"View 换绑到本 tab 的 SessionContext"
    // 的完整语义（对应参考项目：PulseView 换窗 / Logic2 setActiveSession 换
    // 指针——换绑零全局副作用）：
    //   1) restore_device_for_this_tab()  恢复本 tab 绑定的设备（TabSwitch）
    //   2) claim_active_document()        认领 active document 归属
    //   3) apply_device_intent()          应用设备意图（配置→模型→布局）
    //   4) restore_view_data()            数据绑定裁决（文档/会话/清空）
    //   5) finalize_view()                视图收尾（缩放/布局通知）
    void restore_device_for_this_tab();
    void claim_active_document();
    void restore_view_data();
    void finalize_view();

    // ---- 设备意图协议（架构重构 Phase 3）----
    // 本标签页的设备/通道状态（意图）持久化于 _document 的 SignalConfigStore。
    //   deactivate() → harvest_device_state()：从 View/SignalModel 收割状态回写意图；
    //   activate()   → apply_device_intent()（bind 链第 3 段）：把意图应用回全局设备与 Core 模型。
    // 全局 DeviceAgent 只持有一个活动设备，标签页切换 = 意图的收割/应用轮转。
    void apply_device_intent();

    view::View              *_view;
    SigSession              *_session;
    data::SessionDocument   *_document;   // weak reference (owned by DocumentRegistry)
    size_t                  _doc_index;   // owning index in DocumentRegistry
    core::DocumentRegistry  *_doc_registry; // owner of the document
    QString                 _title;
    QString                 _file_path;
    State                   _state;
    QDateTime               _timestamp;
    ds_device_handle        _device_handle = NULL_HANDLE;
    // Device-keyed data pool: slots pinned by this tab after rebinding away
    // from them (file-device pool entries whose data must survive switches).
    // Strong references — the tab keeps them alive while it may return to it.
    std::vector<std::shared_ptr<data::SessionDocument>> _pinned_docs;
    // Documents created for this tab (registry refs dropped on tab close).
    // Foreign shared slots are NOT in this list.
    std::vector<size_t>     _owned_doc_indices;
    // Strong reference to the current binding (keeps it alive across any
    // rebinding/owner-close race). _document is the raw mirror for compat.
    std::shared_ptr<data::SessionDocument> _doc_ref;
};

} // namespace pv

#endif
