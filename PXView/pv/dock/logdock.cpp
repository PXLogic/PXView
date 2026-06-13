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

#include "logdock.h"

#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../log.h"
#include "../ui/dockfonts.h"
#include "../ui/dscombobox.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QShowEvent>
#include <QTextCursor>
#include <QTextStream>
#include <QUrl>
#include <QtGlobal>

namespace pv {
namespace dock {

QMutex LogDock::_log_mutex;
QString LogDock::_log_buffer;
LogDock *LogDock::_instance = nullptr;

LogDock::LogDock(QWidget *parent)
    : pv::widgets::SmoothScrollArea(parent), _auto_scroll(true),
      _needs_reload(false), _callback_index(-1) {
  _instance = this;

  _widget = new QWidget(this);

  _log_view = new QPlainTextEdit(_widget);
  _log_view->setReadOnly(true);
  _log_view->setLineWrapMode(QPlainTextEdit::NoWrap);
  _log_view->setMaximumBlockCount(10000);
  _log_view->setObjectName("log_view");
  QFont font = dock_font_content();
  font.setFamily("Source Code Pro");
  font.setStyleHint(QFont::Monospace);
  font.setFixedPitch(true);
  _log_view->setFont(font);

  _level_combo = new DsComboBox(_widget);
  _level_combo->setObjectName("dock_label");
  _level_combo->setMinimumWidth(60);
  for (int i = 0; i <= 5; i++) {
    _level_combo->addItem(QString::number(i));
  }
  _level_combo->setCurrentIndex(AppConfig::Instance().appOptions.logLevel);
  connect(_level_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &LogDock::on_level_changed);

  _scroll_bottom_btn = new QPushButton(_widget);
  _scroll_bottom_btn->setCheckable(true);
  _scroll_bottom_btn->setChecked(true);
  _scroll_bottom_btn->setFixedSize(28, 28);
  _scroll_bottom_btn->setObjectName("log_scroll_bottom_btn");
  connect(_scroll_bottom_btn, &QPushButton::clicked, this,
          &LogDock::on_scroll_bottom_changed);

  _clear_btn = new QPushButton(_widget);
  _clear_btn->setFixedHeight(28);
  _clear_btn->setObjectName("log_clear_btn");
  connect(_clear_btn, &QPushButton::clicked, this, &LogDock::on_clear);

  _save_file_check = new QCheckBox(_widget);
  _save_file_check->setObjectName("dock_label");
  _save_file_check->setChecked(AppConfig::Instance().appOptions.ableSaveLog);
  connect(_save_file_check, &QCheckBox::checkStateChanged, this,
          &LogDock::on_save_to_file_changed);

  _append_mode_check = new QCheckBox(_widget);
  _append_mode_check->setObjectName("dock_label");
  _append_mode_check->setChecked(
      AppConfig::Instance().appOptions.appendLogMode);
  connect(_append_mode_check, &QCheckBox::checkStateChanged, this,
          &LogDock::on_append_mode_changed);

  _open_btn = new QPushButton(_widget);
  _open_btn->setFixedHeight(28);
  _open_btn->setObjectName("log_open_btn");
  connect(_open_btn, &QPushButton::clicked, this, &LogDock::on_open_log_file);

  QFile qf(get_pxv_log_path());
  if (!qf.exists()) {
    _open_btn->setEnabled(false);
  }

  QHBoxLayout *toolbar_layout = new QHBoxLayout();
  toolbar_layout->setContentsMargins(0, 0, 0, 0);
  toolbar_layout->setSpacing(6);

  _level_label = new QLabel(_widget);
  _level_label->setObjectName("dock_label");
  _level_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_LEVEL), "Log Level"));
  toolbar_layout->addWidget(_level_label);
  toolbar_layout->addWidget(_level_combo);
  toolbar_layout->addWidget(_save_file_check);
  toolbar_layout->addWidget(_append_mode_check);
  toolbar_layout->addStretch(1);
  toolbar_layout->addWidget(_scroll_bottom_btn);
  toolbar_layout->addWidget(_open_btn);
  toolbar_layout->addWidget(_clear_btn);

  QVBoxLayout *main_layout = new QVBoxLayout(_widget);
  main_layout->setContentsMargins(12, 8, 12, 8);
  main_layout->setSpacing(6);
  main_layout->addLayout(toolbar_layout);
  main_layout->addWidget(_log_view, 1);

  _widget->setLayout(main_layout);

  this->setFrameShape(QFrame::NoFrame);
  this->setObjectName("dock_log_scroll");
  this->setWidgetResizable(true);
  this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  this->setWidget(_widget);
  _widget->setObjectName("logWidget");

  _buffer_timer.setInterval(100);
  connect(&_buffer_timer, &QTimer::timeout, this, &LogDock::on_flush_buffer);
  _buffer_timer.start();

  xlog_context *ctx = pxv_log_context();
  if (ctx) {
    xlog_add_receiver(ctx, on_log_callback, &_callback_index);
  }

  retranslateUi();

  ADD_UI(this);
}

