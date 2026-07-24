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

#ifndef PXVIEW_PV_VIEW_VIEW_H
#define PXVIEW_PV_VIEW_VIEW_H

#include <list>
#include <map>
#include <memory>
#include <stdint.h>
#include <vector>

#include <QElapsedTimer>
#include <QScrollArea>
#include <QTimer>

#include "../data/pulse_analyzer.h"
#include "../ui/uimanager.h"
#include "dock_ui_state.h"
#include "view_cursors.h"
#include "view_glitch_filter.h"

// Forward declarations (replaces the former includes of signal.h, viewport.h,
// cursor.h, xcursor.h, viewstatus.h, view_derived_traces.h, view_layout.h,
// datasource.h, signaldata.h, samplingbar.h, icallbacks.h
// — Phase H header hygiene; Phase K additionally forward-declares the
// unique_ptr delegate types ViewLayout / ViewDataSync /
// ViewDerivedTraces / ViewSignalSync and the value-type QSizeF used only
// as a static data member declaration).
// Pointers/references to these types are used as members/function params
// only, so forward declarations suffice; the full definitions are pulled in
// by view.cpp (and other consumers) as needed. unique_ptr members require
// the complete type only at the point of View's destructor definition
// (defined out-of-line in view.cpp).
QT_BEGIN_NAMESPACE
class QPaintEvent;
class QSizeF;
class QSplitter;
QT_END_NAMESPACE

struct srd_decoder;
class DecoderStatus;
class DeviceAgent;

namespace pv {

namespace toolbars {
class SamplingBar;
}

namespace dialogs {
class Lissajous;
} // namespace dialogs

namespace data {
class DataSource;
class SessionDocument;
struct SignalConfig;
class DecoderStack;
namespace decode {
class Decoder;
} // namespace decode
} // namespace data

class SigSession;

namespace view {

class Header;
class DevMode;
class Ruler;
class Trace;
class Viewport;
class DecodeTrace;
class SpectrumTrace;
class MathTrace;
class LissajousTrace;
class GlitchFilterPopup;
class LogicSignal;
class Cursor;
class XCursor;
class ViewStatus;
class Signal;

// Phase K forward declarations for the unique_ptr delegate types. Full
// definitions live in view_layout.h / view_data_sync.h /
// view_derived_traces.h / view_signal_sync.h, which are
// included by view.cpp (and by other TUs that touch the delegate types
// directly). view_cursors.h is included directly above because Cursor /
// XCursor are used by too many TUs as complete types.
class ViewLayout;
class ViewDataSync;
class ViewDerivedTraces;
class ViewSignalSync;

struct SignalGroup {
  int group_id;
  std::vector<Trace *> traces;
  SignalGroup() : group_id(-1) {}
};

// created by MainWindow
class View : public QScrollArea, public IUiWindow {
  Q_OBJECT
  Q_PROPERTY(QColor groupCardColor READ get_group_card_color WRITE
                 set_group_card_color)

public:
  // ---- Public static constants ----
  static const int MinSignalHeight;
  static const int MaxSignalHeight;
  static const int GroupGap = 10;
  static const int GroupCardRadius = 6;

  // static const int SignalHeight;
  static const int SignalMargin;
  static const int SignalSnapGridSize;

  static const QColor CursorAreaColour;
  static const QSizeF LabelPadding;
  static const QString Unknown_Str;

  static const int WellSamplesPerPixel = 2048;
  static constexpr double MaxViewRate = 1.0;
  static const int MaxPixelsPerSample = 100;

  static const int StatusHeight = 20;
  static const int DsoStatusHeight = 55;

  static const int ForeAlpha = 200;
  static const int BackAlpha = 100;
  static QColor Red;
  static QColor Orange;
  static QColor Blue;
  static QColor Green;
  static QColor Purple;
  static QColor LightBlue;
  static QColor LightRed;

  // ---- Public static methods ----
  static void refreshSignalColors();
  static bool compare_trace_v_offsets(const Trace *a, const Trace *b);
  static bool compare_trace_view_index(const Trace *a, const Trace *b);
  static bool compare_trace_y(const Trace *a, const Trace *b);

