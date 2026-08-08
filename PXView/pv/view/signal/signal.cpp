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

#include "pv/view/signal/signal.h"
#include "pv/data/datasource.h"
#include "pv/data/model/signalmodel.h"
#include "pv/base/pxvdef.h"
#include "pv/session/sigsession.h"
#include "pv/view/view.h"
#include <cmath>

namespace pv {
namespace view {

Signal::Signal(std::shared_ptr<data::SignalModel> model, data::DataSource *data_source)
    : Trace(QString::fromStdString(model ? model->name() : std::string()),
            static_cast<uint16_t>(model ? model->index() : 0),
            model ? model->type() : SR_CHANNEL_LOGIC),
      _model(model), _data_source(data_source) {
  // Establish Qt signal connections directly from _model — no need to
  // query the session for the model by index (the model is injected).
  if (_model) {
    connect(_model.get(), &data::SignalModel::appearance_changed, this,
            &Signal::on_appearance_changed);
    connect(_model.get(), &data::SignalModel::visibility_changed, this,
            &Signal::on_visibility_changed);
  }
}

Signal::Signal(const Signal &s, std::shared_ptr<data::SignalModel> model,
               data::DataSource *data_source)
    : Trace((const Trace &)s), _model(model), _data_source(data_source),
      _local_enabled(s._local_enabled) {
  if (_model) {
    connect(_model.get(), &data::SignalModel::appearance_changed, this,
            &Signal::on_appearance_changed);
    connect(_model.get(), &data::SignalModel::visibility_changed, this,
            &Signal::on_visibility_changed);
  }
}

bool Signal::enabled() { return _local_enabled; }

void Signal::set_enabled(bool en) {
  _local_enabled = en;
  // R2: 实时写回 Core (sr_channel->enabled)，让 SigSession::reload() 重建
  // SignalModel 时能读到正确的 enabled 状态。
  // 不在此处广播 DeviceOptionsUpdated: MainWindow::on_event 收到该
  // 消息会调 rebuild_signals() -> apply_model_properties() -> set_enabled()，
  // 形成无限循环。广播由用户交互入口负责（如 DeviceOptionsDock 已有广播）。
  // Task 6.1: 同步写回 Core SignalModel->enabled，保证 headless API
  // 读取到最新状态。 不广播：由调用方（用户交互入口）负责广播，避免 rebuild
  // 循环。 SignalModel::set_enabled() handles the write-back to
  // sr_channel->enabled and to libsigrok via DeviceAgent.
  if (_model)
    _model->set_enabled(en);
}

void Signal::set_name(QString name) {
  Trace::set_name(name);
  // Phase 2: delegate to the Core SignalModel setter, which handles writing
  // back to sr_channel->name (g_free/g_strdup) and emitting appearance_changed.
  if (_model)
    _model->set_name(name.toStdString());
}

void Signal::set_colour(QColor colour) {
  Trace::set_colour(colour);
  if (_model)
    _model->set_color(colour.name().toStdString());
}

void Signal::on_appearance_changed() {
  if (_view) {
    _view->update();
    _view->header_updated();
  }
}

void Signal::on_visibility_changed() {
  if (_view)
    _view->signals_changed(this);
}

} // namespace view
} // namespace pv
