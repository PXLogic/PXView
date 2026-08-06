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

#include "mainwindow_tab_manager.h"

#include "mainwindow.h"

#include <QVariant>
#include <QVBoxLayout>
#include <QObject>
#include <QDebug>
#include <cstdio>

#include "core/documentregistry.h"
#include "data/decoderstack.h"
#include "data/sessiondocument.h"
#include "deviceagent.h"
#include "dock/dsotriggerdock.h"
#include "dock/logdock.h"
#include "dock/measuredock.h"
#include "dock/protocoldock.h"
#include "dock/searchdock.h"
#include "dock/triggerdock.h"
#include "dock/deviceoptionsdock.h"
#include "log.h"
#include "mainwindow.h"
#include "mainwindow_dock_manager.h"
#include "sessionmanager.h"
#include "sigsession.h"
#include "tabcontext.h"
#include "toolbars/samplingbar.h"
#include "ui/draggabletabwidget.h"
#include "ui/langresource.h"
#include "ui/msgbox.h"
#include "view/view.h"

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
      std::make_unique<pv::data::SessionDocument>(_session));
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
      std::make_unique<pv::data::SessionDocument>(_session));
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
            std::make_unique<pv::data::SessionDocument>(_session));
        pv::data::SessionDocument *doc =
            _session->document_registry()->get_document_by_index(doc_idx);
        pv::TabContext *ctx =
            SessionManager::instance()->create_context(view, _session,
                                                       doc, doc_idx,
                                                       _session->document_registry());
        ctx->set_title(title);
        _tab_contexts.append(ctx);
      }
    }
  }
}

} // namespace pv