  // ---- Construction ----
  explicit View(SigSession *session, pv::toolbars::SamplingBar *sampling_bar,
                QWidget *parent = 0);

  ~View();

  // ---- Data source / document binding ----
  void set_data_source(pv::data::DataSource *source);
  inline pv::data::DataSource* data_source() { return _data_source; }
  void set_data_document(pv::data::SessionDocument *doc);
  void clone_signals_for_document(pv::data::SessionDocument *doc);
  void set_signal_data_from_source(pv::data::DataSource *source);
  void clear_signal_data();

  /**
   * Returns the DataSource for snapshot data only. When the bound
   * SessionDocument has captured data (_document->has_data()), returns
   * _document so snapshot/samplerate/sampletime queries read the
   * per-tab captured data; otherwise falls back to _data_source (the
   * SigSession DataSource).
   *
   * SignalModels are NOT available through this — SessionDocument's
   * _signal_models is never populated. Use _data_source directly for
   * SignalModel access (e.g. SignalFactory::create_signals/update_signals).
   */
  data::DataSource *document_snapshot_source();

  // ---- Session / scale / offset accessors ----
  inline SigSession &session() { return *_session; }

  /**
   * Returns the view time scale in seconds per pixel.
   */
  inline double scale() { return _scale; }

  inline double get_minscale() { return _minscale; }

  inline double get_maxscale() { return _maxscale; }

  void auto_set_max_scale();

  /**
   * Returns the pixels offset of the left edge of the view
   */
  inline int64_t offset() { return _offset; }

  /**
   * trigger position fix
   */
  inline double trig_hoff() { return _trig_hoff; }

  inline void set_trig_hoff(double hoff) { _trig_hoff = hoff; }

  int64_t get_min_offset();
  int64_t get_max_offset();
  int64_t get_logic_lst_data_offset();

  void capture_init();

  // ---- Scale / offset mutation (delegate to ViewLayout) ----
  void zoom(double steps);
  bool zoom(double steps, int offset);

  /**
   * Sets the scale and offset.
   * @param scale The new view scale in seconds per pixel.
   * @param offset The view time offset in seconds.
   */
  void set_scale_offset(double scale, int64_t offset);
  void limit_scale_offset();
  void set_preScale_preOffset();

  // ---- Traces access ----
  void get_traces(int type, std::vector<Trace *> &traces);

  // ---- Cursor visibility ----
  /**
   * Returns true if cursors are displayed. false otherwise.
   */
  inline bool cursors_shown() { return _show_cursors; }

  inline bool trig_cursor_shown() { return _show_trig_cursor; }

  inline bool search_cursor_shown() { return _show_search_cursor; }

  /**
   * Shows or hides the cursors.
   */
  void show_cursors(bool show = true);

  inline const QPoint &hover_point() { return _hover_point; }

  void normalize_layout();

  void show_trig_cursor(bool show = true);

  void show_search_cursor(bool show = true);

  // ---- Vertical layout ----
  inline int get_spanY() { return _spanY; }

  inline int get_signalHeight() { return _signalHeight; }

  inline int get_vOffset() { return _vOffset; }
  inline void set_vOffset(int offset) { _vOffset = offset; }
  void zoom_vertical(double steps);
  void compute_signal_groups();
  inline const std::vector<SignalGroup> &get_signal_groups() {
    return _signal_groups;
  }
  QColor get_group_card_color();
  QColor get_group_card_color(int group_index);
  QColor get_trace_card_color(Trace *trace);
  void set_group_card_color(QColor color) { _group_card_color = color; }
  bool is_colored_card_mode();

  int headerWidth();

  inline Ruler *get_ruler() { return _ruler; }

  // ---- Cursors list management (delegate to ViewCursors) ----
  /*
   * cursorList
   */
  std::list<Cursor *> &get_cursorList();

