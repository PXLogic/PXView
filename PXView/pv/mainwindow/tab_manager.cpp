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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "pv/mainwindow/tab_manager.h"

#include "pv/mainwindow/mainwindow.h"

#include <QVariant>
#include <QVBoxLayout>
#include <QObject>
#include <QDebug>
#include <QJsonObject>
#include <QTimer>
#include <cstdio>

#include "pv/core/documentregistry.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/session/deviceagent.h"
#include "pv/dock/dsotriggerdock.h"
#include "pv/dock/logdock.h"
#include "pv/dock/measuredock.h"
#include "pv/dock/protocoldock.h"
#include "pv/dock/searchdock.h"
#include "pv/dock/triggerdock.h"
#include "pv/dock/deviceoptionsdock.h"
#include "pv/base/log.h"
#include "pv/mainwindow/mainwindow.h"
#include "pv/mainwindow/dock_manager.h"
#include "pv/mainwindow/workspace_io.h"
#include "pv/session/sessionmanager.h"
#include "pv/session/sigsession.h"
#include "pv/session/storesession.h"
#include "pv/session/tabcontext.h"
#include "pv/toolbars/samplingbar.h"
#include "pv/ui/draggabletabwidget.h"
#include "pv/core/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/view/view.h"

namespace pv {

// 数据模型重构步骤5：新 tab 的默认采集设备 = 设备列表中第一个非文件/输入
// 模块设备（demo/硬件）。get_device_list 返回 calloc 数组（末尾 handle=0
// 哨兵），调用方负责 free。找不到（异常场景）返回 NULL_HANDLE。
static ds_device_handle default_capture_handle(SigSession *session)
{
  if (!session)
    return NULL_HANDLE;
  int count = 0, active = 0;
  struct ds_device_base_info *list = session->get_device_list(count, active);
  if (!list) {
    return NULL_HANDLE;
  }
  ds_device_handle found = NULL_HANDLE;
  for (int i = 0; i < count; ++i) {
    struct sr_dev_inst *sdi =
        session->get_device()->find_sdi_by_handle(list[i].handle);
    if (sdi && sr_dev_inst_driver_get(sdi) != nullptr) {
      found = list[i].handle;
      break;
    }
  }
  free(list);
  return found;
}

// ---------------------------------------------------------------------------
// per-tab 视图密度（随 tab 会话走，不进 .pxc）
// ---------------------------------------------------------------------------

// 视图密度是 per-tab 状态：.pxc 的自然键是 (driver, workMode)，同设备同模式的
// 多个 tab 会互相覆盖（见 AGENT_CONTRACTS.md "Workspace (tab session)
// persistence"）。这里是 ViewLayout 与 JSON 的唯一转换点 —— 刻意放在
// tab_manager 而不是 workspace_io，让后者保持零 View 依赖、可独立单测。
static void capture_session_ui_layout(pv::view::View *view,
                                      QJsonObject &session) {
  if (!view)
    return;
  QJsonObject ui = session.value("uiLayout").toObject();
  ui["signalHeightScale"] = view->layout_delegate()->signalHeightScale();
  session["uiLayout"] = ui;
}

static void apply_session_ui_layout(pv::view::View *view,
                                    const QJsonObject &session) {
  if (!view || !session.contains("uiLayout"))
    return;
  const QJsonObject ui = session.value("uiLayout").toObject();
  const int shs = ui.value("signalHeightScale").toInt(0);
  if (shs <= 0) {
    pxv_warn("apply_session_ui_layout: invalid signalHeightScale (%d), "
             "keeping current view density", shs);
    return;
  }
  // set_signalHeightScale() 会把该值标记为"显式设置" → 主题默认值不再覆盖
  // 这个恢复出来的 per-tab 密度。
  view->layout_delegate()->set_signalHeightScale(shs);
  view->layout_delegate()->set_signalHeight(shs);
  view->update_all_trace_postion();
  pxv_info("apply_session_ui_layout: restored signalHeightScale=%d", shs);
}

// ---------------------------------------------------------------------------
// TabManager — construction / destruction
// ---------------------------------------------------------------------------

TabManager::TabManager(MainWindow *wnd) : _wnd(wnd) {
  // workspace 的事件驱动写入（2s 防抖）。只在退出时写会在崩溃/强杀时丢掉整个
  // tab 结构；退出路径仍会立即写一次（MainWindow::able_to_close）。
  if (wnd) {
    _ws_save_timer = new QTimer(wnd);
    _ws_save_timer->setSingleShot(true);
    QObject::connect(_ws_save_timer, &QTimer::timeout, wnd,
                     [this]() { save_workspace(); });
  }
}

void TabManager::schedule_workspace_save() {
  if (_ws_save_timer && !_ws_save_timer->isActive())
    _ws_save_timer->start(2000);
}

TabManager::~TabManager() {
  // TabContexts are owned by SessionManager; we do not delete them here.
  // The DraggableTabWidget is a QObject child of MainWindow and will be
  // deleted by Qt's parent-child mechanism.
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void TabManager::create_tab_widget(QWidget *parent, QVBoxLayout *layout) {
  _tab_widget = new pv::ui::DraggableTabWidget(parent);
  layout->addWidget(_tab_widget);
}

void TabManager::init_initial_tab() {
  SigSession *_session = _wnd->session();
  toolbars::SamplingBar *_sampling_bar = _wnd->sampling_bar();

  pv::view::View *initial_view =
      new pv::view::View(_session, _sampling_bar, _wnd);
  // phase 2: document ownership moved into DocumentRegistry. take_document
  // returns a stable index; get_document_by_index yields a weak pointer.
  size_t initial_doc_idx = _session->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_session->device()));
  pv::data::SessionDocument *initial_doc =
      _session->document_registry()->get_document_by_index(initial_doc_idx);

