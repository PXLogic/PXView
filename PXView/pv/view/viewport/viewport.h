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

#ifndef PXVIEW_PV_VIEW_VIEWPORT_H
#define PXVIEW_PV_VIEW_VIEWPORT_H

#include <cstdint>

#include <QElapsedTimer>
#include <QNativeGestureEvent>
#include <QTime>
#include <QTimer>
#include <QWidget>
#include <chrono>
#include <memory>


#include "pv/base/pxvdef.h"
#include "pv/data/decoderanalogdata.h"
#include "pv/interface/icallbacks.h"
#include "pv/ui/uimanager.h"
#include "pv/view/view.h"
#include "pv/view/component/edge_nav_button.h"
#include "pv/view/trace/trace.h"


class QPainter;
class QPaintEvent;
class SigSession;
class QAction;

// Frame timing: thread-local DSO paint sub-timing, written by
// DsoSignal::paint_mid and read by ViewportPainter::doPaint summary.
struct DsoPaintTiming {
    bool active = false;
    qint64 get_samples_ms = 0;
    qint64 paint_draw_ms = 0;
    qint64 hw_offset_ms = 0;
    int64_t sample_count = 0;
    double samples_per_pixel = 0;
};
extern thread_local DsoPaintTiming s_dso_timing;

using std::chrono::high_resolution_clock;
using std::chrono::milliseconds;

namespace pv {
namespace view {

enum UpdateEventType {
  UPDATE_EV_GENERIC,
  UPDATE_EV_MS_CLICK,
  UPDATE_EV_MS_MOVE,
  UPDATE_EV_MS_UP,
};

class Signal;
class LogicSignal;
class View;
class ViewportPainter;
class ViewportInteraction;
class ViewportDrag;

// Action / measure enumerators — promoted from Viewport's nested enums to
// namespace scope during Phase F so that ViewportPainter / ViewportInteraction
// / ViewportDrag (ported verbatim from viewport.cpp) can keep using bare
// names like LOGIC_ZOOM / NO_ACTION / DSO_VALUE without C++20 `using enum`.
enum ActionType {
  NO_ACTION,
  CURS_MOVE,
  LOGIC_EDGE,
  LOGIC_MOVE,
  LOGIC_ZOOM,
  LOGIC_JUMP,
  RESIZE_SIGNAL,
  DSO_XM_STEP0,
  DSO_XM_STEP1,
  DSO_XM_STEP2,
  DSO_YM,
  DSO_TRIG_MOVE,
  ANALOG_RANGE_DRAG
};

enum MeasureType { NO_MEASURE, LOGIC_FREQ, LOGIC_EDGE_CNT, DSO_VALUE };

struct AnalogMeasurementV2Options {
  bool show_channel = true;
  bool show_time = true;
  bool show_normalized = true;
  bool show_engineering_value = true;
  bool rise_time = true;
  bool fall_time = true;
  bool positive_overshoot = true;
  bool negative_overshoot = true;
  bool period = true;
  bool frequency = true;
  bool positive_width = true;
  bool negative_width = true;
  bool positive_duty_cycle = true;
  bool negative_duty_cycle = true;
  bool cycle_rms = true;
};

// main graph view port, in the middle region
// draw the left and right rule scale
// created by View
class Viewport : public QWidget, public IUiWindow {
  Q_OBJECT
  Q_PROPERTY(QColor panelBgColor READ panelBgColor WRITE setPanelBgColor)
  Q_PROPERTY(QColor panelTextColor READ panelTextColor WRITE setPanelTextColor)

public:
  static const int HitCursorMargin = 10;
  static const double HitCursorTimeMargin;
  static const int DragTimerInterval = 100;
  static const int MinorDragOffsetUp = 100;
  static const int DsoMeasureStages = 3;
  static const double MinorDragRateUp;
  static const double DragDamping;
  static const int SnapMinSpace = 10;
  static const int WaitLoopTime = 400;
  static const QColor PROBE_COLORS[8];

public:
  explicit Viewport(View &parent, View_type type);
  ~Viewport();