  void add_cursor(QColor color, uint64_t sampleIndex);
  void add_cursor(uint64_t sampleIndex);
  void del_cursor(Cursor *cursor);
  void add_xcursor(double value0, double value1);
  void del_xcursor(XCursor *xcursor);

  void clear_cursors();
  void set_cursor_middle(int index);

  inline Cursor *get_trig_cursor() { return _trig_cursor; }

  Cursor *get_cursor_by_index(int index);

  // Task C2.7: write a dragged cursor's new position back to the Core-layer
  // CursorRegistry via DataSource::set_cursor_position. Called by the
  // ruler / viewport drag handlers after TimeMarker::set_index.
  void sync_cursor_position(Cursor *cursor);

  // Task C2.7: reconcile the View's rendering cursor list with the Core
  // CursorRegistry. Called on data-source binding so cursors added by MCP
  // while headless appear once the View is created.
  void sync_cursors_from_core();

  inline Cursor *get_search_cursor() { return _search_cursor; }

  inline bool get_search_hit() { return _search_hit; }

  void set_search_pos(uint64_t search_pos, bool hit);

  inline uint64_t get_search_pos() { return _search_pos; }

  void scroll_to_logic_last_data_time();

  /*
   * horizental cursors
   */
  inline bool xcursors_shown() { return _show_xcursors; }

  inline void show_xcursors(bool show) { _show_xcursors = show; }

  inline std::list<XCursor *> &get_xcursorList() { return _xcursorList; }

  // ---- Viewport update ----
  void set_update(Viewport *viewport, bool need_update);
  void set_all_update(bool need_update);

  uint64_t get_cursor_samples(int index);
  QString get_cm_time(int index);
  QString get_cm_delta(int index1, int index2);
  QString get_index_delta(uint64_t start, uint64_t end);

  void on_state_changed(bool stop);

  QRect get_view_rect();
  int get_view_width();
  int get_view_height();
  int get_work_mode() const;
  // 统一 LOGIC 与 MSO 模式的渲染/交互路径：MSO 视为 "LOGIC + 模拟通道"，
  // 所有 LOGIC 模式下的渲染、交互行为在 MSO 模式下应当照常生效。
  bool is_logic_rendering_mode() const;

  double get_hori_res();

  QString get_measure(QString option);

  void viewport_update();

  void show_captured_progress(bool triggered, int progress);

  bool get_dso_trig_moved();

  inline ViewStatus *get_viewstatus() { return _viewbottom; }

  /*
   * back paint status
   */
  inline bool back_ready() { return _back_ready; }

  inline void set_back(bool ready) { _back_ready = ready; }

  /*
   * untils
   */
  double index2pixel(uint64_t index, bool has_hoff = true);
  uint64_t pixel2index(double pixel);

  int get_cursor_index_by_key(uint64_t key);

  void rebuild_signals();
  void rebuild_signals_from_config(const data::SignalConfig &config);

  // ---- Decoder management (delegate to ViewDerivedTraces) ----
  /**
   * Adds a protocol decoder.
   * Calls Core layer (SigSession::add_decoder) to create the DecoderStack,
   * then directly creates a DecodeTrace wrapping that stack. Does NOT rely
   * on the signals_changed event callback to populate the View's
   * DecodeTrace list.
   *
   * @param dec The srd_decoder to add.
   * @param silent If true, do not auto-start the decode task.
   * @param dstatus The DecoderStatus to associate with the new stack.
   * @param sub_decoders Sub-decoders to attach to the root decoder.
   * @param out_stack Output: the newly created DecoderStack (Core-owned).
   * @return true on success, false on failure.
   */
  bool add_decoder(srd_decoder *const dec, bool silent, DecoderStatus *dstatus,
                   std::list<pv::data::decode::Decoder *> &sub_decoders,
                   std::shared_ptr<pv::data::DecoderStack> &out_stack);

  /**
   * Removes a protocol decoder.
   * The View deletes its DecodeTrace (View-owned) first, then notifies
   * the Core layer to delete the corresponding DecoderStack via
   * remove_decoder_by_key_handel.
   *
   * @param trace The DecodeTrace to remove. Must be owned by this View.
   */
  void remove_decoder(DecodeTrace *trace);