  DeviceAgent *_device_agent = _wnd->device_agent();
  if (_device_agent && _device_agent->have_instance()) {
    initial_doc->save_signal_config(_session->get_signal_models(), {});
    pxv_info("MainWindow::setup_ui() saved initial signal config, mode=%d "
             "ch_count=%d",
             initial_doc->get_signal_config().work_mode,
             static_cast<int>(initial_doc->get_signal_config().channels.size()));
  }

  pv::TabContext *initial_ctx = SessionManager::instance()->create_context(
      initial_view, _session, initial_doc, initial_doc_idx,
      _session->document_registry());
  initial_ctx->set_title(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_FILE), "File"));

  // 注意：初始 demo 标签的设备 handle 不在此处设置。init_initial_tab() 由
  // setup_ui() 在 set_default_device() 之前调用，此时 _device_agent->handle()
  // 尚未就绪（可能是残留值），会错误地把非 demo 设备 handle 记到 demo 标签
  // 上。handle 的设置移到 on_load_device_first() 的 set_default_device()
  // 之后（见 mainwindow.cpp），那里当前设备即 demo，handle 才正确。

  _tab_contexts.append(initial_ctx);
  qDebug() << "TabManager::init_initial_tab() before addTab, initial_doc="
           << initial_doc
           << "has_config=" << initial_doc->has_signal_config();
  pxv_info("DBG before addTab has_config=%d", initial_doc->has_signal_config());
  _tab_widget->addTab(initial_view, initial_ctx->title());
  pxv_info("DBG after addTab");
  fprintf(stderr, "DBG TabManager::init_initial_tab() after addTab\n");
  fflush(stderr);
  _current_tab_index = 0;

  initial_ctx->activate();
}

void TabManager::setup_connections() {
  // Primary signal/slot connections
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::currentChanged, _wnd,
          &MainWindow::on_tab_changed);
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabMoved, _wnd,
          &MainWindow::on_tab_moved);
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabDetached, _wnd,
          &MainWindow::on_tab_detach);
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabAttached, _wnd,
          &MainWindow::on_tab_attached);
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::newTabRequested, _wnd,
          &MainWindow::on_new_tab_requested);
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabCloseRequested, _wnd,
          &MainWindow::remove_tab);
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabCloseOthersRequested, _wnd,
          [this](int index) { on_close_others_requested(index); });
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabCloseRightRequested, _wnd,
          [this](int index) { on_close_right_requested(index); });

  // Tab renamed — inline lambda
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabRenamed, _wnd,
          [this](int index, const QString &title) {
            on_tab_renamed(index, title);
          });

  // Extended tab attach handler (handles detached-window reattach + new-view
  // creation)
  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::tabAttached, _wnd,
          [this](QWidget *widget, const QString &title) {
            on_tab_attached_extended(widget, title);
          });
}