LogDock::~LogDock() {
  _buffer_timer.stop();

  xlog_context *ctx = pxv_log_context();
  if (ctx && _callback_index >= 0) {
    xlog_remove_receiver_by_index(ctx, _callback_index);
  }

  _instance = nullptr;
  REMOVE_UI(this);
}

void LogDock::on_log_callback(const char *data, int length) {
  if (_instance) {
    QMutexLocker locker(&_log_mutex);
    _log_buffer.append(QString::fromUtf8(data, length));
  }
}

void LogDock::bind_context(TabContext *ctx) { (void)ctx; }

void LogDock::unbind_context() {}

void LogDock::load_log_file() {
  QString log_path = get_pxv_log_path();
  QFile file(log_path);
  if (!file.exists()) {
    _log_view->setPlainText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_NO_FILE), "No log file found."));
    return;
  }
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    _log_view->setPlainText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_OPEN_FAIL),
                                "Failed to open log file."));
    return;
  }

  // Read only the last ~10000 lines to avoid freezing when the log file
  // is huge (common in debug builds with DIAG/PROFILER messages).
  // setPlainText with a giant string creates hundreds of thousands of
  // QTextBlock objects then trims via setMaximumBlockCount — the cost is
  // paid in layout/paint when the widget first becomes visible.
  static const int kMaxInitLines = 10000;
  qint64 fileSize = file.size();
  if (fileSize <= 1024 * 1024) {
    // Small file: read everything
    QTextStream in(&file);
    _log_view->setPlainText(in.readAll());
  } else {
    // Large file: seek to the last ~256KB and read from there,
    // then take only the last kMaxInitLines.
    qint64 seekPos = qMax<qint64>(0, fileSize - 256 * 1024);
    file.seek(seekPos);
    // Skip partial first line if we didn't start at beginning
    if (seekPos > 0)
      file.readLine();
    QString content = QString::fromUtf8(file.readAll());
    QStringList lines = content.split('\n');
    if (lines.size() > kMaxInitLines) {
      lines = lines.mid(lines.size() - kMaxInitLines);
    }
    _log_view->setPlainText(lines.join('\n'));
  }
  file.close();

  QScrollBar *sb = _log_view->verticalScrollBar();
  sb->setValue(sb->maximum());
}

void LogDock::append_log_text(const QString &text) {
  if (text.isEmpty())
    return;
  QScrollBar *sb = _log_view->verticalScrollBar();
  int saved_pos = sb->value();
  bool was_at_bottom = (saved_pos >= sb->maximum() - 4);
  _log_view->appendPlainText(text.trimmed());
  if (_auto_scroll || was_at_bottom) {
    sb->setValue(sb->maximum());
  } else {
    sb->setValue(saved_pos);
  }
}

void LogDock::showEvent(QShowEvent *event) {
  pv::widgets::SmoothScrollArea::showEvent(event);

  // Discard stale flag — just start receiving new data from now on.
  // Old data discarded while hidden is still in the log file on disk.
  _needs_reload = false;
}

void LogDock::on_clear() {
  pxv_clear_log_file();
  _log_view->clear();
  QFile qf(get_pxv_log_path());
  _open_btn->setEnabled(qf.exists());
}

void LogDock::on_level_changed(int index) {
  pxv_log_level(index);
  AppConfig::Instance().appOptions.logLevel = index;
  AppConfig::Instance().SaveApp();
}

void LogDock::on_scroll_bottom_changed(bool checked) {
  _auto_scroll = checked;
  if (_auto_scroll) {
    QScrollBar *sb = _log_view->verticalScrollBar();
    sb->setValue(sb->maximum());
  }
}