  /**
   * Removes a protocol decoder by index in the View's DecodeTrace list.
   */
  void remove_decoder(int index);

  /**
   * Removes a protocol decoder by the key handle of its DecoderStack.
   * This is the primary entry point for GUI components (e.g., ProtocolDock)
   * that hold the key_handle but not the DecodeTrace pointer. The method
   * finds the matching DecodeTrace, deletes it, then notifies Core to
   * delete the DecoderStack.
   *
   * @param key_handel The key handle returned by DecoderStack::get_key_handel().
   */
  void remove_decoder_by_key_handel(void *key_handel);

  /**
   * Removes all protocol decoders from this View.
   * The View deletes all its DecodeTrace objects first, then notifies Core
   * to clear all DecoderStacks. This is the View layer entry point for
   * ProtocolDock::del_all_protocol().
   */
  void clear_all_decoders();

  /**
   * Resets (re-opens options dialog + re-runs decode) a protocol decoder
   * by its DecoderStack key handle.
   *
   * The View layer first looks up the DecodeTrace that wraps the given
   * DecoderStack, then re-opens the DecoderOptionsDlg via
   * DecodeTrace::create_popup(false). If the user cancels the dialog (no
   * settings change), no reset is performed and the existing configuration
   * is preserved. Otherwise the View forwards to Core's
   * rst_decoder_by_key_handel() to clear and re-add the decode task.
   *
   * This restores the pre-de-view-ization behavior where SigSession called
   * DecodeTrace::create_popup(false) directly; that call was removed because
   * Core must not depend on Qt Widgets.
   *
   * @param handel The key handle of the DecoderStack to reset.
   * @return true if the reset proceeded (user accepted the dialog),
   *         false if the decoder was not found or the user cancelled.
   */
  bool rst_decoder_by_key_handel(void *handel, QPoint anchor = QPoint());

  inline std::vector<Signal *> &get_own_signals() { return _own_signals; }

  /**
   * View-owned wrapper lists for derived trace types.
   * These wrap the Core layer's Stack/Model objects (DecoderStack,
   * SpectrumStack, MathStack, LissajousModel) into View layer Trace
   * subclasses so that the rendering code can operate on view::Trace*.
   * Synced lazily via sync_derived_traces() when the underlying data
   * source changes.
   */
  inline std::vector<DecodeTrace *> &get_own_decode_traces() {
    sync_derived_traces();
    return _own_decode_traces;
  }
  inline std::vector<SpectrumTrace *> &get_own_spectrum_traces() {
    sync_derived_traces();
    return _own_spectrum_traces;
  }
  inline MathTrace *get_own_math_trace() {
    sync_derived_traces();
    return _own_math_trace;
  }
  inline LissajousTrace *get_own_lissajous_trace() {
    sync_derived_traces();
    return _own_lissajous_trace;
  }
  void sync_derived_traces();
  void mark_derived_traces_dirty();

  void update_view_port();

  inline void update_all_trace_postion() { signals_changed(NULL); }

  inline Viewport *get_time_view() { return _time_viewport; }

  void update_font();
  void check_measure();
  bool header_is_draging();

  // IUiWindow
  void UpdateLanguage() override;
  void UpdateTheme() override;
  void UpdateFont() override;

  bool view_is_ready();

  /**
   * Glitch filter preview ranges cached per-signal for overlay rendering.
   * Populated by on_glitch_preview_changed() while the GlitchFilterPopup is
   * open; consumed by LogicSignal::paint_mid_align() via get_preview_ranges().
   * Returns nullptr when no preview is cached for the given signal.
   */
  const std::vector<pv::data::PulseAnalyzer::Pulse> *
  get_preview_ranges(LogicSignal *sig) const;

