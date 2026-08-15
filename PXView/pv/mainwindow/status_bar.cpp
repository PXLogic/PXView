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

#include "pv/mainwindow/status_bar.h"
#include "pv/mainwindow/mainwindow.h"
#include "pv/mainwindow/dock_manager.h"
#include "pv/mainwindow/tab_manager.h"

#include <QDateTime>
#include <QDir>
#include <QLabel>

#include "pv/config/appconfig.h"
#include "pv/session/sigsession.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/session/tabcontext.h"
#include "pv/view/view.h"
#include "pv/view/viewport/viewport.h"
#include "pv/ui/langresource.h"

namespace pv {

MainWindowStatusBar::MainWindowStatusBar(MainWindow *wnd)
    : _wnd(wnd) {}

void MainWindowStatusBar::init(QLabel *disk_cache_label,
                                QLabel *trig_time_label,
                                QLabel *sample_period_label,
                                QLabel *fps_label) {
  _disk_cache_label = disk_cache_label;
  _trig_time_label = trig_time_label;
  _sample_period_label = sample_period_label;
  _fps_label = fps_label;
}

void MainWindowStatusBar::update_disk_cache_status() {
  update_sample_period();
  DeviceAgent *agent = _wnd->device_agent();
  if (!agent || !agent->have_instance()) {
    if (_disk_cache_label)
      _disk_cache_label->hide();
    _trig_time_label->hide();
    return;
  }

  SigSession *session = _wnd->session();
  QDateTime trig_time = session->get_trig_time();
  if (session->is_triged() && trig_time.isValid()) {
    _trig_time_label->setText(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIGGER_TIME), "Trigger Time: ") +
        trig_time.toString("yyyy-MM-dd hh:mm:ss"));
    _trig_time_label->show();
  } else {
    _trig_time_label->hide();
  }

  bool cache_enabled = false;
  // DISK_CACHE_ENABLE is a PXLogic fork key — only DSL/PXLogic devices
  // implement it. demo/file/compat devices would otherwise log "Option not
  // available" every 500ms via _disk_cache_status_timer.
  if (agent->is_dsl_device())
    agent->get_config_bool(SR_CONF_DISK_CACHE_ENABLE, cache_enabled);

  if (!cache_enabled) {
    _disk_cache_label->hide();
    return;
  }

  QString cache_path;
  agent->get_config_string(SR_CONF_DISK_CACHE_PATH, cache_path);
  if (cache_path.isEmpty()) {
    cache_path = QDir::tempPath() + "/PXView_cache";
  }
  QString text = QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_ON),
                             "Disk Cache: ON")) +
                 " | " +
                 QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_PATH_LABEL),
                             "Path: ")) +
                 cache_path;

  double wspeed = session->get_disk_write_speed_mbps();
  size_t qdepth = session->get_disk_write_queue_depth();

  data::LogicSnapshot *logic = session->get_logic_snapshot();
  uint64_t pf = 0;
  uint64_t ws = 0;
  uint64_t qb = 0;

  if (logic) {
    pf = logic->get_page_fault_count();
    ws = logic->get_working_set_bytes();
    qb = logic->get_async_queue_bytes();
  }

  if (!session->is_working()) {
    wspeed = 0.0;
  }

  text +=
      " | " +
      QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_WRITE), "Write: ")) +
      QString("%1 MB/s").arg(wspeed, 0, 'f', 1);

  if (logic) {
    text +=
        " | " +
        QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_QUEUE), "Queue: ")) +
        QString("%1 MB (%2 blks)")
            .arg(qb / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(qdepth);
    text += " | " +
            QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_RAM), "RAM: ")) +
            QString("%1 MB").arg(ws / (1024.0 * 1024.0), 0, 'f', 1);
    text += " | " +
            QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_PF), "PF/s: ")) +
            QString("%1").arg(pf);

    if (logic->is_disk_cache_active()) {
      // raw 口径 (raw 复原 spec, 用户拍板): mmap 分配器文件大小 = "磁盘当内存"
      // 的实际占用, 比块数启发式 (total_blocks * 2105376) 更准确.
      uint64_t disk_bytes = session->get_logic_disk_bytes();
      text +=
          " | " +
          QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_DISK), "Disk: ")) +
          QString("%1 GB").arg(disk_bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
  }

  if (session->is_disk_write_disk_full()) {
    text += " | " + QString(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISK_CACHE_FULL),
                                "DISK FULL"));
    _disk_cache_label->setStyleSheet("color: red; font-weight: bold;");
  } else if (qdepth > 256) {
    _disk_cache_label->setStyleSheet("color: red; font-weight: bold;");
  } else if (qdepth > 64) {
    _disk_cache_label->setStyleSheet(
        "color: yellow; font-weight: bold;");
  } else {
    _disk_cache_label->setStyleSheet("");
  }

  _disk_cache_label->setText(text);
  _disk_cache_label->show();
}

void MainWindowStatusBar::update_fps() {
  int ui_fps = 0;
  pv::view::View *cur_view = _wnd->current_view();
  if (cur_view && cur_view->get_time_view()) {
    ui_fps = cur_view->get_time_view()->get_fps();
  }

  int dock_fps = 0;
  if (_wnd->dock_manager() && _wnd->dock_manager()->sliding_drawer()) {
    dock_fps = _wnd->dock_manager()->sliding_drawer()->get_fps();
  }

  _wnd->acq_count() = 0;

  if (_fps_label) {
    QString fps_text =
        QString("UI: %1ms | Dock: %2ms").arg(ui_fps).arg(dock_fps);
    _fps_label->setText(fps_text);
    _fps_label->show();
  }
}

void MainWindowStatusBar::update_sample_period() {
  if (!_sample_period_label)
    return;

  pv::TabContext *ctx = _wnd->current_context();
  if (!ctx || !ctx->document()) {
    _sample_period_label->setText(
        (AppConfig::Instance().frameOptions.language == LAN_CN)
            ? "采样周期: --"
            : "Sample Period: --");
    return;
  }

  uint64_t samplerate = ctx->document()->get_samplerate();
  if (samplerate == 0) {
    _sample_period_label->setText(
        (AppConfig::Instance().frameOptions.language == LAN_CN)
            ? "采样周期: --"
            : "Sample Period: --");
    return;
  }

  double period = 1.0 / samplerate;
  QString unit = "s";
  double val = period;
  if (period < 1.0) {
    if (period >= 1e-3) {
      val = period * 1e3;
      unit = "ms";
    } else if (period >= 1e-6) {
      val = period * 1e6;
      unit = "us";
    } else if (period >= 1e-9) {
      val = period * 1e9;
      unit = "ns";
    } else if (period >= 1e-12) {
      val = period * 1e12;
      unit = "ps";
    } else {
      val = period * 1e15;
      unit = "fs";
    }
  }

  QString val_str = QString::number(val, 'f', 4);
  if (val_str.contains('.')) {
    while (val_str.endsWith('0')) {
      val_str.chop(1);
    }
    if (val_str.endsWith('.')) {
      val_str.chop(1);
    }
  }

  QString prefix = (AppConfig::Instance().frameOptions.language == LAN_CN)
                       ? "采样周期: "
                       : "Sample Period: ";
  _sample_period_label->setText(prefix + val_str + " " + unit);
}

} // namespace pv