void TabManager::close_detached_windows() {
  if (_tab_widget)
    _tab_widget->closeAllDetachedWindows();
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

pv::view::View *TabManager::current_view() {
  if (_current_tab_index >= 0 && _current_tab_index < _tab_contexts.size()) {
    return _tab_contexts[_current_tab_index]->view();
  }
  return nullptr;
}

pv::TabContext *TabManager::current_context() {
  if (_current_tab_index >= 0 && _current_tab_index < _tab_contexts.size()) {
    return _tab_contexts[_current_tab_index];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Dock binding helpers
// ---------------------------------------------------------------------------

void TabManager::bind_docks(pv::TabContext *ctx) {
  _wnd->sampling_bar()->bind_context(ctx);
  _wnd->dock_manager()->measure_widget()->bind_context(ctx);
  _wnd->dock_manager()->search_widget()->bind_context(ctx);
  _wnd->dock_manager()->protocol_widget()->bind_context(ctx);
  _wnd->dock_manager()->device_options_widget()->bind_context(ctx);
  _wnd->dock_manager()->log_widget()->bind_context(ctx);
  _wnd->dock_manager()->trigger_widget()->bind_context(ctx);
  _wnd->dock_manager()->dso_trigger_widget()->bind_context(ctx);
}

void TabManager::unbind_docks() {
  _wnd->sampling_bar()->unbind_context();
  _wnd->dock_manager()->measure_widget()->unbind_context();
  _wnd->dock_manager()->search_widget()->unbind_context();
  _wnd->dock_manager()->protocol_widget()->unbind_context();
  _wnd->dock_manager()->device_options_widget()->unbind_context();
  _wnd->dock_manager()->log_widget()->unbind_context();
  _wnd->dock_manager()->trigger_widget()->unbind_context();
  _wnd->dock_manager()->dso_trigger_widget()->unbind_context();
}

void TabManager::set_view_on_docks(pv::view::View *view) {
  _wnd->sampling_bar()->set_context(_wnd->session(), view);
  _wnd->sampling_bar()->set_readonly(false);
  _wnd->sampling_bar()->set_view(view);
  _wnd->dock_manager()->measure_widget()->set_view(view);
  _wnd->dock_manager()->search_widget()->set_view(view);
  _wnd->dock_manager()->protocol_widget()->set_view(view);
  view->installEventFilter(_wnd);
}

// ---------------------------------------------------------------------------
// Tab operations
// ---------------------------------------------------------------------------

void TabManager::add_tab(pv::TabContext *ctx) {
  pv::view::View *view = ctx->view();
  _tab_contexts.append(ctx);
  _tab_widget->addTab(view, ctx->title());
  _tab_widget->setCurrentIndex(_tab_widget->count() - 1);
  update_tab_style(_tab_widget->count() - 1);
}

void TabManager::add_tab_silent(pv::TabContext *ctx) {
  // 恢复 workspace 时使用：只登记，不切当前页。add_tab() 会
  // setCurrentIndex()，逐个添加会反复激活（activate() 切设备 / 重放数据），
  // 而且最终停在最后一个 tab —— 恢复必须由调用方在末尾统一 setCurrentIndex。
  if (!ctx)
    return;
  _tab_contexts.append(ctx);
  _tab_widget->addTab(ctx->view(), ctx->title());
  update_tab_style(_tab_widget->count() - 1);
}

void TabManager::remove_tab(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;

  if (_tab_contexts.size() <= 1)
    return;

  SigSession *_session = _wnd->session();

  pv::TabContext *ctx = _tab_contexts[index];
  // 数据模型重构步骤6 修正：removeAt(index) 之后 _tab_contexts 会移位，
  // _current_tab_index 语义失效——"关闭前哪个 tab 是当前 tab"必须现在捕获，
  // 供后面的借用回落判断使用。
  pv::TabContext *was_current = current_context();
  // Rebind model v3: every document this tab OWNS dies with it (registry
  // ref dropped in ~TabContext) — its binding state (current/pinned) no
  // longer matters with strong references. Foreign pool slots shared from
  // other tabs survive. Owned file devices are closed below; handles are
  // cached up-front since ctx is destroyed further down.
  std::vector<ds_device_handle> owned_file_handles;
  std::vector<size_t> dying_docs = ctx->owned_doc_indices();
  for (size_t idx : dying_docs) {
    if (auto *d = _session->document_registry()->get_document_by_index(idx)) {
      if (d->is_file_device_slot() && d->device_handle() != NULL_HANDLE &&
          std::find(owned_file_handles.begin(), owned_file_handles.end(),
                    d->device_handle()) == owned_file_handles.end())
        owned_file_handles.push_back(d->device_handle());
    }
  }
  const bool owns_file_devices = !owned_file_handles.empty();
  if (ctx->is_live() && _session->is_working()) {
    _session->stop_capture();
  }

  // Active-document cleanup: clear it if it is any doc dying with this tab
  // (subsumes the old == ctx->document() check; a foreign shared doc that
  // survives keeps the active binding — the surviving foreground tab's
  // activate() re-claims it).
  if (auto *ad = _session->get_active_document()) {
    size_t ad_idx = _session->document_registry()->index_of_document(ad);
    if (std::find(dying_docs.begin(), dying_docs.end(), ad_idx) !=
        dying_docs.end())
      _session->set_active_document(nullptr);
  }

  _tab_contexts.removeAt(index);
  QObject::disconnect(_tab_widget, &pv::ui::DraggableTabWidget::currentChanged, _wnd,
             &MainWindow::on_tab_changed);
  _tab_widget->removeTab(index);
  // Task 4.3: capture owner cleanup is now RAII-managed by CaptureOwnerGuard.
  for (size_t di : dying_docs)
    _session->clear_capture_owner_document(
        _session->document_registry()->get_document_by_index(di));

  // A2 fix: stop decoder threads working on the dying documents' stacks
  // before they are destroyed (covers the current doc and all owned pinned
  // slots; foreign shared slots survive and keep running).
  for (size_t di : dying_docs) {
    auto *ddoc = _session->document_registry()->get_document_by_index(di);
    if (!ddoc)
      continue;
    for (auto &stack : ddoc->get_decoder_stacks()) {
      if (stack && stack->IsRunning()) {
        stack->stop_decode_work();
      }
    }
  }

  // 数据模型重构步骤6：所有权不可变，因此关闭一个 tab 永远不需要动其他
  // tab 的文档/身份——幸存者各自持有自己的文档。唯一需要处理的是"借用"：
  // 借用者若正借看着本 tab 拥有的池槽，解除借用并回落自己的数据（所有权
  // = 寿命：被借用文档的寿命不因借用而延长）。
  for (pv::TabContext *other : _tab_contexts) {
    if (other == ctx || !other->is_borrowing())
      continue;
    const bool borrowed_from_this_tab =
        std::find(dying_docs.begin(), dying_docs.end(),
                  _session->document_registry()->index_of_document(
                      other->render_document())) != dying_docs.end();
    if (!borrowed_from_this_tab)
      continue;
    const bool is_current = other == was_current;
    other->release_borrow();
    if (other->view()) {
      other->view()->set_data_document(nullptr);
      other->view()->clear_signal_data();
      other->view()->mark_derived_traces_dirty();
      other->view()->sync_derived_traces();
    }
    if (is_current)
      other->activate();   // 立即回落本 tab 数据（随后的统一 activate 也会覆盖）
    update_tab_style(_tab_contexts.indexOf(other));
  }

  // A2 fix: detach View→Document pointer BEFORE deleteLater().
  ctx->view()->set_data_document(nullptr);

  ctx->view()->deleteLater();
  SessionManager::instance()->destroy_context(ctx);

  if (_current_tab_index >= _tab_contexts.size()) {
    _current_tab_index = _tab_contexts.size() - 1;
  } else if (index < _current_tab_index) {
    _current_tab_index--;
  }

  // 数据模型重构步骤5：关闭 pxl/导入文件 tab 时直接 close_file() 释放本
  // tab 的虚拟设备。若被关设备恰是当前全局设备，close_file 的 isCurrent
  // 分支会 set_default_device() 回到非文件设备；随后的幸存 tab activate()
  // 再经 per-tab 设备恢复精确切回自己的设备。旧的"先
  // restore_previous_device（_saved_device_handle 单槽记忆）再 close_file"
  // 两段式已随 _saved_device_handle 机制一并退役。
  // NOTE: handles were cached up-front — ctx (and its pinned slots) has
  // already been destroyed by destroy_context() above.
  if (owns_file_devices) {
    for (ds_device_handle h : owned_file_handles)
      _session->close_file(h);
  }

  _tab_contexts[_current_tab_index]->activate();
  _tab_widget->setCurrentIndex(_current_tab_index);
  update_tab_style(_current_tab_index);

  pv::TabContext *new_ctx = _tab_contexts[_current_tab_index];
  bind_docks(new_ctx);

  pv::view::View *view = current_view();
  if (view) {
    set_view_on_docks(view);
  }

  QObject::connect(_tab_widget, &pv::ui::DraggableTabWidget::currentChanged, _wnd,
          &MainWindow::on_tab_changed);
}

void TabManager::update_tab_style(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;

  pv::TabContext *ctx = _tab_contexts[index];
  // 数据模型重构步骤6：借用态必须显式可见——标题加角标，用户一眼知道
  // "本 tab 正在借看别处的数据"，避免旧 rebind 模型静默改身份的困惑。
  QString text = ctx->title();
  if (ctx->is_borrowing())
    text += QString::fromUtf8("  ⇢ ") + ctx->borrow_label();
  _tab_widget->setTabText(index, text);

  // tab 状态变化（增删 / 切换 / 改名 / 借用角标）的统一出口 → 防抖写 workspace。
  schedule_workspace_save();
}

// ---------------------------------------------------------------------------
// Slot handlers
// ---------------------------------------------------------------------------

void TabManager::on_tab_changed(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;

  int old_index = _current_tab_index;
  pxv_info("TabManager::on_tab_changed(%d) old=%d", index, old_index);

  if (old_index >= 0 && old_index < _tab_contexts.size() &&
      old_index != index) {
    _tab_contexts[old_index]->deactivate();
    update_tab_style(old_index);
  }

  _current_tab_index = index;

  // 架构重构 Phase 5：移除 restore_previous_device() 兜底。所有标签创建
  // 路径（file_ops / tab_manager / on_load_device_first）均已记录 per-tab
  // 设备 handle，TabContext::activate() 的 per-tab 恢复即完备且语义精确
  // （handle -> 该标签自己的设备，TabSwitch reason）。旧兜底在
  // _saved_device_handle 用尽后会退化到 set_default_device()，曾误选设备
  // 列表末尾的 VCD/文件设备（有通道）而把 demo 标签"两通道化"。
  _tab_contexts[index]->activate();
  update_tab_style(index);

  // 恢复自 workspace 的文件设备 tab：启动时不批量读文件（那会触发整条采集 /
  // 回放管线），首次切到它时才按 filePath 懒加载重开。
  // 判据 = 有 filePath 但尚未绑定设备句柄（普通 tab 在创建时即已绑定）。
  if (!_reopening_file_tab) {
    pv::TabContext *ctx = _tab_contexts[index];
    if (ctx && ctx->device_handle() == NULL_HANDLE &&
        !ctx->file_path().isEmpty()) {
      _reopening_file_tab = true;
      _wnd->reopen_recovered_file_tab(ctx);
      _reopening_file_tab = false;
    }
  }

  pv::view::View *view = current_view();
  _wnd->update_sample_period();
  if (view) {
    if (old_index >= 0 && old_index < _tab_contexts.size() &&
        old_index != index) {
      unbind_docks();
    }

    pv::TabContext *new_ctx = _tab_contexts[index];
    bind_docks(new_ctx);

    view->installEventFilter(_wnd);
  }

  _wnd->update_title_bar_text();
  SessionManager::instance()->set_active_context(_tab_contexts[index]);
}

void TabManager::on_tab_moved(int from, int to) {
  if (from < 0 || from >= _tab_contexts.size() || to < 0 ||
      to >= _tab_contexts.size())
    return;
  if (from == to)
    return;

  pv::TabContext *ctx = _tab_contexts[from];
  _tab_contexts.removeAt(from);
  _tab_contexts.insert(to, ctx);

  if (_current_tab_index == from) {
    _current_tab_index = to;
  } else if (from < _current_tab_index && to >= _current_tab_index) {
    _current_tab_index--;
  } else if (from > _current_tab_index && to <= _current_tab_index) {
    _current_tab_index++;
  }
}

void TabManager::on_tab_detach(int index, QWidget *widget,
                                const QString &title) {
  (void)index;
  (void)title;

  pv::TabContext *ctx = nullptr;
  for (auto c : _tab_contexts) {
    if (c->view() == widget) {
      ctx = c;
      break;
    }
  }

  if (ctx) {
    if (ctx->is_live()) {
      ctx->deactivate();
    }
    _tab_contexts.removeOne(ctx);
    if (_current_tab_index >= _tab_contexts.size()) {
      _current_tab_index = _tab_contexts.size() - 1;
    }
    if (!_tab_contexts.isEmpty()) {
      _tab_contexts[_current_tab_index]->activate();
      update_tab_style(_current_tab_index);
    }
    SessionManager::instance()->detach_context(ctx);
    ctx->view()->setProperty("detached_ctx",
                             QVariant::fromValue((quintptr)ctx));
  }
}

void TabManager::on_tab_attached(QWidget *widget, const QString &title) {
  (void)title;
  QVariant prop = widget->property("detached_ctx");
  if (!prop.isValid() || prop.isNull())
    return;

  pv::TabContext *ctx =
      reinterpret_cast<pv::TabContext *>(prop.value<quintptr>());
  if (!ctx)
    return;

  _tab_contexts.append(ctx);
  SessionManager::instance()->attach_context(ctx);
  widget->setProperty("detached_ctx", QVariant());
}

void TabManager::on_new_tab_requested() {
  SigSession *_session = _wnd->session();
  toolbars::SamplingBar *_sampling_bar = _wnd->sampling_bar();
  DeviceAgent *_device_agent = _wnd->device_agent();

  pv::view::View *new_view = new pv::view::View(_session, _sampling_bar, _wnd);
  // phase 2: document owned by DocumentRegistry.
  size_t new_doc_idx = _session->document_registry()->take_document(
      std::make_unique<pv::data::SessionDocument>(_session->device()));
  pv::data::SessionDocument *new_doc =
      _session->document_registry()->get_document_by_index(new_doc_idx);

  if (_device_agent && _device_agent->have_instance()) {
    new_doc->save_signal_config(_session->get_signal_models(), {});
    pxv_info("TabManager::on_new_tab_requested() saved signal config, mode=%d "
             "ch_count=%d",
             new_doc->get_signal_config().work_mode,
             static_cast<int>(new_doc->get_signal_config().channels.size()));
  }

  pv::TabContext *new_ctx =
      SessionManager::instance()->create_context(new_view, _session, new_doc,
                                                 new_doc_idx,
                                                 _session->document_registry());
  new_ctx->set_title(
      QString::fromUtf8(L_S(STR_PAGE_MSG, S_ID(IDS_TAB_TITLE), "Tab %1"))
          .arg(_tab_contexts.size() + 1));

  // 阶段3修复 + 数据模型重构步骤5：新建空白 tab 记住自己的设备身份以供
  // activate() 恢复——但当前设备是文件/输入模块设备时绝不继承（文件设备
  // 身份随属主 tab 的生死失效，曾导致"FileDeviceClosed 后 handle=0 →
  // 显示全局设备"的混乱），改绑设备列表中第一个非文件设备（demo/硬件）。
  if (_device_agent && _device_agent->have_instance()) {
    ds_device_handle h = _device_agent->handle();
    if (_device_agent->is_file() || _device_agent->is_input_module())
      h = default_capture_handle(_session);
    new_ctx->set_device_handle(h);
    new_doc->set_device_handle(h);
  }

  add_tab(new_ctx);
}

void TabManager::on_close_others_requested(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;
  // remove_tab 会移除并移位 _tab_contexts，必须从最高索引向下关闭，
  // 才能保证保留的 index 不失效（remove_tab 内部有 size<=1 保护）。
  for (int i = _tab_contexts.size() - 1; i >= 0; --i) {
    if (i == index)
      continue;
    remove_tab(i);
  }
}

void TabManager::on_close_right_requested(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;
  for (int i = _tab_contexts.size() - 1; i > index; --i)
    remove_tab(i);
}

void TabManager::on_tab_renamed(int index, const QString &title) {
  if (index >= 0 && index < _tab_contexts.size()) {
    _tab_contexts[index]->set_title(title);
    update_tab_style(index);
  }
}

void TabManager::on_tab_attached_extended(QWidget *widget,
                                           const QString &title) {
  SigSession *_session = _wnd->session();

  pv::view::View *view = qobject_cast<pv::view::View *>(widget);
  if (view) {
    pv::TabContext *existing_ctx = nullptr;
    for (auto c : _tab_contexts) {
      if (c->view() == view) {
        existing_ctx = c;
        break;
      }
    }
    if (!existing_ctx) {
      QVariant var = view->property("detached_ctx");
      if (var.isValid()) {
        existing_ctx = (pv::TabContext *)(var.value<quintptr>());
        if (existing_ctx) {
          existing_ctx->set_title(title);
          _tab_contexts.append(existing_ctx);
          view->setProperty("detached_ctx", QVariant());
        }
      }
      if (!existing_ctx) {
        // phase 2: document owned by DocumentRegistry.
        size_t doc_idx = _session->document_registry()->take_document(
            std::make_unique<pv::data::SessionDocument>(_session->device()));
        pv::data::SessionDocument *doc =
            _session->document_registry()->get_document_by_index(doc_idx);
        pv::TabContext *ctx =
            SessionManager::instance()->create_context(view, _session,
                                                       doc, doc_idx,
                                                       _session->document_registry());
        ctx->set_title(title);

        // 阶段3修复 + 数据模型重构步骤5：重新 attach 新建的标签同样记录设备
        // 身份，文件/输入模块设备活跃时不继承（同 on_new_tab_requested）。
        if (_session->get_device() && _session->get_device()->have_instance()) {
          ds_device_handle h = _session->get_device()->handle();
          if (_session->get_device()->is_file() ||
              _session->get_device()->is_input_module())
            h = default_capture_handle(_session);
          ctx->set_device_handle(h);
          doc->set_device_handle(h);
        }

        _tab_contexts.append(ctx);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Workspace (跨会话 tab 会话持久化)
// 契约见 AGENT_CONTRACTS.md "Workspace (tab session) persistence"。
// ---------------------------------------------------------------------------

void TabManager::save_workspace() {
  Workspace ws;
  ws.activeTab = _current_tab_index;

  SigSession *session = _wnd ? _wnd->session() : nullptr;

  for (pv::TabContext *ctx : _tab_contexts) {
    if (!ctx)
      continue;
    WorkspaceTab t;
    t.title = ctx->title();
    t.filePath = ctx->file_path();
    t.isFileDevice = !t.filePath.isEmpty();
    // Preserve WHICH loader the path needs: native .pxl session vs input-module
    // import (VCD/CSV/...). Without this the restore path always used set_file()
    // and imported files never came back.
    t.isImportedFile = ctx->is_imported_file();
    t.importFormat = ctx->import_format();

    // Decoder (protocol analyzer) stacks. Two independent reasons the file
    // device paths cannot supply them on restore:
    //   1. file devices never write a `.pxc` (see
    //      MainWindowConfigIO::save_config — only hardware/demo do);
    //   2. an Open-path archive (.sr) has no "decoders" member of its own
    //      (unlike .pxl), and VCD/CSV imports have no container at all.
    // So the workspace is the only carrier for both. Scoped to THIS tab's
    // document so a multi-tab workspace records each tab's own stacks.
    //
    // This only reads a non-empty list if the decoder was actually filed under
    // ctx->document() — see ViewDerivedTraces::add_decoder(), which now names
    // the target document explicitly instead of relying on the registry's
    // global "active document" (unstable during an Open-path replay, because
    // TabContext::claim_active_document() is skipped while is_working()).
    if (session && ctx->document()) {
      StoreSession ss(session);
      ss.set_decoder_doc(ctx->document());
      ss.gen_decoders_json(t.decoder);
    }

    // 设备身份用 (driver, connid)：ds_device_handle 是进程内句柄，跨会话无意义
    // （契约 "Workspace (tab session) persistence"）。
    if (session && ctx->device_handle() != NULL_HANDLE)
      session->device_identity_of_handle(ctx->device_handle(), &t.driver,
                                         &t.connid);

    if (ctx->document()) {
      t.session = ctx->document()->signal_config_to_json();
      t.workMode = ctx->document()->get_signal_config().work_mode;
    }
    // 视图密度是 per-tab 状态：随 tab 会话走，不进 .pxc（.pxc 的自然键是
    // (driver, workMode)，同一设备同模式的多个 tab 会互相覆盖）。
    capture_session_ui_layout(ctx->view(), t.session);
    ws.tabs.push_back(t);
  }

  write_workspace_file(ws);
}

int TabManager::restore_workspace() {
  Workspace ws;
  if (!read_workspace_file(ws))
    return 0;  // 无 workspace / 解析失败 / 版本不支持 → 保持单 tab 行为

  SigSession *session = _wnd ? _wnd->session() : nullptr;
  if (!session)
    return 0;
  DeviceAgent *da = session->get_device();
  if (!da || !da->have_instance()) {
    pxv_warn("restore_workspace: no active device instance, skipping restore");
    return 0;
  }

  const int cur_mode = da->get_work_mode();
  const QString cur_driver = da->driver_name();

  // 恢复期间屏蔽 QTabWidget 的 currentChanged：addTab 期间若触发切换会反复
  // activate()（切设备 / 重放数据）。末尾再统一 setCurrentIndex(activeTab)。
  const bool blocked = _tab_widget->blockSignals(true);

  // --- tabs[0] → 已初始化的初始 tab ---
  // 双重守卫：workMode 一致 + driver 一致（老 workspace 无 driver 信息时放行）。
  // 模式不同时 apply_signal_config() 会切换设备模式，超出"FirstInit 已完成"的
  // 既有语义（契约 mode guard）；driver 不同说明当前设备与保存时不是同一台，
  // 套用会把别的设备（甚至别的通道数）的配置写进当前设备。
  pv::TabContext *ctx0 = current_context();
  if (ctx0 && ctx0->document() && !ws.tabs.empty()) {
    const WorkspaceTab &t0 = ws.tabs.front();
    const bool driver_ok = t0.driver.isEmpty() || t0.driver == cur_driver;
    if (t0.workMode == cur_mode && driver_ok && !t0.session.isEmpty()) {
      ctx0->document()->signal_config_from_json(t0.session);
      ctx0->document()->apply_signal_config();
      if (auto *v = ctx0->view()) {
        v->rebuild_signals();
        apply_session_ui_layout(v, t0.session);
      }
      if (!t0.title.isEmpty()) {
        ctx0->set_title(t0.title);
        _tab_widget->setTabText(0, t0.title);
      }
      pxv_info("restore_workspace: applied session to initial tab '%s'",
               t0.title.toUtf8().constData());
    } else {
      pxv_info("restore_workspace: initial tab session skipped "
               "(saved mode=%d/driver=%s, current=%d/%s) — keeping profile result",
               t0.workMode, t0.driver.toUtf8().constData(), cur_mode,
               cur_driver.toUtf8().constData());
    }
  }

  // --- tabs[1..] → 新建 tab（不激活） ---
  int restored = 0;
  for (size_t i = 1; i < ws.tabs.size(); ++i) {
    const WorkspaceTab &t = ws.tabs[i];

    // 设备句柄按 (driver, connid) 重新解析：ds_device_handle 是进程内句柄，
    // 旧值跨会话无效（契约 logical restore only）。解析不到时降级为"当前设备
    // 列表里第一个可用设备"。
    ds_device_handle h = NULL_HANDLE;
    if (!t.isFileDevice) {
      h = session->resolve_device_handle_by_identity(t.driver, t.connid);
      if (h == NULL_HANDLE)
        h = default_capture_handle(session);
    }
    // 文件设备 tab 故意保持未绑定（h == NULL_HANDLE）：不在启动时读文件，
    // 首次切到该 tab 时由 on_tab_changed 按 filePath 懒加载重开。

    auto *view = new pv::view::View(session, _wnd->sampling_bar(), _wnd);
    size_t doc_idx = session->document_registry()->take_document(
        std::make_unique<pv::data::SessionDocument>(session->get_device()));
    pv::data::SessionDocument *doc =
        session->document_registry()->get_document_by_index(doc_idx);
    if (!doc) {
      delete view;
      pxv_warn("restore_workspace: document registry refused tab %d",
               static_cast<int>(i));
      continue;
    }

    if (!t.session.isEmpty())
      doc->signal_config_from_json(t.session);

    pv::TabContext *ctx = SessionManager::instance()->create_context(
        view, session, doc, doc_idx, session->document_registry());
    if (!ctx) {
      pxv_warn("restore_workspace: create_context failed for tab %d",
               static_cast<int>(i));
      continue;
    }

    ctx->set_title(
        t.title.isEmpty()
            ? QString::fromUtf8(
                  L_S(STR_PAGE_MSG, S_ID(IDS_TAB_TITLE), "Tab %1"))
                  .arg(static_cast<int>(i) + 1)
            : t.title);
    ctx->set_file_path(t.filePath);
    // Remember which loader to use on the lazily-reopened tab (import_file for
    // VCD/CSV/..., set_file for .pxl). See TabManager::save_workspace.
    ctx->set_imported_file(t.isImportedFile);
    ctx->set_import_format(t.importFormat);
    // Decoder stacks to re-apply once the file is lazily reopened. Applied by
    // MainWindowFileOps::reload_file_into_context — NOT here, because the
    // decoders need their channels bound and that only happens after the file
    // device exists (contract "File-device tabs restore lazily").
    ctx->set_pending_decoders(t.decoder);
    if (h != NULL_HANDLE) {
      ctx->set_device_handle(h);
      doc->set_device_handle(h);
    }
    // per-tab 视图密度先落到 ViewLayout；切到该 tab 时的 rebuild 会用上它。
    apply_session_ui_layout(view, t.session);

    add_tab_silent(ctx);
    ++restored;
  }

  _tab_widget->blockSignals(blocked);

  // 统一切到保存时的活动 tab（触发 on_tab_changed → activate()）。
  const int target =
      (ws.activeTab >= 0 && ws.activeTab < static_cast<int>(_tab_contexts.size()))
          ? ws.activeTab
          : 0;
  if (_tab_widget->currentIndex() != target)
    _tab_widget->setCurrentIndex(target);
  _current_tab_index = target;

  pxv_info("restore_workspace: restored %d extra tab(s), active=%d",
           restored, target);
  return restored;
}

} // namespace pv