  /**
   * Undo stack snapshot for the glitch filter. Records the activation state
   * and the prior thresholds/modes before each apply so the user can undo
   * (Ctrl+Z) the most recent filter application. undo_filter() restores the
   * exact previous state: if was_active==true, set_glitch_filter(thresholds,
   * modes) is called; if was_active==false, clear_glitch_filter() is called.
   */
  struct FilterSnapshot {
    std::map<int, uint32_t> thresholds;
    std::map<int, GlitchFilterMode> modes;
    bool was_active;
  };

  void undo_filter();
  bool can_undo_filter() const { return !_filter_undo_stack.empty(); }

  /**
   * Forwards glitch filter completion/clearing notifications (originating
   * from FilterProcessor via MainWindow::on_filter_completed) to the
   * GlitchFilterPopup. If the popup is currently open for a LogicSignal,
   * it will recompute the histogram to reflect the updated LogicSnapshot
   * data (filtered pulses become long pulses after applying the filter).
   * No-op when the popup is closed.
   */
  void on_glitch_filter_completed();
  void on_glitch_filter_cleared();

  /**
   * Per-tab UI state cache for dock widgets and the sampling toolbar.
   * View is per-tab and survives tab switches without destruction, so
   * hosting DockUiState here gives docks/toolbar a stable per-tab store
   * accessed via `ctx->view()->dock_ui_state()`. Replaces the former
   * `_dock_*` fields on SessionDocument (Core layer).
   */
  DockUiState &dock_ui_state() { return _dock_ui_state; }
  const DockUiState &dock_ui_state() const { return _dock_ui_state; }

  void show_wait_trigger();
  void set_device();
  void set_receive_len(uint64_t len);
  int get_body_width();
  int get_body_height();

  // ---- Signal lifecycle (signals_changed event family) ----
  void signals_changed(const Trace *eventTrace);

  /**
   * Handler for the Core-layer signals_changed event.
   * Incrementally updates _own_signals to match the Core's SignalModel
   * list via SignalFactory::update_signals(), preserving UI state
   * (selection, visibility, v_offset, etc.) for signals that survive.
   * Does NOT directly affect DecodeTrace/SpectrumTrace/MathTrace — those
   * are managed by sync_derived_traces() based on Core's Stack list.
   * Then calls signals_changed(NULL) to refresh layout and trigger lazy
   * sync of derived traces.
   *
   * This is intended as the View-layer entry point for the
   * ISessionCallback::signals_changed() event. Currently MainWindow
   * dispatches the event to View::signals_changed(NULL); once
   * ISessionCallback is split (Task 13), MainWindow should call
   * View::on_signals_changed() instead.
   */
  void on_signals_changed();

  /**
   * Incremental layout update for newly added signals.
   * Called after SignalFactory::update_signals(Added) added new signals
   * to _own_signals. Triggers signals_changed(NULL) for layout recalculation.
   */
  void signals_added_layout();

  /**
   * Incremental layout update for removed signals.
   * Called after SignalFactory::update_signals(Removed) removed signals
   * from _own_signals. Triggers signals_changed(NULL) for layout recalculation.
   */
  void signals_removed_layout();

  /**
   * Incremental update for modified signal properties.
   * Called after SignalFactory::update_signals(Modified) refreshed properties.
   * Only repaints the affected signals, no layout changes needed.
   */
  void signals_modified_refresh();

public slots:
  void reload();
  void set_measure_en(int enable);

  void data_updated();
  void update_scale_offset();
  void show_region(uint64_t start, uint64_t end, bool keep);
  void status_clear();
  void repeat_unshow();

  // -- repeat
  void repeat_show();
  // --
  void timebase_changed();
  // --
  void vDial_updated();

  void dso_factor_updated();

  // --
  void update_hori_res();

  //
  void header_updated();

  void receive_trigger(quint64 trig_pos1);

  void receive_end();

  void frame_began();

  void mode_changed();

  // -- glitch filter popup handlers (Task 7)
  void on_show_glitch_filter_popup(pv::view::LogicSignal *sig);
  void on_clear_glitch_filter_requested(bool all_channels);
  void on_toggle_invert_requested(pv::view::LogicSignal *sig);