  // Public static constant (needed by ViewportInteraction)
  static constexpr int DragFrameInterval = 16;

  QColor panelBgColor() const { return _panelBgColor; }
  void setPanelBgColor(QColor c) { _panelBgColor = c; }
  QColor panelTextColor() const { return _panelTextColor; }
  void setPanelTextColor(QColor c) { _panelTextColor = c; }

  int get_total_height();
  QPoint get_mouse_point();
  QString get_measure(QString option);
  void set_measure_en(int enable);
  void stop_trigger_timer();

  void clear_measure();
  void clear_dso_xm();
  void set_need_update(bool update);
  void set_decode_dirty();
  // P2 decode-only paint. Marks this viewport so the next paint runs the
  // decode-only path (SignalPixmapPass skips the signal-pixmap rebuild and
  // just blits the existing cache; DecodeTracePass still redraws). The flag
  // is consumed (self-cleared) by ViewportPainter::paintSignals via
  // take_decode_only_paint(). Note: set_decode_dirty() is NOT called on this
  // path — signal waveforms are unchanged during decode growth.
  void set_decode_only_paint() { _decode_only_paint = true; }
  bool take_decode_only_paint() {
    const bool v = _decode_only_paint;
    _decode_only_paint = false;
    return v;
  }
  int get_fps();
  bool get_dso_trig_moved();

  void set_receive_len(quint64 length);
  void unshow_wait_trigger();
  void show_wait_trigger();

  void measure();
  void update(int event);

