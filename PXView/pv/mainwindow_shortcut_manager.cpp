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

#include "mainwindow_shortcut_manager.h"
#include "mainwindow.h"
#include "mainwindow_dock_manager.h"
#include "mainwindow_tab_manager.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>

#include "config/appconfig.h"
#include "config/shortcutdefs.h"
#include "log.h"
#include "toolbars/filebar.h"
#include "toolbars/samplingbar.h"
#include "toolbars/trigbar.h"
#include "view/dsosignal.h"
#include "view/signal.h"
#include "view/view.h"
#include "widgets/searchpatterninput.h"
#include "widgets/sidebar.h"
#include "dock/deviceoptionsdock.h"
#include "dock/searchdock.h"
#include "dock/triggerdock.h"
#include "dock/protocoldock.h"
#include "dock/dsotriggerdock.h"
#include "dock/measuredock.h"

namespace pv {

MainWindowShortcutManager::MainWindowShortcutManager(MainWindow *wnd)
    : _wnd(wnd) {}

int MainWindowShortcutManager::resolveShortcutAction(int key, int modifiers) {
  AppConfig &app = AppConfig::Instance();
  int count = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&count);

  for (int i = 0; i < count; i++) {
    QString keySeqStr;

    bool found = false;
    for (int j = 0; j < app.shortcutOptions.items.size(); j++) {
      if (app.shortcutOptions.items[j].actionId == infos[i].actionId) {
        keySeqStr = app.shortcutOptions.items[j].keySequence;
        found = true;
        break;
      }
    }

    if (!found || keySeqStr.isEmpty()) {
      keySeqStr = infos[i].keySequence;
    }

    QKeySequence seq(keySeqStr);
    if (seq.count() > 0) {
      QKeyCombination combined = seq[0];
      int combinedInt = combined.toCombined();
      int seqKey = combinedInt & ~Qt::KeyboardModifierMask;
      int seqMods = combinedInt & Qt::KeyboardModifierMask;

      if (seqMods == 0 && modifiers == 0 && seqKey == key) {
        return infos[i].actionId;
      }

      if (seqMods != 0) {
        bool modsMatch = true;
        if ((seqMods & Qt::ShiftModifier) && !(modifiers & Qt::ShiftModifier))
          modsMatch = false;
        if ((seqMods & Qt::ControlModifier) &&
            !(modifiers & Qt::ControlModifier))
          modsMatch = false;
        if ((seqMods & Qt::AltModifier) && !(modifiers & Qt::AltModifier))
          modsMatch = false;
        if (modsMatch && seqKey == key) {
          return infos[i].actionId;
        }
      }
    }
  }

  return 0;
}