  // -- trig pos / lissajous
  void set_trig_pos(int percent);
  void show_lissajous(bool show);

signals:
  void hover_point_changed();
  void cursor_update();
  void xcursor_update();
  void cursor_moving();
  void cursor_moved();
  void measure_updated();
  void prgRate(int progress);
  void resize();
  void auto_trig(int index);
  // Emitted (after a 100ms debounce) whenever _scale / _offset / view_width
  // changes in a way that alters the visible sample range. Listeners (e.g.
  // ProtocolDock) use this to filter their list to the visible portion.
  void visible_range_changed();

private slots:
  void h_scroll_value_changed(int value);
  void v_scroll_value_changed(int value);

  void on_traces_moved();
  void on_measure_updated();

  void splitterMoved(int pos, int index);
  void on_header_collapse_changed(bool collapsed);

  // -- glitch filter popup internal handlers (Task 7)
  void
  on_glitch_preview_changed(pv::view::LogicSignal *sig, uint32_t threshold,
                            GlitchFilterMode mode);
  void on_glitch_apply_requested(pv::view::LogicSignal *sig, uint32_t threshold,
                                 GlitchFilterMode mode, bool all_channels);
  void on_glitch_popup_closed();
  // 批量模式:对一组逻辑通道统一应用/预览滤波
  void on_apply_batch_requested(const std::vector<pv::view::LogicSignal *> &sigs,
                                uint32_t threshold, GlitchFilterMode mode);
  void on_preview_batch_changed(const std::vector<pv::view::LogicSignal *> &sigs,
                                uint32_t threshold, GlitchFilterMode mode);

private:
  // ---- Friends (delegates touch View's private state directly) ----
  // Phase E delegates — friend classes so they can touch View's private
  // state (_scale / _offset / _own_decode_traces / _trig_cursor / …)
  // directly. Each delegate owns *behaviour*; the state still lives on View.
  friend class ViewLayout;
  friend class ViewCursors;
  friend class ViewDerivedTraces;
  // Phase J delegates — signal-sync / glitch-filter / data-sync behaviour
  // extracted from the View God-class during modernize-view-layer-v3.
  friend class ViewSignalSync;
  friend class ViewGlitchFilter;
  friend class ViewDataSync;

  // ---- Private static constants ----
  static const int LabelMarginWidth;
  static const int RulerHeight;
  static const int MaxScrollValue;
  static const int MaxHeightUnit;

  // ---- Internal helpers (delegate to ViewLayout / ViewCursors) ----
  void get_scroll_layout(int64_t &length, int64_t &offset);
  void update_scroll();
  void update_margins();
  void set_scale(double scale);

  void set_trig_cursor_posistion(uint64_t percent);
  void make_cursors_order();

  void clear();
  void reconstruct();
  bool eventFilter(QObject *object, QEvent *event);
  bool viewportEvent(QEvent *e);
  void paintEvent(QPaintEvent *event);
  void resizeEvent(QResizeEvent *e);
  void scrollContentsBy(int dx, int dy);

  // Restart the visible-range debounce timer. Repeated calls while the
  // timer is already running restart it (QTimer::start semantics), so only
  // the last call in a burst of drag/zoom events fires visible_range_changed.
  void schedule_visible_range_notify();

  // ---- Delegate members (Phase E + J) ----
  std::unique_ptr<ViewLayout> _layout;
  std::unique_ptr<ViewCursors> _cursors;
  std::unique_ptr<ViewDerivedTraces> _derived;
  std::unique_ptr<ViewSignalSync> _signal_sync;
  std::unique_ptr<ViewGlitchFilter> _glitch_filter;
  std::unique_ptr<ViewDataSync> _data_sync;