void LogDock::on_save_to_file_changed(int state) {
  bool checked = (state == Qt::Checked);
  AppConfig &app = AppConfig::Instance();
  app.appOptions.ableSaveLog = checked;
  app.SaveApp();

  if (checked) {
    pxv_log_enalbe_logfile(false);
  }
  pxv_set_log_file_enable(checked);

  QFile qf(get_pxv_log_path());
  _open_btn->setEnabled(qf.exists());
}

void LogDock::on_append_mode_changed(int state) {
  bool checked = (state == Qt::Checked);
  AppConfig &app = AppConfig::Instance();
  app.appOptions.appendLogMode = checked;
  app.SaveApp();
}

void LogDock::on_open_log_file() {
  QFile qf(get_pxv_log_path());
  if (qf.exists()) {
    QDesktopServices::openUrl(QUrl("file:///" + get_pxv_log_path()));
  } else {
    QString strMsg(
        L_S(STR_PAGE_MSG, S_ID(IDS_MSG_FILE_NOT_EXIST), "Not exist!"));
    MsgBox::Show(strMsg);
  }
}

void LogDock::on_flush_buffer() {
  // When not visible, just drain the buffer to prevent unbounded memory growth.
  // Data is still written to the log file by xlog, so nothing is lost.
  // When the panel becomes visible again, showEvent will reload from file.
  if (!isVisible()) {
    QMutexLocker locker(&_log_mutex);
    if (!_log_buffer.isEmpty()) {
      _needs_reload = true;
      _log_buffer.clear();
    }
    return;
  }

  QString text;
  {
    QMutexLocker locker(&_log_mutex);
    if (!_log_buffer.isEmpty()) {
      text = _log_buffer;
      _log_buffer.clear();
    }
  }
  if (!text.isEmpty()) {
    QScrollBar *sb = _log_view->verticalScrollBar();
    int saved_pos = sb->value();
    bool was_at_bottom = (saved_pos >= sb->maximum() - 4);

    QStringList lines = text.split('\n', Qt::SkipEmptyParts);

    // Cap the number of lines per flush to keep UI responsive.
    // Any overflow is pushed back to the buffer for next timer tick.
    const int kMaxLinesPerFlush = 200;
    if (lines.size() > kMaxLinesPerFlush) {
      QStringList overflow = lines.mid(kMaxLinesPerFlush);
      lines = lines.mid(0, kMaxLinesPerFlush);
      // Put overflow back into buffer for next flush
      QMutexLocker locker(&_log_mutex);
      _log_buffer.prepend(overflow.join('\n') + '\n');
    }

    // Batch insert using QTextCursor to avoid per-line document relayout.
    // A single insertText call triggers ONE relayout instead of N.
    if (!lines.isEmpty()) {
      QTextCursor cursor = _log_view->textCursor();
      cursor.movePosition(QTextCursor::End);
      QString batch = lines.join('\n');
      if (!_log_view->document()->isEmpty()) {
        batch.prepend('\n');
      }
      cursor.insertText(batch);
    }

    if (_auto_scroll || was_at_bottom) {
      sb->setValue(sb->maximum());
    } else {
      sb->setValue(saved_pos);
    }
  }
}

void LogDock::retranslateUi() {
  if (_level_label) {
      _level_label->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_LEVEL), "Log Level"));
  }
  _clear_btn->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CLEARE), "Clear"));
  _open_btn->setText(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_OPEN), "Open"));
  _scroll_bottom_btn->setToolTip(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_LOG_AUTO_SCROLL), "Auto Scroll"));
  _save_file_check->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SAVE_FILE), "Save To File"));
  _append_mode_check->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_APPEND_MODE), "Append mode"));
}

void LogDock::reStyle() {
  QString iconPath = GetIconPath();
  _scroll_bottom_btn->setIcon(QIcon(iconPath + "/scroll-bottom.svg"));
  _open_btn->setIcon(QIcon(iconPath + "/open.svg"));
}

void LogDock::UpdateLanguage() { retranslateUi(); }

void LogDock::UpdateTheme() { reStyle(); }

void LogDock::UpdateFont() {
  QFont font = dock_font_content();
  font.setFamily("Source Code Pro");
  font.setStyleHint(QFont::Monospace);
  font.setFixedPitch(true);
  _log_view->setFont(font);
}

} // namespace dock
} // namespace pv