bool MainWindowShortcutManager::handleKeyPress(QObject *object,
                                                QEvent *event) {
  (void)object;

  QKeyEvent *ke = (QKeyEvent *)event;
  QWidget *focused = qApp->focusWidget();

  pxv_info("MainWindow::eventFilter key=%d, object=%p (%s), focused=%p (%s)",
           ke->key(), object, object->metaObject()->className(), focused,
           focused ? focused->metaObject()->className() : "nullptr");

  if (focused && qobject_cast<pv::widgets::SearchPatternInput *>(focused)) {
    static bool in_filter = false;
    if (in_filter)
      return false;
    in_filter = true;
    qApp->sendEvent(focused, event);
    in_filter = false;
    return true;
  }

  // Manually forward events to focus widget if it's an input or in the drawer
  if (focused &&
      (qobject_cast<QLineEdit *>(focused) ||
       qobject_cast<QAbstractSpinBox *>(focused) ||
       qobject_cast<QComboBox *>(focused) ||
       qobject_cast<QAbstractButton *>(focused) ||
       (_wnd->dock_manager()->sliding_drawer() &&
        _wnd->dock_manager()->sliding_drawer()->isAncestorOf(focused)) ||
       (_wnd->dock_manager()->device_options_widget() &&
        _wnd->dock_manager()->device_options_widget()->isAncestorOf(focused)) ||
       (_wnd->dock_manager()->search_widget() &&
        _wnd->dock_manager()->search_widget()->isAncestorOf(focused)) ||
       (_wnd->dock_manager()->trigger_widget() &&
        _wnd->dock_manager()->trigger_widget()->isAncestorOf(focused)) ||
       (_wnd->dock_manager()->protocol_widget() &&
        _wnd->dock_manager()->protocol_widget()->isAncestorOf(focused)) ||
       (_wnd->dock_manager()->dso_trigger_widget() &&
        _wnd->dock_manager()->dso_trigger_widget()->isAncestorOf(focused)) ||
       (_wnd->dock_manager()->measure_widget() &&
        _wnd->dock_manager()->measure_widget()->isAncestorOf(focused)))) {
    QWidget *target = focused;
    if (focused->focusProxy()) {
      target = focused->focusProxy();
    } else if (qobject_cast<QAbstractSpinBox *>(focused) ||
               qobject_cast<QComboBox *>(focused)) {
      QLineEdit *le = focused->findChild<QLineEdit *>();
      if (le) {
        target = le;
      }
    }

    QString text = ke->text();
    uint key = ke->key();

    // Fix for WinNativeWidget's raw VK codes
    if (key == 0x08)
      key = Qt::Key_Backspace;
    else if (key == 0x0D)
      key = Qt::Key_Return;
    else if (key == 0x25)
      key = Qt::Key_Left;
    else if (key == 0x26)
      key = Qt::Key_Up;
    else if (key == 0x27)
      key = Qt::Key_Right;
    else if (key == 0x28)
      key = Qt::Key_Down;
    else if (key == 0x2E)
      key = Qt::Key_Delete;
    else if (key == 0x24)
      key = Qt::Key_Home;
    else if (key == 0x23)
      key = Qt::Key_End;
    else if (key >= 0x60 && key <= 0x69) // VK_NUMPAD0 to VK_NUMPAD9
      key = Qt::Key_0 + (key - 0x60);
    else if (key == 0x6A) // VK_MULTIPLY
      key = Qt::Key_Asterisk;
    else if (key == 0x6B) // VK_ADD
      key = Qt::Key_Plus;
    else if (key == 0x6D) // VK_SUBTRACT
      key = Qt::Key_Minus;
    else if (key == 0x6E) // VK_DECIMAL
      key = Qt::Key_Period;
    else if (key == 0x6F) // VK_DIVIDE
      key = Qt::Key_Slash;

    if (text.isEmpty() && target->inherits("QLineEdit")) {
      if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
        char c = (char)key;
        bool shift = (ke->modifiers() & Qt::ShiftModifier);
        if (c >= 'A' && c <= 'Z' && !shift) {
          c += 32;
        } else if (c >= 'a' && c <= 'z' && shift) {
          c -= 32;
        }
        text = QString(QChar(c));
      }
    }

    QKeyEvent newEvent(ke->type(), key, ke->modifiers(), text,
                       ke->isAutoRepeat(), ke->count());

    pxv_info("  Forwarding event to focused widget: %s (target: %s, text: "
             "%s, mapped_key: %d)",
             focused->metaObject()->className(),
             target->metaObject()->className(), text.toStdString().c_str(),
             key);
    static bool in_forward = false;
    if (in_forward)
      return true;
    in_forward = true;
    qApp->sendEvent(target, &newEvent);
    in_forward = false;
    return true;
  }

  const auto &sigs = _wnd->current_view()->get_own_signals();

  int modifier = ke->modifiers();

  // Ctrl+Z — undo the most recent glitch filter application (Task 9).
  // Handled here before the generic shortcut resolver because the
  // configurable shortcut system does not define an Undo action; the
  // generic path below would otherwise consume Ctrl+Z (returns true for
  // unrecognized Ctrl combos) and swallow the keystroke.
  if ((modifier & Qt::ControlModifier) && ke->key() == Qt::Key_Z) {
    pv::view::View *view = _wnd->current_view();
    if (view && view->can_undo_filter()) {
      view->undo_filter();
      return true;
    }
  }

  int action = resolveShortcutAction(ke->key(), (int)modifier);
  if (action == 0) {
    if (modifier & Qt::ControlModifier || modifier & Qt::AltModifier) {
      return true;
    }
    return false;
  }

  switch (action) {
  case SHORTCUT_RUN_STOP:
    _wnd->sampling_bar()->run_or_stop();
    break;
  case SHORTCUT_INSTANT:
    _wnd->sampling_bar()->run_or_stop_instant();
    break;
  case SHORTCUT_TRIGGER:
    _wnd->dock_manager()->side_bar()->getItem(MainWindow::SIDEBAR_TRIGGER)->button->click();
    break;
  case SHORTCUT_DECODE:
    _wnd->dock_manager()->side_bar()->getItem(MainWindow::SIDEBAR_DECODE)->button->click();
    break;
  case SHORTCUT_MEASURE:
    _wnd->dock_manager()->side_bar()->getItem(MainWindow::SIDEBAR_MEASURE)->button->click();
    break;
  case SHORTCUT_SEARCH:
    _wnd->dock_manager()->side_bar()->getItem(MainWindow::SIDEBAR_SEARCH)->button->click();
    break;
  case SHORTCUT_OPTIONS:
    _wnd->dock_manager()->side_bar()->getItem(MainWindow::SIDEBAR_OPTIONS)->button->click();
    break;
  case SHORTCUT_DEVICE_SELECT:
    _wnd->sampling_bar()->device_selected();
    break;
  case SHORTCUT_PAGE_UP:
    _wnd->current_view()->set_scale_offset(
        _wnd->current_view()->scale(),
        _wnd->current_view()->offset() - _wnd->current_view()->get_view_width());
    break;
  case SHORTCUT_PAGE_DOWN:
    _wnd->current_view()->set_scale_offset(
        _wnd->current_view()->scale(),
        _wnd->current_view()->offset() + _wnd->current_view()->get_view_width());
    break;
  case SHORTCUT_ZOOM_IN:
    _wnd->current_view()->zoom(1);
    break;
  case SHORTCUT_ZOOM_OUT:
    _wnd->current_view()->zoom(-1);
    break;
  case SHORTCUT_DSO_CH0:
    for (auto &s : sigs) {
      if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
        if (dsoSig->get_index() == 0)
          dsoSig->set_vDialActive(!dsoSig->get_vDialActive());
        else
          dsoSig->set_vDialActive(false);
      }
    }
    _wnd->current_view()->setFocus();
    _wnd->update();
    break;
  case SHORTCUT_DSO_CH1:
    for (auto &s : sigs) {
      if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
        if (dsoSig->get_index() == 1)
          dsoSig->set_vDialActive(!dsoSig->get_vDialActive());
        else
          dsoSig->set_vDialActive(false);
      }
    }
    _wnd->current_view()->setFocus();
    _wnd->update();
    break;
  case SHORTCUT_DSO_VUP:
    for (auto &s : sigs) {
      if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
        if (dsoSig->get_vDialActive()) {
          dsoSig->go_vDialNext(true);
          _wnd->update();
          break;
        }
      }
    }
    break;
  case SHORTCUT_DSO_VDOWN:
    for (auto &s : sigs) {
      if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
        if (dsoSig->get_vDialActive()) {
          dsoSig->go_vDialPre(true);
          _wnd->update();
          break;
        }
      }
    }
    break;
  case SHORTCUT_FILE_OPEN:
    _wnd->file_bar()->_action_open->trigger();
    break;
  case SHORTCUT_FILE_SAVE:
    _wnd->file_bar()->_action_save->trigger();
    break;
  case SHORTCUT_FILE_EXPORT:
    _wnd->file_bar()->_action_export->trigger();
    break;
  case SHORTCUT_FILE_IMPORT:
    _wnd->file_bar()->_action_import->trigger();
    break;
  case SHORTCUT_FILE_LOAD:
    _wnd->file_bar()->_action_load->trigger();
    break;
  case SHORTCUT_FILE_STORE:
    _wnd->file_bar()->_action_store->trigger();
    break;
  case SHORTCUT_SCREENSHOT:
    _wnd->file_bar()->_action_capture->trigger();
    break;
  case SHORTCUT_FFT:
    _wnd->trig_bar()->_action_fft->trigger();
    break;
  case SHORTCUT_MATH:
    _wnd->trig_bar()->_action_math->trigger();
    break;
  case SHORTCUT_LISSAJOUS:
    _wnd->trig_bar()->_action_lissajous->trigger();
    break;
  case SHORTCUT_SETTINGS:
    _wnd->trig_bar()->_action_dispalyOptions->trigger();
    break;
  case SHORTCUT_LOG:
    _wnd->dock_manager()->side_bar()->getItem(MainWindow::SIDEBAR_LOG)->button->click();
    break;
  case SHORTCUT_FUNCTION:
    _wnd->dock_manager()->side_bar()->getItem(MainWindow::SIDEBAR_FUNCTION)->button->click();
    break;
  case SHORTCUT_THEME_TOGGLE: {
    AppConfig &app = AppConfig::Instance();
    if (app.IsDarkStyle())
      _wnd->switchTheme(THEME_STYLE_LIGHT);
    else
      _wnd->switchTheme(THEME_STYLE_DARK);
    break;
  }
  case SHORTCUT_NEW_TAB:
    _wnd->on_new_tab_requested();
    break;
  case SHORTCUT_CLOSE_TAB:
    if (_wnd->tab_manager()->tab_widget() &&
        _wnd->tab_manager()->tab_widget()->count() > 0)
      _wnd->remove_tab(_wnd->tab_manager()->tab_widget()->currentIndex());
    break;
  case SHORTCUT_ZOOM_FIT:
    if (_wnd->current_view()) {
      _wnd->current_view()->auto_set_max_scale();
      _wnd->current_view()->set_scale_offset(
          _wnd->current_view()->scale(), 0);
    }
    break;
  default:
    return false;
  }
  return true;
}

} // namespace pv