  // ---- Session / data source ----
  SigSession *_session;
  pv::data::DataSource *_data_source;
  pv::data::SessionDocument *_document;
  pv::toolbars::SamplingBar *_sampling_bar;
  DockUiState _dock_ui_state;
  // Re-entrancy guard for rebuild_signals_from_config(). Set while a rebuild
  // is in progress so a nested broadcast (e.g. DeviceOptionsUpdated) cannot
  // recurse into another rebuild and stack-overflow. The RAII guard in
  // rebuild_signals_from_config() resets it on all exit paths.
  bool _rebuild_in_progress = false;

  // ---- View-owned signal/trace wrappers ----
  std::vector<Signal *> _own_signals;
  // View-owned wrapper traces for derived types. Synced lazily from the
  // Core layer's Stack/Model objects via sync_derived_traces().
  std::vector<DecodeTrace *> _own_decode_traces;
  std::vector<SpectrumTrace *> _own_spectrum_traces;
  MathTrace *_own_math_trace = nullptr;
  LissajousTrace *_own_lissajous_trace = nullptr;
  bool _derived_traces_dirty = true;

  // ---- Viewport / widgets ----
  QWidget *_viewcenter;
  ViewStatus *_viewbottom;
  QSplitter *_vsplitter;
  Viewport *_time_viewport;
  Viewport *_fft_viewport;
  Viewport *_active_viewport;
  std::list<QWidget *> _viewport_list;
  std::map<int, int> _trace_view_map;
  Ruler *_ruler;
  Header *_header;
  DevMode *_devmode;
  bool _header_collapsed;

  // ---- Scale / offset ----
  /// The view time scale in seconds per pixel.
  double _scale;
  double _preScale;
  double _maxscale;
  double _minscale;
  /// DSO user zoom factor (1.0 = fit one frame to viewport width).
  /// DSO mode update_scale_offset() forces _scale = cur_view_time/width
  /// every frame (called from data_updated()). To let the user zoom in
  /// and pan horizontally like LOGIC mode, we keep this factor separate
  /// and reapply it: _scale = base_scale * _dso_zoom_factor. zoom() only
  /// mutates this factor; data frames re-derive _scale from it. Reset to
  /// 1.0 on mode change (ViewDataSync::mode_changed).
  double _dso_zoom_factor = 1.0;

  /// The pixels offset of the left edge of the view
  int64_t _offset;
  int64_t _preOffset;
  int _spanY;
  int _signalHeight;
  int _vOffset;
  int _signalHeightScale;
  int _lastWidth;
  std::vector<SignalGroup> _signal_groups;
  QColor _group_card_color;
  bool _updating_scroll;

  // ---- Visible-range notify debounce ----
  // Single-shot 100ms timer coalescing bursts of scale/offset/resize changes
  // into a single visible_range_changed() emission. Owned by View (parent
  // QObject) so it is destroyed automatically.
  QTimer *_viewport_change_timer = nullptr;

  // trigger position fix
  double _trig_hoff;

  // ---- Cursors ----
  bool _show_cursors;
  std::list<Cursor *> _logic_cursors;
  std::list<Cursor *> _dso_cursors;
  Cursor *_trig_cursor;
  bool _show_trig_cursor;
  Cursor *_search_cursor;
  bool _show_search_cursor;
  uint64_t _search_pos;
  bool _search_hit;

  bool _show_xcursors;
  std::list<XCursor *> _xcursorList;

  // ---- Misc ----
  QPoint _hover_point;

  bool _dso_auto;
  bool _show_lissajous;
  bool _back_ready;
  bool _destroying = false;
  DeviceAgent *_device_agent;
  QElapsedTimer _data_updated_timer;

  // ---- Glitch filter popup (View-owned, Task 7) ----
  GlitchFilterPopup *_glitch_filter_popup = nullptr;
  // Per-signal cached preview ranges (orange overlay) while the popup is open.
  std::map<LogicSignal *, std::vector<pv::data::PulseAnalyzer::Pulse>>
      _preview_ranges;
  // Undo stack for glitch filter applications (Task 9). Accessed only on the
  // GUI thread (slots + Ctrl+Z handler), so no synchronization needed.
  std::vector<FilterSnapshot> _filter_undo_stack;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEW_H