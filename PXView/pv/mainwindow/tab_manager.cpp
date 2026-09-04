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
#include "pv/session/sessionmanager.h"
#include "pv/session/sigsession.h"
#include "pv/session/tabcontext.h"
#include "pv/toolbars/samplingbar.h"
#include "pv/ui/draggabletabwidget.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/view/view.h"

namespace pv {

// ---------------------------------------------------------------------------
// TabManager — construction / destruction
// ---------------------------------------------------------------------------

TabManager::TabManager(MainWindow *wnd) : _wnd(wnd) {}

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
             (int)initial_doc->get_signal_config().channels.size());
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

void TabManager::remove_tab(int index) {
  if (index < 0 || index >= _tab_contexts.size())
    return;

  if (_tab_contexts.size() <= 1)
    return;

  SigSession *_session = _wnd->session();

  pv::TabContext *ctx = _tab_contexts[index];
  // 方案A（问题2）：记录被关闭的 tab 是否为文件（pxl）tab。
  bool was_file_tab = !ctx->file_path().isEmpty();
  // Cache the tab's device handle up-front: ctx is deleted by
  // destroy_context() below, so it must not be dereferenced afterwards.
  const ds_device_handle closed_file_handle = ctx->device_handle();
  if (ctx->is_live() && _session->is_working()) {
    _session->stop_capture();
  }

  if (_session->get_active_document() == ctx->document()) {
    _session->set_active_document(nullptr);
  }

  _tab_contexts.removeAt(index);
  QObject::disconnect(_tab_widget, &pv::ui::DraggableTabWidget::currentChanged, _wnd,
             &MainWindow::on_tab_changed);
  _tab_widget->removeTab(index);
  // Task 4.3: capture owner cleanup is now RAII-managed by CaptureOwnerGuard.
  _session->clear_capture_owner_document(ctx->document());

  // A2 fix: stop decoder threads working on this document's stacks before the
  // document is destroyed.
  auto doc = ctx->document();
  if (doc) {
    for (auto &stack : doc->get_decoder_stacks()) {
      if (stack && stack->IsRunning()) {
        stack->stop_decode_work();
      }
    }
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

  // 方案A（问题2）+ 阶段2：关闭 pxl/导入文件 tab 时：
  //  1) 先恢复之前的硬件/demo 设备（_saved_device_handle），让当前设备回到
  //     非文件状态；
  //  2) 再 close_file() 释放本 tab 的虚拟设备。由于此时该文件设备已不是
  //     当前设备，close_file 只从 DeviceAgent 注销并 free 其 sdi，不会触发
  //     set_default_device（避免在后续 activate() 之外多切一次设备）。
  // 这样关闭文件 tab 不再泄漏虚拟设备，设备列表里也不会残留已关闭的文件。
  if (was_file_tab) {
    _session->restore_previous_device();
    // NOTE: use the cached handle — ctx has already been destroyed by
    // destroy_context() above.
    if (closed_file_handle != NULL_HANDLE)
      _session->close_file(closed_file_handle);
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
  _tab_widget->setTabText(index, ctx->title());
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

  // 方案A（问题2）：从文件（pxl）tab 切回非文件 tab 时，若当前全局设备仍是
  // 文件设备，恢复之前保存的硬件/demo 设备，避免全局设备被 pxl 占用导致
  // 该 tab 无法正常采集/显示实体设备数据。
  {
    pv::TabContext *target = _tab_contexts[index];
    SigSession *_session = _wnd->session();
    // 阶段3修复：仅当目标标签没有记录设备 handle（旧标签/未设置路径）时，
    // 才使用 restore_previous_device() 兜底。已记录 handle 的标签统一由
    // TabContext::activate() 的 per-tab 恢复处理，避免兜底在 _saved_device_handle
    // 用尽后退化到 set_default_device()，误选设备列表末尾的 VCD/文件设备
    // （有通道）而把 demo 标签"两通道化"。
    if (target && target->file_path().isEmpty() &&
        _session->get_device()->is_file() &&
        target->device_handle() == NULL_HANDLE) {
      pxv_info("TabManager::on_tab_changed: switching back to non-file tab, "
               "restoring hardware/demo device (fallback)");
      _session->restore_previous_device();
    }
  }

  _tab_contexts[index]->activate();
  update_tab_style(index);

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
             (int)new_doc->get_signal_config().channels.size());
  }

  pv::TabContext *new_ctx =
      SessionManager::instance()->create_context(new_view, _session, new_doc,
                                                 new_doc_idx,
                                                 _session->document_registry());
  new_ctx->set_title(
      QString::fromUtf8(L_S(STR_PAGE_MSG, S_ID(IDS_TAB_TITLE), "Tab %1"))
          .arg(_tab_contexts.size() + 1));

  // 阶段3修复：新建空白标签同样记住当前设备 handle（与 demo 默认标签一致），
  // 使其切回时能由 activate() 正确恢复自己的设备，而非依赖一次性兜底。
  if (_device_agent && _device_agent->have_instance()) {
    new_ctx->set_device_handle(_device_agent->handle());
    new_doc->set_device_handle(new_ctx->device_handle());
  }

  add_tab(new_ctx);
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

        // 阶段3修复：拖出窗口再重新 attach 时新建的标签也记住当前设备 handle。
        if (_session->get_device() && _session->get_device()->have_instance()) {
          ctx->set_device_handle(_session->get_device()->handle());
          doc->set_device_handle(ctx->device_handle());
        }

        _tab_contexts.append(ctx);
      }
    }
  }
}

} // namespace pv
