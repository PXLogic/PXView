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

#include "pv/view/signal/signalfactory.h"

#include <algorithm>
#include <set>

#include <QColor>
#include <QString>

#include "pv/view/signal/analogsignal.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/signal/signal.h"

#include "pv/api/types.h"
#include "pv/data/datasource.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/model/signalmodel.h"
#include "pv/session/deviceagent.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"

namespace pv {
namespace view {

/**
 * Apply properties (name, color, enabled, visible) from a SignalModel to
 * an already-constructed view::Signal.
 */
static void apply_model_properties(Signal *signal,
                                   std::shared_ptr<data::SignalModel> model) {
  if (!signal || !model)
    return;

  if (!model->name().empty())
    signal->set_name(QString::fromStdString(model->name()));

  if (!model->color().empty())
    signal->set_colour(QColor(QString::fromStdString(model->color())));

  signal->set_enabled(model->enabled());
  signal->set_visible(model->enabled());

  if (auto *logic_sig = signal->as_logic()) {
    logic_sig->set_trig(model->trig_type());
    // Establish live sync: subsequent SignalModel::set_trig_type() calls
    // will auto-update this LogicSignal's _trig via Qt signal/slot.
    // UniqueConnection prevents duplicate connections when
    // apply_model_properties is called again (e.g. via
    // update_signals(Modified)). Connection is auto-disconnected when either
    // object is destroyed.
    QObject::connect(model.get(), &data::SignalModel::trig_type_changed,
                     logic_sig, &LogicSignal::set_trig, Qt::UniqueConnection);
  }
}

Signal *SignalFactory::create_signal(std::shared_ptr<data::SignalModel> model,
                                     data::DataSource *data_source) {
  if (!model || !data_source) {
    return nullptr;
  }

  Signal *signal = nullptr;
  switch (model->type()) {
  case SR_CHANNEL_LOGIC:
    signal = new LogicSignal(get_logic_snapshot(data_source), model, data_source);
    break;
  case SR_CHANNEL_ANALOG:
    signal = new AnalogSignal(get_analog_snapshot(data_source), model, data_source);
    break;
  case SR_CHANNEL_DSO:
    signal = new DsoSignal(get_dso_snapshot(data_source), model, data_source);
    break;
  default:
    pxv_warn("create_signal: UNKNOWN type=%d (index=%d), returning nullptr",
             model->type(), model->index());
    return nullptr;
  }

  apply_model_properties(signal, model);
  return signal;
}

std::vector<std::unique_ptr<Signal>> SignalFactory::create_signals(data::DataSource *source,
                                                    data::DataSource *data_source) {
  std::vector<std::unique_ptr<Signal>> result;
  if (!source || !data_source)
    return result;

  auto models = source->get_signal_models_snapshot();
  result.reserve(models.size());
  for (auto model : models) {
    Signal *s = create_signal(model, data_source);
    if (s)
      result.push_back(std::unique_ptr<Signal>(s));
  }
  return result;
}

SignalFactory::SignalChangeEvent SignalFactory::compute_change_event(
    const std::vector<std::unique_ptr<Signal>> &current_signals,
    const std::vector<std::shared_ptr<data::SignalModel>> &models) {
  // Empty current + non-empty models → first creation → AllReplaced
  if (current_signals.empty() && !models.empty())
    return AllReplaced;

  // Non-empty current + empty models → all removed → AllReplaced
  if (!current_signals.empty() && models.empty())
    return AllReplaced;

  // Both empty → no change, but Modified is safe fallback
  if (current_signals.empty() && models.empty())
    return Modified;

  // Build sets of channel indices
  std::set<int> current_indices;
  for (auto &sig : current_signals) {
    if (sig)
      current_indices.insert(sig->get_index());
  }

  std::set<int> model_indices;
  for (auto &model : models) {
    if (model)
      model_indices.insert(model->index());
  }

  // Check if index sets are identical → Modified (properties may have changed)
  if (current_indices == model_indices) {
    // Pointer-identity check: even when channel indices match, if any existing
    // Signal's _model points to a SignalModel object that is NOT in the new
    // models list (by raw pointer identity), Core has rebuilt the SignalModels
    // wholesale (init_signals/reload/switch_work_mode). The shared_ptr stored
    // in view::Signal keeps the old SignalModel alive so this .get() read is
    // safe, but the View must fully rebuild (AllReplaced) so each Signal
    // rebinds its _model to the new SignalModel. Without this, the stale
    // _model causes UAF (this=0xfeeefeeefeeefeee) when handlers later access it
    // (e.g. DsoSignal::set_zero_ratio -> SignalModel::set_zero_offset).
    std::set<data::SignalModel *> model_ptrs;
    for (auto &model : models) {
      if (model)
        model_ptrs.insert(model.get());
    }
    for (auto &sig : current_signals) {
      if (!sig)
        continue;
      auto sig_model = sig->model();
      if (sig_model && model_ptrs.find(sig_model.get()) == model_ptrs.end()) {
        return AllReplaced;
      }
    }
    return Modified;
  }

  // Check if models is pure superset of current → Added
  // (all current indices exist in models, and models has extra indices)
  bool all_current_in_models = true;
  bool some_new_not_in_current = false;
  for (int idx : current_indices) {
    if (model_indices.find(idx) == model_indices.end()) {
      all_current_in_models = false;
      break;
    }
  }
  for (int idx : model_indices) {
    if (current_indices.find(idx) == current_indices.end()) {
      some_new_not_in_current = true;
      break;
    }
  }

  if (all_current_in_models && some_new_not_in_current)
    return Added;

  // Check if current is pure superset of models → Removed
  // (all model indices exist in current, and current has extra indices)
  bool all_models_in_current = true;
  bool some_current_not_in_models = false;
  for (int idx : model_indices) {
    if (current_indices.find(idx) == current_indices.end()) {
      all_models_in_current = false;
      break;
    }
  }
  for (int idx : current_indices) {
    if (model_indices.find(idx) == model_indices.end()) {
      some_current_not_in_models = true;
      break;
    }
  }

  if (all_models_in_current && some_current_not_in_models)
    return Removed;

  // Mixed: both additions and removals → conservative fallback to AllReplaced
  return AllReplaced;
}

void SignalFactory::update_signals(std::vector<std::unique_ptr<Signal>> &current_signals,
                                   data::DataSource *source,
                                   data::DataSource *data_source,
                                   SignalChangeEvent event) {
  if (!source || !data_source) {
    if (event == AllReplaced) {
      current_signals.clear();
    }
    return;
  }

  auto models = source->get_signal_models_snapshot();

  switch (event) {
  case AllReplaced: {
    // Save UI state from existing signals, recreate all, restore state.
    std::map<int, SignalUiState> saved_state = save_ui_state(current_signals);

    // unique_ptr auto-deletes all Signal elements on clear().
    current_signals.clear();

    current_signals = create_signals(source, data_source);

    // R8: restore_ui_state restores UI state saved from the OLD Signal objects.
    // This preserves the user's custom layout (view_index/v_offset/own_height)
    // across model rebuilds triggered by reload().
    restore_ui_state(current_signals, saved_state);

    // If there is a SessionDocument with a valid SignalConfig, apply its
    // persisted layout state to override the saved_state. This ensures that
    // layouts persisted via .pxc files are restored even if the old Signal
    // objects had default layouts (e.g., after a previous capture reset).
    auto *doc = source->get_active_document();
    if (doc && doc->get_signal_config().is_valid) {
      const auto &cfg = doc->get_signal_config();
      for (auto &sig : current_signals) {
        auto it = std::find_if(cfg.channels.begin(), cfg.channels.end(),
                               [&](const data::ChannelConfig &ch) {
                                 return ch.index == sig->get_index();
                               });
        if (it != cfg.channels.end()) {
          if (it->view_index >= 0)
            sig->set_view_index(it->view_index);
          sig->set_v_offset(it->v_offset);
          if (it->own_height >= 0)
            sig->set_own_height(it->own_height);
        }
      }
    }

    break;
  }

  case Added: {
    // Create signals for models that have no matching signal yet.
    for (auto model : models) {
      bool exists = false;
      for (auto &s : current_signals) {
        if (s->get_index() == model->index()) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        Signal *new_sig = create_signal(model, data_source);
        if (new_sig)
          current_signals.push_back(std::unique_ptr<Signal>(new_sig));
      }
    }
    break;
  }

  case Removed: {
    // Remove signals whose index no longer exists in the model list.
    // unique_ptr auto-deletes the Signal when the element is erased.
    current_signals.erase(
        std::remove_if(current_signals.begin(), current_signals.end(),
            [&](std::unique_ptr<Signal> &s) {
                for (auto model : models) {
                    if (model->index() == s->get_index())
                        return false;  // keep
                }
                return true;  // remove
            }),
        current_signals.end());
    break;
  }

  case Modified: {
    // Refresh properties of existing signals from the models.
    for (auto &s : current_signals) {
      for (auto model : models) {
        if (model->index() != s->get_index())
          continue;
        apply_model_properties(s.get(), model);
        break;
      }
    }
    break;
  }
  }
}

std::map<int, SignalFactory::SignalUiState>
SignalFactory::save_ui_state(const std::vector<std::unique_ptr<Signal>> &sig_list) {
  std::map<int, SignalUiState> state;
  for (auto &s : sig_list) {
    if (!s)
      continue;
    SignalUiState ui;
    ui.channel_index = s->get_index();
    ui.selected = s->selected();
    ui.visible = s->visible();
    ui.view_index = s->get_view_index();
    ui.v_offset = s->get_v_offset();
    ui.own_height = s->get_own_height();
    state[ui.channel_index] = ui;
  }
  return state;
}

void SignalFactory::restore_ui_state(
    std::vector<std::unique_ptr<Signal>> &sig_list,
    const std::map<int, SignalUiState> &saved_state) {
  for (auto &s : sig_list) {
    if (!s)
      continue;
    auto it = saved_state.find(s->get_index());
    if (it == saved_state.end())
      continue;
    const SignalUiState &ui = it->second;
    s->select(ui.selected);
    s->set_visible(ui.visible);
    s->set_view_index(ui.view_index);
    s->set_v_offset(ui.v_offset);
    s->set_own_height(ui.own_height);
  }
}

data::LogicSnapshot *SignalFactory::get_logic_snapshot(data::DataSource *data_source) {
  if (!data_source)
    return nullptr;
  return data_source->get_logic_snapshot();
}

data::AnalogSnapshot *SignalFactory::get_analog_snapshot(data::DataSource *data_source) {
  if (!data_source)
    return nullptr;
  return data_source->get_analog_snapshot();
}

data::DsoSnapshot *SignalFactory::get_dso_snapshot(data::DataSource *data_source) {
  if (!data_source)
    return nullptr;
  return data_source->get_dso_snapshot();
}

} // namespace view
} // namespace pv