  // State helpers retained on Viewport (called by delegates via back-pointer).
  // Public so ViewportPainter / ViewportInteraction / ViewportDrag
  // can call them without friend declarations.
  void start_trigger_timer(int msec);
  void get_captured_progress(double &progress, int &progress100);
  void set_action(ActionType action);
  // Forwarding method so ViewportInteraction can call the base-class
  // QWidget::keyPressEvent (which is protected in QWidget).
  void forward_keyPressEvent(QKeyEvent *event);
  // Forwarding method so ViewportInteraction can call the base-class
  // QWidget::event (which is protected in QWidget).
  bool forward_event(QEvent *event);
  // Delegate-accessed slots (moved from private to public).
  void applyDragFrame();
  void show_logic_contextmenu(const QPoint &pos);
  void clear_analog_measurement();
  void configure_analog_measurement();
  void export_decoder_audio_wav();
  void play_decoder_audio();

protected:
  bool event(QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

  // IUiWindow
  void UpdateLanguage() override;
  void UpdateTheme() override;
  void UpdateFont() override;

private:
  // Qt event overrides are thin forwarders to the interaction delegate.
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void leaveEvent(QEvent *) override;
  void resizeEvent(QResizeEvent *e) override;
  void keyPressEvent(QKeyEvent *event) override;
  bool gestureEvent(QNativeGestureEvent *event);


private slots:
  void on_trigger_timer();
  void on_drag_timer();
  void on_progress_timer();

  void show_contextmenu(const QPoint &pos);
  void add_cursor_x();
  void add_cursor_y();
  void copy_waveform_this_channel();
  void copy_waveform_decoder_track();
  void copy_waveform_decoder_group();
  void copy_waveform_all_channels();

signals:
  void measure_updated();
  void prgRate(int progress);

public:
  // ---- Spec v2 Task 1: Public accessors for delegate classes ----
  // Member variables are now private; delegates use these reference-returning
  // accessors instead of direct _viewport->_xxx access.

  // A. Rendering state (ViewportPainter / SignalPixmapPass)
  View& view() { return _view; }
  View_type& type() { return _type; }
  bool& need_update() { return _need_update; }
  QPixmap& pixmap() { return _pixmap; }
  QMenu*& cmenu() { return _cmenu; }
  double& curScale() { return _curScale; }
  int64_t& curOffset() { return _curOffset; }
  int& curSignalHeight() { return _curSignalHeight; }
  int& curVOffset() { return _curVOffset; }

  // B. Interaction state (ViewportInteraction)
  QPoint& mouse_point() { return _mouse_point; }
  QPoint& mouse_down_point() { return _mouse_down_point; }
  int64_t& mouse_down_offset() { return _mouse_down_offset; }
  ActionType& action_type() { return _action_type; }
  Signal*& drag_sig() { return _drag_sig; }
  Trace*& resize_trace_upper() { return _resize_trace_upper; }
  Trace*& resize_trace_lower() { return _resize_trace_lower; }
  int& resize_mouse_down_y() { return _resize_mouse_down_y; }
  int& resize_upper_height() { return _resize_upper_height; }
  int& resize_lower_height() { return _resize_lower_height; }
  bool& curs_moved() { return _curs_moved; }
  bool& xcurs_moved() { return _xcurs_moved; }
  LogicSignal*& hover_logic_signal() { return _hover_logic_signal; }

  // C. Drag state (ViewportDrag)
  QTimer& drag_frame_timer() { return _drag_frame_timer; }
  QPoint& drag_last_pos() { return _drag_last_pos; }
  bool& drag_frame_pending() { return _drag_frame_pending; }
  Qt::MouseButtons& drag_buttons() { return _drag_buttons; }
  int& drag_strength() { return _drag_strength; }
  QElapsedTimer& elapsed_time() { return _elapsed_time; }
  QTimer& drag_timer() { return _drag_timer; }

  // D. Measure state (MeasureOverlayPass)
  bool& measure_en() { return _measure_en; }
  MeasureType& measure_type() { return _measure_type; }
  bool& dso_xm_valid() { return _dso_xm_valid; }
  int& dso_xm_y() { return _dso_xm_y; }
  auto& dso_xm_index() { return _dso_xm_index; }
  bool& dso_ym_valid() { return _dso_ym_valid; }
  uint16_t& dso_ym_sig_index() { return _dso_ym_sig_index; }
  double& dso_ym_sig_value() { return _dso_ym_sig_value; }
  uint64_t& dso_ym_index() { return _dso_ym_index; }
  int& dso_ym_start() { return _dso_ym_start; }
  int& dso_ym_end() { return _dso_ym_end; }
  double& cur_preX() { return _cur_preX; }
  double& cur_aftX() { return _cur_aftX; }
  double& cur_thdX() { return _cur_thdX; }
  int& cur_midY() { return _cur_midY; }
  int& cur_preY() { return _cur_preY; }
  int& cur_preY_top() { return _cur_preY_top; }
  int& cur_preY_bottom() { return _cur_preY_bottom; }
  int& cur_aftY() { return _cur_aftY; }
  bool& edge_hit() { return _edge_hit; }
  QString& mm_width() { return _mm_width; }
  QString& mm_period() { return _mm_period; }
  QString& mm_freq() { return _mm_freq; }
  QString& mm_duty() { return _mm_duty; }
  uint64_t& edge_rising() { return _edge_rising; }
  uint64_t& edge_falling() { return _edge_falling; }
  uint64_t& edge_start() { return _edge_start; }
  uint64_t& edge_end() { return _edge_end; }
  QString& em_rising() { return _em_rising; }
  QString& em_falling() { return _em_falling; }
  QString& em_edges() { return _em_edges; }
  uint64_t& cur_sample() { return _cur_sample; }
  uint64_t& nxt_sample() { return _nxt_sample; }
  uint64_t& thd_sample() { return _thd_sample; }
  uint64_t& hover_index() { return _hover_index; }
  bool& hover_hit() { return _hover_hit; }
  uint16_t& hover_sig_index() { return _hover_sig_index; }
  double& hover_sig_value() { return _hover_sig_value; }
  std::shared_ptr<pv::data::DecoderAnalogData>& analog_measure_data() { return _analog_measure_data; }
  int& analog_measure_channel() { return _analog_measure_channel; }
  uint64_t& analog_measure_start() { return _analog_measure_start; }
  uint64_t& analog_measure_end() { return _analog_measure_end; }
  pv::data::DecoderAnalogStatistics& analog_measure_stats() { return _analog_measure_stats; }
  pv::data::DecoderAnalogCycleMetrics& analog_measure_cycle() { return _analog_measure_cycle; }
  bool& analog_measure_valid() { return _analog_measure_valid; }
  AnalogMeasurementV2Options& analog_measure_options() { return _analog_measure_options; }

  // E. Trigger state (TriggerInfoPass)
  bool& transfer_started() { return _transfer_started; }
  int& timer_cnt() { return _timer_cnt; }
  int& waiting_trig() { return _waiting_trig; }
  bool& dso_trig_moved() { return _dso_trig_moved; }
  QTimer& trigger_timer() { return _trigger_timer; }
  bool& is_checked_trig() { return _is_checked_trig; }

  // F. Progress / FPS state
  uint64_t& sample_received() { return _sample_received; }
  double& progress_displayed() { return _progress_displayed; }
  QTimer& progress_timer() { return _progress_timer; }
  int& max_frame_time() { return _max_frame_time; }
  int& fps() { return _fps; }
  QTimer& fps_timer() { return _fps_timer; }
  QElapsedTimer& frame_interval_timer() { return _frame_interval_timer; }
  bool& is_idle() { return _is_idle; }
  int& paint_in_this_second() { return _paint_in_this_second; }

  // G. Context menu / actions
  QMenu*& logic_cmenu() { return _logic_cmenu; }
  QAction*& copy_this_channel_action() { return _copy_this_channel_action; }
  QAction*& copy_decoder_track_action() { return _copy_decoder_track_action; }
  QAction*& copy_decoder_group_action() { return _copy_decoder_group_action; }
  QAction*& copy_all_channels_action() { return _copy_all_channels_action; }
  QAction*& export_decoder_wav_action() { return _export_decoder_wav_action; }
  QAction*& play_decoder_audio_action() { return _play_decoder_audio_action; }
  QAction*& clear_analog_measure_action() { return _clear_analog_measure_action; }
  QAction*& configure_analog_measure_action() { return _configure_analog_measure_action; }
  QPoint& logic_menu_pos() { return _logic_menu_pos; }
  QAction*& yAction() { return _yAction; }
  QAction*& xAction() { return _xAction; }

  // H. Edge navigation
  EdgeNavButton*& prev_edge_btn() { return _prev_edge_btn; }
  EdgeNavButton*& next_edge_btn() { return _next_edge_btn; }

  // I. Wait trigger state
  high_resolution_clock::time_point& lst_wait_tigger_time() { return _lst_wait_tigger_time; }
  int& tigger_wait_times() { return _tigger_wait_times; }

  // J. Drag snapshot state (g_ prefixed members)
  bool& drag_active() { return g_drag_active; }
  QPixmap& drag_snapshot() { return g_drag_snapshot; }

  // K. Delegate access (unique_ptr members)
  ViewportPainter* painter() const { return _painter.get(); }
  ViewportInteraction* interaction() const { return _interaction.get(); }
  ViewportDrag* drag() const { return _drag.get(); }

private:
  // ---- Member variables (private, Spec v2 Task 1) ----
  View &_view;
  View_type _type;
  bool _need_update;
  // P2: decode-only paint flag (see set_decode_only_paint). Consumed by
  // ViewportPainter::paintSignals.
  bool _decode_only_paint = false;
  QPixmap _pixmap;
  QMenu *_cmenu;

  uint64_t _sample_received;
  double _progress_displayed;  // Smoothly interpolated 0.0–1.0 for fluid progress bar
  QTimer _progress_timer;      // 16ms timer (≈60 FPS) for smooth progress animation
  QPoint _mouse_point;
  QPoint _mouse_down_point;
  int64_t _mouse_down_offset;
  double _curScale;
  int64_t _curOffset;
  int _curSignalHeight;

  bool _measure_en;
  ActionType _action_type;
  MeasureType _measure_type;
  uint64_t _cur_sample;
  uint64_t _nxt_sample;
  uint64_t _thd_sample;
  double _cur_preX;
  double _cur_aftX;
  double _cur_thdX;
  int _cur_midY;
  int _cur_preY;
  int _cur_preY_top;
  int _cur_preY_bottom;
  int _cur_aftY;
  bool _edge_hit;
  QString _mm_width;
  QString _mm_period;
  QString _mm_freq;
  QString _mm_duty;

  uint64_t _edge_rising;
  uint64_t _edge_falling;
  uint64_t _edge_start;
  uint64_t _edge_end;
  QString _em_rising;
  QString _em_falling;
  QString _em_edges;

  QTimer _trigger_timer;
  bool _is_checked_trig;
  bool _transfer_started;
  int _timer_cnt;
  Signal *_drag_sig;
  uint64_t _hover_index;
  bool _hover_hit;
  uint16_t _hover_sig_index;
  double _hover_sig_value;

  // ZB-FENG: 1.5.3-ZB style decoder-analog point/range measurement.
  std::shared_ptr<pv::data::DecoderAnalogData> _analog_measure_data;
  int _analog_measure_channel = -1;
  uint64_t _analog_measure_start = 0;
  uint64_t _analog_measure_end = 0;
  pv::data::DecoderAnalogStatistics _analog_measure_stats;
  pv::data::DecoderAnalogCycleMetrics _analog_measure_cycle;
  bool _analog_measure_valid = false;
  AnalogMeasurementV2Options _analog_measure_options;

  QElapsedTimer _elapsed_time;
  QTimer _drag_timer;
  int _drag_strength;
  bool _dso_xm_valid;
  int _dso_xm_y;
  uint64_t _dso_xm_index[DsoMeasureStages];

  bool _dso_ym_valid;
  uint16_t _dso_ym_sig_index;
  double _dso_ym_sig_value;
  uint64_t _dso_ym_index;
  int _dso_ym_start;
  int _dso_ym_end;
  int _waiting_trig;
  bool _dso_trig_moved;
  Trace *_resize_trace_upper;
  Trace *_resize_trace_lower;
  int _resize_mouse_down_y;
  int _resize_upper_height;
  int _resize_lower_height;
  bool _curs_moved;
  bool _xcurs_moved;
  int _curVOffset;

  high_resolution_clock::time_point _lst_wait_tigger_time;
  int _tigger_wait_times;
  QAction *_yAction;
  QAction *_xAction;

  QMenu *_logic_cmenu;
  QAction *_copy_this_channel_action;
  QAction *_copy_decoder_track_action;
  QAction *_copy_decoder_group_action;
  QAction *_copy_all_channels_action;
  QAction *_export_decoder_wav_action = nullptr;
  QAction *_play_decoder_audio_action = nullptr;
  QAction *_clear_analog_measure_action = nullptr;
  QAction *_configure_analog_measure_action = nullptr;
  QPoint _logic_menu_pos;

  QColor _panelBgColor;
  QColor _panelTextColor;
  int _max_frame_time;
  int _fps;
  QTimer _fps_timer;
  QElapsedTimer _frame_interval_timer;
  bool _is_idle;

  QTimer _drag_frame_timer;
  QPoint _drag_last_pos;
  bool _drag_frame_pending;
  Qt::MouseButtons _drag_buttons;

  // Edge navigation buttons
  EdgeNavButton *_prev_edge_btn;
  EdgeNavButton *_next_edge_btn;
  LogicSignal *_hover_logic_signal;

  // Delegates (Phase F1/F2/F3). Owned by Viewport; constructed in the ctor.
  std::unique_ptr<ViewportPainter> _painter;
  std::unique_ptr<ViewportInteraction> _interaction;
  std::unique_ptr<ViewportDrag> _drag;

  bool g_drag_active;
  int _paint_in_this_second;
  QPixmap g_drag_snapshot;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_VIEWPORT_H
