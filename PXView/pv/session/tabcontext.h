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
    //
    // view 允许为 nullptr（QML/headless tab 路径，QML 迁移 Phase 2.1 起支持，
    // 经 SessionManager::create_context(nullptr, ...) 创建）。所有解引用
    // _view 的方法均已守护：restore_view_data()/finalize_view() 直接跳过，
    // apply_device_intent() 只跳过尾部 View 信号重建（Core 侧意图应用照常），
    // harvest_device_state() 原有 if (_view) 守护。Widgets 路径仍传真 View，
    // 行为不变。
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

    // 数据模型重构步骤6：pin/invalidate 机制已删除。所有权不可变（文档从不
    // 在 tab 之间流动），因此不需要"钉住旧槽""按设备失效清除身份"——借用只
    // 是 _borrow_doc，可被 release_borrow()/属主关闭显式解除。
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
    // 数据模型重构步骤6：owns_document 已删除（无 pin，无多槽判定）。
    // 数据模型重构步骤6：pinned_docs 已删除（见 rebind 注释）。

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
    inline size_t doc_index() const { return _doc_index; }
    // 数据模型重构步骤6：unpin_document/invalidate_device 已删除（所有权
    // 不可变，无需清除设备身份——借用由 release_borrow() 解除）。

    // --- 渲染借用（数据模型重构步骤6：所有权不可变 + 渲染主体可借用）---
    // 借用 = 本 tab 临时渲染【另一个文档】（典型：已打开文件设备的池槽），
    // 仅改变"画什么"，绝不改变数据所有权：_document/_device_handle 保持原值
    // （创建时定死），因此不需要 rebind/pin/invalidate/幸存者重绑等任何
    // 所有权搬运机制。借用同时记录该文档对应设备，供 activate 恢复设备用。
    // 生命周期遵循"所有权 = 寿命"：属主 tab 关闭 → 借用立即解除，本 tab 回落
    // 自己的数据（不延长被借用文档寿命）。
    void borrow_document(std::shared_ptr<data::SessionDocument> doc,
                         ds_device_handle handle);
    void release_borrow();
    bool is_borrowing() const { return _borrow_doc != nullptr; }
    // 当前渲染主体（借用 ? 借用的文档 : 本 tab 自己的文档）。
    data::SessionDocument *render_document() const {
        return _borrow_doc ? _borrow_doc.get() : _document;
    }
    // 渲染主体的强引用（供 SessionStateContext 以 weak_ptr 安全持有——
    // 渲染文档可随属主/借用解除而销毁，弱引用悬挂自动变空）。
    std::shared_ptr<data::SessionDocument> render_doc_shared() const {
        return _borrow_doc ? _borrow_doc : _doc_ref;
    }
    // 当前"表达的设备"：借用时是借用文档的设备，否则是本 tab 自己的设备。
    ds_device_handle effective_device_handle() const {
        return _borrow_device_handle != NULL_HANDLE ? _borrow_device_handle
                                                    : _device_handle;
    }
    inline ds_device_handle borrow_device_handle() const {
        return _borrow_device_handle;
    }
    // 借用来源的可读名（UI 角标用；未设置时返回通用文案）。
    inline QString borrow_label() const {
        return _borrow_label.isEmpty() ? QString("借用中") : _borrow_label;
    }
    inline void set_borrow_label(const QString &label) { _borrow_label = label; }

    // Harvest the tab's device intent (channel config + layout + model stash)
    // into the bound document. Public for the rebind model: the GUI calls it
    // in on_current_device_change_prev when the current tab LEAVES a file
    // device (same-tab leave), so the pool slot carries the final intent for
    // the zero-rebuild restore on switch-back.
    void harvest_device_state();

    void make_live();
    void activate();
    void deactivate();

    // 数据模型重构步骤7：archive_session_data_if_owned 已删除——数据落 doc
    // 由 on_rev_end_packet 拷贝路径唯一负责（数据代 Frozen → owner doc）。

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
    // 数据模型重构步骤6：_pinned_docs 已删除（无 pin 机制）。
    // Documents created for this tab (registry refs dropped on tab close).
    // Foreign shared slots are NOT in this list.
    std::vector<size_t>     _owned_doc_indices;
    // Strong reference to the current binding (keeps it alive across any
    // rebinding/owner-close race). _document is the raw mirror for compat.
    std::shared_ptr<data::SessionDocument> _doc_ref;
    // 渲染借用（所有权不可变）：借用中的文档 + 其设备 handle。仅影响
    // render_document()/effective_device_handle()，不影响所有权。
    std::shared_ptr<data::SessionDocument> _borrow_doc;
    ds_device_handle _borrow_device_handle = NULL_HANDLE;
    QString _borrow_label;
};

} // namespace pv

#endif
