/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
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

#ifndef PXVIEW_PV_LOGDOCK_H
#define PXVIEW_PV_LOGDOCK_H

#include <QCheckBox>
#include <QDockWidget>
#include <QFrame>
#include <QLabel>
#include <QMutex>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QTimer>

#include "../interface/icontextaware.h"
#include "../ui/dscombobox.h"
#include "../ui/uimanager.h"
#include "../widgets/smoothscrollarea.h"
#include <log/xlog.h>

namespace pv {

namespace dock {

class LogDock : public pv::widgets::SmoothScrollArea,
                public IContextAware,
                public IUiWindow {
  Q_OBJECT

public:
  LogDock(QWidget *parent);
  ~LogDock();

  void bind_context(TabContext *ctx) override;
  void unbind_context() override;

  static void on_log_callback(const char *data, int length);

private:
  void retranslateUi();
  void reStyle();

  void UpdateLanguage() override;
  void UpdateTheme() override;
  void UpdateFont() override;

  void load_log_file();
  void append_log_text(const QString &text);

protected:
  void showEvent(QShowEvent *event) override;

private slots:
  void on_clear();
  void on_level_changed(int index);
  void on_scroll_bottom_changed(bool checked);
  void on_flush_buffer();
  void on_save_to_file_changed(int state);
  void on_append_mode_changed(int state);
  void on_open_log_file();

private:
  QWidget *_widget;
  QPlainTextEdit *_log_view;
  QLabel *_level_label;
  QPushButton *_clear_btn;
  QPushButton *_open_btn;
  DsComboBox *_level_combo;
  QPushButton *_scroll_bottom_btn;
  QCheckBox *_save_file_check;
  QCheckBox *_append_mode_check;
  bool _auto_scroll;
  bool _needs_reload;

  QTimer _buffer_timer;

  static QMutex _log_mutex;
  static QString _log_buffer;
  static LogDock *_instance;
  int _callback_index;
};

} // namespace dock
} // namespace pv

#endif // PXVIEW_PV_LOGDOCK_H
