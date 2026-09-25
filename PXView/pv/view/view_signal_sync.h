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

#ifndef PXVIEW_PV_VIEW_VIEW_SIGNAL_SYNC_H
#define PXVIEW_PV_VIEW_VIEW_SIGNAL_SYNC_H

#include <cstdint>
#include <memory>
#include <vector>
#include <QColor>

#include "pv/view/iview_delegates.h"

class QColor;
class QString;
class QRect;

namespace pv {

namespace data {
struct SignalConfig;
} // namespace data

namespace view {

class View;
class Trace;
class Signal;

struct SignalGroup;

// ViewSignalSync — delegate for View's signal-group / signal-rebuild /
// signals-changed layout responsibilities. Extracted from the View
// God-class during Phase J of the modernize-view-layer-v3 spec. Signal
// state (_own_signals / _signal_groups / _rebuild_in_progress /
// _group_card_color) lives here. Signal height state (_spanY /
// _signalHeight / _signalHeightScale) lives on ViewLayout (migrated
// from View in Phase 1). View declares `friend class ViewSignalSync;`
// so the delegate can access View's private widget members and layout state.
class ViewSignalSync : public IViewSignalStore {
public:
  explicit ViewSignalSync(View *view);
  ~ViewSignalSync();

  // -- signal grouping ---------------------------------------------------
  void compute_signal_groups();

  // -- full layout refresh ----------------------------------------------
  void signals_changed(const Trace *eventTrace);

  // -- signal rebuild ----------------------------------------------------
  void rebuild_signals_from_config(const pv::data::SignalConfig &config);
  void rebuild_signals();

  // -- incremental signal-change events --------------------------------
  void on_signals_changed();
  void signals_added_layout();
  void signals_removed_layout();
  void signals_modified_refresh();

  // -- trace access (Phase J additional) --------------------------------
  void get_traces(int type, std::vector<Trace *> &traces);
  static bool compare_trace_v_offsets(const Trace *a, const Trace *b);
  static bool compare_trace_view_index(const Trace *a, const Trace *b);
  static bool compare_trace_y(const Trace *a, const Trace *b);

  // -- layout (Phase J additional) --------------------------------------
  void normalize_layout();
  void zoom_vertical(double steps);
  int headerWidth();

  /**
   * Slide the non-dragged traces aside while a channel drag is in progress.
   *
   * This is the "让位" (make-way) half of the drag-reorder UX: the dragged
   * trace follows the cursor with no animation (Header calls
   * Trace::force_to_v_offset), while every *other* visible trace is given a
   * new layout target — the position it would occupy if the drag were
   * released now — and animates towards it over ~100ms.
   *
   * Deliberately does NOT touch view_index, the group structure, or
   * persistence: those stay the release-time responsibility of
   * Header::mouseReleaseEvent (which re-sorts by Y and saves the layout).
   * Keeping the preview purely arithmetic means the animation can never
   * corrupt the ordering that gets written to the document.
   *
   * @param dragged  Trace currently under the cursor (must not be animated;
   *                 pass nullptr to animate every visible trace instead).
   * @return true if any trace was given a new target (i.e. a repaint is due).
   */
  bool animate_make_way_for_drag(Trace *dragged);

  // -- theme / colors (Phase J additional) ------------------------------
  void UpdateTheme();
  QColor get_group_card_color();
  QColor get_group_card_color(int group_index);
  QColor get_trace_card_color(Trace *trace);
  bool is_colored_card_mode();

  std::vector<std::unique_ptr<Signal>> &own_signals() { return _own_signals; }
  std::vector<SignalGroup> &signal_groups() { return _signal_groups; }
  QColor group_card_color() const { return _group_card_color; }
  void set_group_card_color(QColor c) { _group_card_color = c; }
  bool rebuild_in_progress() const override { return _rebuild_in_progress; }
  void set_rebuild_in_progress(bool v) { _rebuild_in_progress = v; }

  // -- IViewSignalStore overrides ----------------------------------------
  size_t signal_count() const override { return _own_signals.size(); }

private:
  View *_view;

  std::vector<std::unique_ptr<Signal>> _own_signals;
  std::vector<SignalGroup> _signal_groups;
  QColor _group_card_color;
  bool _rebuild_in_progress = false;

  // --- signals_changed() split helpers (was 300-line God-method) ---

  void normalize_view_indices();

  // 不变量校验：view_index 必须构成 0..n-1 的连续序列（无重复、无空洞），
  // 且每个 group 内的 view_index 必须连续。
  //
  // 为什么需要显式校验：`_signal_groups` 是从 `view_index` 派生出来的**第二次
  // 独立遍历**，两者靠"normalize_view_indices() 先于 compute_signal_groups()
  // 调用"这个**约定**保持一致，而不是靠类型系统保证。compute_signal_groups()
  // 的注释自证了该契约，且历史上已踩过坑（重复 view_index → std::sort 不稳定
  // → 通道顺序错乱）。任何新增路径若直接改 view_index 却不重跑归一化，就会
  // 静默重现该 bug。本函数把这个隐式约定变成显式校验（Release 下仅记日志，
  // 见 pxv_assert 的 NDEBUG 分支）。
  //
  // 只在 logic 渲染模式（分组生效）时校验分组连续性。
  bool validate_view_index_invariants() const;
  void classify_traces(std::vector<Trace *> &time_traces,
                        std::vector<Trace *> &fft_traces,
                        std::vector<Trace *> &logic_traces,
                        std::vector<Trace *> &decoder_traces);
  void update_fft_viewport(const std::vector<Trace *> &fft_traces);
  void layout_time_signals(std::vector<Trace *> &time_traces,
                            const std::vector<Trace *> &logic_traces,
                            const std::vector<Trace *> &decoder_traces);
  void finalize_signal_layout();
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_SIGNAL_SYNC_H
