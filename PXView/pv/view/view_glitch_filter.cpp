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

// Phase J (modernize-view-layer-v3): glitch-filter popup / preview / apply
// / undo behaviour extracted from the View God-class. ViewGlitchFilter is
// declared a friend of View so it can touch the private glitch-filter
// state (_glitch_filter_popup / _preview_ranges / _filter_undo_stack /
// _own_signals / _time_viewport / _header) directly. Cross-method calls
// that remain on View (e.g. session, update) go through _view->… so the
// public View API is unchanged. tr() is invoked as View::tr() to keep the
// original translation context; Toast::show parent is _view (a QWidget*).

#include "view_glitch_filter.h"

#include <algorithm>
#include <vector>

#include <QCursor>
#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>

#include "view.h"

#include "../data/decode/decoder.h"
#include "../data/decoderstack.h"
#include "../data/pulse_analyzer.h"
#include "../pxvdef.h"
#include "../sigsession.h"
#include "../ui/toast.h"

#include "decodetrace.h"
#include "glitchfilterpopup.h"
#include "header.h"
#include "logicsignal.h"
#include "signal.h"
#include "viewport.h"

using namespace std;

namespace pv {
namespace view {

ViewGlitchFilter::ViewGlitchFilter(View *view) : _view(view) {}

ViewGlitchFilter::~ViewGlitchFilter() = default;

void ViewGlitchFilter::set_glitch_filter_popup(GlitchFilterPopup *p) {
  _glitch_filter_popup.reset(p);
}

void ViewGlitchFilter::on_show_glitch_filter_popup(
    pv::view::LogicSignal *sig) {
  if (!sig)
    return;
  if (!_glitch_filter_popup)
    return;

  // 对齐 HTML 原型:弹窗紧贴 name 区右侧 (label-row.right + 8),
  // 垂直方向对齐轨道上界(sig->get_y() - totalHeight/2)。
  int name_right = _view->header_widget()
                       ? _view->header_widget()->width() - sig->get_rightWidth()
                       : 0;
  int anchor_x = name_right + 8;
  int anchor_y = sig->get_y() - sig->get_totalHeight() / 2;
  QPoint anchor = _view->header_widget()
                      ? _view->header_widget()->mapToGlobal(QPoint(anchor_x, anchor_y))
                      : QCursor::pos();

  // Keep the popup on screen (assume ~420x500 popup size).
  QScreen *screen = QGuiApplication::screenAt(anchor);
  if (screen) {
    QRect geo = screen->availableGeometry();
    if (anchor.x() + 420 > geo.right())
      anchor.setX(geo.right() - 420);
    if (anchor.y() + 500 > geo.bottom())
      anchor.setY(geo.bottom() - 500);
    if (anchor.x() < geo.left())
      anchor.setX(geo.left());
    if (anchor.y() < geo.top())
      anchor.setY(geo.top());
  }

  _glitch_filter_popup->open_for_signal(sig, anchor);
}

void ViewGlitchFilter::on_clear_glitch_filter_requested(bool all_channels) {
  // Core's clear_glitch_filter() is global (clears all channels); the
  // all_channels flag only affects the toast message. A per-channel clear
  // would require a Core API extension.
  _view->session().clear_glitch_filter();
  _preview_ranges.clear();
  if (_view->get_time_view())
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  pv::ui::Toast::show(_view,
                      all_channels ? View::tr("已清除所有通道滤波")
                                   : View::tr("已清除通道滤波"),
                      pv::ui::Toast::Info);
}

void ViewGlitchFilter::on_toggle_invert_requested(
    pv::view::LogicSignal *sig) {
  if (!sig)
    return;

  auto &sess = _view->session();

  // SigSession exposes no per-channel invert getter, so a true per-channel
  // toggle is not possible in this version. Toggle at the session level:
  // if any invert is active, clear all; otherwise apply invert to the
  // target channel only.
  if (sess.is_signal_invert_active()) {
    sess.clear_signal_invert();
    pv::ui::Toast::show(_view, View::tr("已清除信号取反"), pv::ui::Toast::Info);
    if (_view->get_time_view())
      _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
    return;
  }

  // Build the channels vector indexed by enabled-logic-channel ordinal.
  std::vector<LogicSignal *> logic_sigs;
  for (auto &s : _view->get_own_signals()) {
    if (auto *logic = s->as_logic())
      logic_sigs.push_back(logic);
  }
  std::vector<bool> channels(logic_sigs.size(), false);
  auto it = std::find(logic_sigs.begin(), logic_sigs.end(), sig);
  if (it == logic_sigs.end())
    return;
  channels[std::distance(logic_sigs.begin(), it)] = true;

  sess.set_signal_invert(channels);
  pv::ui::Toast::show(_view,
                      View::tr("已对通道 %1 取反").arg(sig->get_name()),
                      pv::ui::Toast::Info);
  if (_view->get_time_view())
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
}

void ViewGlitchFilter::on_glitch_preview_changed(pv::view::LogicSignal *sig,
                                                  uint32_t threshold,
                                                  GlitchFilterMode mode) {
  if (!sig)
    return;
  auto *snap = sig->data();
  if (!snap)
    return;
  int sig_index = sig->model() ? sig->model()->index() : 0;
  auto pulses = pv::data::PulseAnalyzer::find_pulses(snap, sig_index);
  _preview_ranges[sig] =
      pv::data::PulseAnalyzer::preview_filter(pulses, threshold, mode);
  if (_view->get_time_view())
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
}

void ViewGlitchFilter::on_glitch_apply_requested(
    pv::view::LogicSignal *sig, uint32_t threshold, GlitchFilterMode mode,
    bool all_channels) {
  if (!sig)
    return;
  auto &sess = _view->session();

  // Push the current state onto the undo stack (Task 9 / I4). Capture the
  // prior thresholds/modes BEFORE the new apply so undo_filter() can restore
  // the exact previous state instead of always clearing.
  FilterSnapshot snap;
  snap.was_active = sess.is_glitch_filter_active();
  if (snap.was_active) {
    snap.thresholds = sess.glitch_filter_thresholds();
    snap.modes = sess.glitch_filter_modes();
  }
  _filter_undo_stack.push_back(snap);
  if (_filter_undo_stack.size() > 20)
    _filter_undo_stack.erase(_filter_undo_stack.begin());

  // 架构修复：用 channel_index 作 key，与 _ch_index 中的位置无关。
  // 即使 View 层包含禁用通道，Core 层也只处理 _ch_index 中的通道。
  std::map<int, uint32_t> thresholds;
  std::map<int, GlitchFilterMode> modes;

  if (all_channels) {
    // "应用到所有通道"：用新阈值替换全部，不合并已有状态
    for (auto &s : _view->get_own_signals()) {
      if (s && s->signal_type() == SR_CHANNEL_LOGIC) {
        int ch_idx = s->model() ? s->model()->index() : -1;
        if (ch_idx >= 0) {
          thresholds[ch_idx] = threshold;
          modes[ch_idx] = mode;
        }
      }
    }
  } else {
    // 单通道应用：先继承已有的 thresholds/modes，再覆盖目标通道
    // 这样 Core 层从 backup 恢复后会重新对所有已滤波通道执行滤波，
    // 避免新通道滤波导致旧通道滤波效果丢失
    if (sess.is_glitch_filter_active()) {
      thresholds = sess.glitch_filter_thresholds();
      modes = sess.glitch_filter_modes();
    }
    int ch_idx = sig->model() ? sig->model()->index() : -1;
    if (ch_idx >= 0) {
      thresholds[ch_idx] = threshold;
      modes[ch_idx] = mode;
    }
  }

  sess.set_glitch_filter(thresholds, modes);

  // Clear preview overlay once the real filter is applied.
  _preview_ranges.clear();

  if (all_channels) {
    pv::ui::Toast::show(
        _view,
        View::tr("已对所有逻辑通道应用滤波 (阈值 %1)").arg(threshold),
        pv::ui::Toast::Info);
  } else {
    pv::ui::Toast::show(_view,
                        View::tr("已对通道 %1 应用滤波 (阈值 %2)")
                            .arg(sig->get_name())
                            .arg(threshold),
                        pv::ui::Toast::Info);
  }
}

void ViewGlitchFilter::on_glitch_popup_closed() {
  _preview_ranges.clear();
  if (_view->get_time_view()) {
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
    _view->get_time_view()->setFocus();
  }
}

void ViewGlitchFilter::on_apply_batch_requested(
    const std::vector<pv::view::LogicSignal *> &sigs, uint32_t threshold,
    GlitchFilterMode mode) {
  if (sigs.empty())
    return;
  auto &sess = _view->session();

  // Push undo snapshot (与单通道 apply 一致的撤销栈逻辑)
  FilterSnapshot snap;
  snap.was_active = sess.is_glitch_filter_active();
  if (snap.was_active) {
    snap.thresholds = sess.glitch_filter_thresholds();
    snap.modes = sess.glitch_filter_modes();
  }
  _filter_undo_stack.push_back(snap);
  if (_filter_undo_stack.size() > 20)
    _filter_undo_stack.erase(_filter_undo_stack.begin());

  // 架构修复：用 channel_index 作 key，与 _ch_index 中的位置无关
  std::map<int, uint32_t> thresholds;
  std::map<int, GlitchFilterMode> modes;

  // 批处理应用：先继承已有的 thresholds/modes，再覆盖 batch 中的通道
  // 避免 batch 中未包含的已滤波通道丢失滤波效果
  if (sess.is_glitch_filter_active()) {
    thresholds = sess.glitch_filter_thresholds();
    modes = sess.glitch_filter_modes();
  }

  // 对 batch 中每个 sig 设置对应 channel_index 的 threshold/mode
  for (auto *sig : sigs) {
    if (!sig)
      continue;
    int ch_idx = sig->model() ? sig->model()->index() : -1;
    if (ch_idx >= 0) {
      thresholds[ch_idx] = threshold;
      modes[ch_idx] = mode;
    }
  }

  sess.set_glitch_filter(thresholds, modes);
  _preview_ranges.clear();

  QString desc;
  if (sigs.size() == 1) {
    desc = View::tr("已对通道 %1 应用滤波 (阈值 %2)")
               .arg(sigs[0]->get_name())
               .arg(threshold);
  } else {
    desc = View::tr("已对 %1 个子通道应用滤波 (阈值 %2)")
               .arg((int)sigs.size())
               .arg(threshold);
  }
  pv::ui::Toast::show(_view, desc, pv::ui::Toast::Info);
}

void ViewGlitchFilter::on_preview_batch_changed(
    const std::vector<pv::view::LogicSignal *> &sigs, uint32_t threshold,
    GlitchFilterMode mode) {
  // 为 batch 中每个 sig 更新预览 overlay
  for (auto *sig : sigs) {
    if (!sig)
      continue;
    auto *snap = sig->data();
    if (!snap)
      continue;
    int sig_index = sig->model() ? sig->model()->index() : 0;
    auto pulses = pv::data::PulseAnalyzer::find_pulses(snap, sig_index);
    _preview_ranges[sig] =
        pv::data::PulseAnalyzer::preview_filter(pulses, threshold, mode);
  }
  if (_view->get_time_view())
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
}

const std::vector<pv::data::PulseAnalyzer::Pulse> *
ViewGlitchFilter::get_preview_ranges(LogicSignal *sig) const {
  if (!sig)
    return nullptr;
  auto it = _preview_ranges.find(sig);
  if (it == _preview_ranges.end())
    return nullptr;
  return &it->second;
}

void ViewGlitchFilter::undo_filter() {
  if (_filter_undo_stack.empty())
    return;
  auto &sess = _view->session();
  FilterSnapshot snap = _filter_undo_stack.back();
  _filter_undo_stack.pop_back();
  // I4: restore the prior state captured at apply time. If the filter was
  // active before the now-undone apply, re-apply the previous thresholds/
  // modes; otherwise clear the filter entirely.
  if (snap.was_active) {
    sess.set_glitch_filter(snap.thresholds, snap.modes);
  } else {
    sess.clear_glitch_filter();
  }
  _preview_ranges.clear();
  if (_view->get_time_view())
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  pv::ui::Toast::show(_view, View::tr("已撤销滤波"), pv::ui::Toast::Info);
}

void ViewGlitchFilter::on_glitch_filter_completed() {
  // FilterProcessor completed a glitch filter application. If the popup is
  // currently open, refresh its histogram so it reflects the filtered
  // LogicSnapshot (filtered pulses are now long pulses; previously-filtered
  // short pulses are gone). Also clear stale preview ranges — the red
  // overlay from get_filtered_ranges() now shows the actual filtered state.
  _preview_ranges.clear();
  if (_view->get_time_view())
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  if (_glitch_filter_popup && _glitch_filter_popup->is_open())
    _glitch_filter_popup->on_filter_completed();
}

void ViewGlitchFilter::on_glitch_filter_cleared() {
  // FilterProcessor cleared the glitch filter. Refresh the popup's histogram
  // so it reflects the unfiltered LogicSnapshot, and clear preview ranges so
  // the orange overlay disappears (red overlay already gone because
  // get_filtered_ranges() returns empty after clear_filtered_ranges()).
  _preview_ranges.clear();
  if (_view->get_time_view())
    _view->get_time_view()->update(UpdateEventType::UPDATE_EV_GENERIC);
  if (_glitch_filter_popup && _glitch_filter_popup->is_open())
    _glitch_filter_popup->on_filter_cleared();
}

} // namespace view
} // namespace pv
