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

#ifndef PXVIEW_PV_VIEW_VIEW_GLITCH_FILTER_H
#define PXVIEW_PV_VIEW_VIEW_GLITCH_FILTER_H

#include <cstdint>
#include <vector>

#include "../data/logicsnapshot.h"  // GlitchFilterMode
#include "../data/pulse_analyzer.h"

namespace pv {
namespace view {

class View;
class LogicSignal;
class DecodeTrace;

// ViewGlitchFilter — delegate for View's glitch-filter popup / preview /
// apply / undo responsibilities. Extracted from the View God-class during
// Phase J of the modernize-view-layer-v3 spec. All glitch-filter state
// (_glitch_filter_popup / _preview_ranges / _filter_undo_stack) still
// lives on View; this class only owns the *behaviour*. View declares
// `friend class ViewGlitchFilter;` so the delegate can read and mutate
// those private members directly.
class ViewGlitchFilter {
public:
  explicit ViewGlitchFilter(View *view) : _view(view) {}

  // -- popup lifecycle ---------------------------------------------------
  void on_show_glitch_filter_popup(pv::view::LogicSignal *sig);
  void on_glitch_popup_closed();

  // -- filter apply / clear / invert ------------------------------------
  void on_clear_glitch_filter_requested(bool all_channels);
  void on_toggle_invert_requested(pv::view::LogicSignal *sig);
  void on_glitch_apply_requested(pv::view::LogicSignal *sig,
                                 uint32_t threshold,
                                 GlitchFilterMode mode, bool all_channels);
  // 批量模式:对一组逻辑通道统一应用/预览滤波(如 DecodeTrace 根解码器绑定的子通道)
  void on_apply_batch_requested(const std::vector<pv::view::LogicSignal *> &sigs,
                                uint32_t threshold, GlitchFilterMode mode);

  // -- preview overlay ---------------------------------------------------
  void on_glitch_preview_changed(pv::view::LogicSignal *sig,
                                 uint32_t threshold,
                                 GlitchFilterMode mode);
  void on_preview_batch_changed(const std::vector<pv::view::LogicSignal *> &sigs,
                                uint32_t threshold, GlitchFilterMode mode);
  const std::vector<pv::data::PulseAnalyzer::Pulse> *
  get_preview_ranges(LogicSignal *sig) const;

  // -- undo --------------------------------------------------------------
  void undo_filter();

  // -- filter completion / clearing notifications -----------------------
  void on_glitch_filter_completed();
  void on_glitch_filter_cleared();

private:
  View *_view;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_GLITCH_FILTER_H
