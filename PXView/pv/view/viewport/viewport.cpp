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

#include "pv/view/viewport/viewport.h"
#include "pv/view/viewport/viewport_drag.h"
#include "pv/view/viewport/viewport_interaction.h"
#include "pv/view/renderer/viewport_painter.h"
#include "pv/view/component/ruler.h"
#include "pv/view/component/viewstatus.h"

#include "pv/session/sigsession.h"
#include "pv/view/signal/analogsignal.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/signal/signal.h"
#include "pv/view/trace/spectrumtrace.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSettings>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>

#include "pv/config/appconfig.h"
#include "pv/ui/msgbox.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/view/component/waveform_copy_helper.h"
#include "pv/view/component/decoderaudioplayer.h"

#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/base/perflog.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/fn.h"
#include "pv/ui/langresource.h"
#include "pv/view/trace/lissajoustrace.h"
#include "pv/view/trace/mathtrace.h"
#include "pv/view/component/waveform_copy_helper.h"
#include "pv/view/trace/decodetrace.h"

#include <QClipboard>
#include <QGuiApplication>

using namespace std;

namespace pv {
namespace view {

// Analog measurement options persistence (QSettings group).
static const char *kAnalogMeasurementSettingsGroup = "AnalogMeasurementV2";

static AnalogMeasurementV2Options load_analog_measurement_options()
{
  AnalogMeasurementV2Options options;
  QSettings settings(QApplication::organizationName(),
                     QApplication::applicationName());
  settings.beginGroup(kAnalogMeasurementSettingsGroup);
  options.show_channel = settings.value("showChannel", true).toBool();
  options.show_time = settings.value("showTime", true).toBool();
  options.show_normalized = settings.value("showNormalized", true).toBool();
  options.show_engineering_value =
      settings.value("showEngineeringValue", true).toBool();
  options.rise_time = settings.value("riseTime", true).toBool();
  options.fall_time = settings.value("fallTime", true).toBool();
  options.positive_overshoot =
      settings.value("positiveOvershoot", true).toBool();
  options.negative_overshoot =
      settings.value("negativeOvershoot", true).toBool();
  options.period = settings.value("period", true).toBool();
  options.frequency = settings.value("frequency", true).toBool();
  options.positive_width = settings.value("positiveWidth", true).toBool();
  options.negative_width = settings.value("negativeWidth", true).toBool();
  options.positive_duty_cycle =
      settings.value("positiveDutyCycle", true).toBool();
  options.negative_duty_cycle =
      settings.value("negativeDutyCycle", true).toBool();
  options.cycle_rms = settings.value("cycleRms", true).toBool();
  settings.endGroup();
  return options;
}

static void save_analog_measurement_options(
    const AnalogMeasurementV2Options &options)
{
  QSettings settings(QApplication::organizationName(),
                     QApplication::applicationName());
  settings.beginGroup(kAnalogMeasurementSettingsGroup);
  settings.setValue("version", 2);
  settings.setValue("showChannel", options.show_channel);
  settings.setValue("showTime", options.show_time);
  settings.setValue("showNormalized", options.show_normalized);
  settings.setValue("showEngineeringValue", options.show_engineering_value);
  settings.setValue("riseTime", options.rise_time);
  settings.setValue("fallTime", options.fall_time);
  settings.setValue("positiveOvershoot", options.positive_overshoot);
  settings.setValue("negativeOvershoot", options.negative_overshoot);
  settings.setValue("period", options.period);
  settings.setValue("frequency", options.frequency);
  settings.setValue("positiveWidth", options.positive_width);
  settings.setValue("negativeWidth", options.negative_width);
  settings.setValue("positiveDutyCycle", options.positive_duty_cycle);
  settings.setValue("negativeDutyCycle", options.negative_duty_cycle);
  settings.setValue("cycleRms", options.cycle_rms);
  settings.endGroup();
  settings.sync();
}

const double Viewport::DragDamping = 1.05;
const double Viewport::MinorDragRateUp = 10;

const QColor Viewport::PROBE_COLORS[8] = {

    QColor(0x75, 0x50, 0x7B), // Violet
    QColor(0x34, 0x65, 0xA4), // Blue
    QColor(0x73, 0xD2, 0x16), // Green
    QColor(0xED, 0xD4, 0x00), // Yellow
    QColor(0xF5, 0x79, 0x00), // Orange
    QColor(0xCC, 0x00, 0x00), // Red
    QColor(0x8F, 0x52, 0x02), // Brown
    QColor(0x50, 0x50, 0x50), // Black

};

Viewport::Viewport(View &parent, View_type type)
    : QWidget(&parent), _view(parent), _type(type), _need_update(false),
      _sample_received(0), _action_type(NO_ACTION), _measure_type(NO_MEASURE),
      _cur_sample(0), _nxt_sample(1), _cur_preX(0), _cur_aftX(1), _cur_midY(0),
      _hover_index(0), _hover_hit(false), _dso_xm_valid(false),
      _dso_ym_valid(false), _waiting_trig(0), _dso_trig_moved(false),
      _resize_trace_upper(nullptr), _resize_trace_lower(nullptr),
      _resize_mouse_down_y(0), _resize_upper_height(0), _resize_lower_height(0),
      _curs_moved(false), _xcurs_moved(false), _curVOffset(0),
      _max_frame_time(0), _fps(0), _is_idle(true), _drag_frame_pending(false),
      _hover_logic_signal(nullptr), g_drag_active(false),
      _paint_in_this_second(0) {
  _panelBgColor = AppConfig::Instance().GetThemeColor("@panel-bg");
  if (!_panelBgColor.isValid())
    _panelBgColor = QColor("#1a1a1a");
  _panelTextColor = AppConfig::Instance().GetThemeColor("@panel-text");
  if (!_panelTextColor.isValid())
    _panelTextColor = QColor("#f5f0e5");

  setMouseTracking(true);
  setAutoFillBackground(true);
  setBackgroundRole(QPalette::Base);
  setFocusPolicy(Qt::StrongFocus);

  // setFixedSize(QSize(600, 400));
  _mm_width = View::Unknown_Str;
  _mm_period = View::Unknown_Str;
  _mm_freq = View::Unknown_Str;
  _mm_duty = View::Unknown_Str;
_measure_en = true;
_analog_measure_options = load_analog_measurement_options();
_edge_hit = false;
  _transfer_started = false;
  _timer_cnt = 0;
  _sample_received = 0;
  _progress_displayed = 0.0;
  _is_checked_trig = false;

  _lst_wait_tigger_time = high_resolution_clock::now();
  _tigger_wait_times = 0;

  // drag inertial
  _drag_strength = 0;
  _drag_timer.setSingleShot(true);

  _cmenu = new QMenu(this);
  QAction *yAction = _cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_Y_CURSOR), "Add Y-cursor"));
  QAction *xAction = _cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_X_CURSOR), "Add X-cursor"));
  _yAction = yAction;
  _xAction = xAction;

  setContextMenuPolicy(Qt::CustomContextMenu);

  connect(&_trigger_timer, &QTimer::timeout, this, &Viewport::on_trigger_timer);
  connect(&_drag_timer, &QTimer::timeout, this, &Viewport::on_drag_timer);

  // Smooth progress animation: 16ms ≈ 60 FPS. Lerps the displayed
  // progress toward the actual _sample_received value so the progress
  // circle animates fluidly instead of jumping in discrete steps at
  // each data-packet arrival (which can be as slow as 10–25 FPS).
  _progress_timer.setInterval(16);
  connect(&_progress_timer, &QTimer::timeout, this, &Viewport::on_progress_timer);
  connect(yAction, &QAction::triggered, this, &Viewport::add_cursor_y);
  connect(xAction, &QAction::triggered, this, &Viewport::add_cursor_x);
  connect(this, &QWidget::customContextMenuRequested, this,
          &Viewport::show_contextmenu);

  // Logic/MSO mode context menu for "copy waveform between cursors"
  _logic_cmenu = new QMenu(this);
  _copy_this_channel_action = _logic_cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_THIS_CHANNEL), "Copy Channel Waveform Data"));
  _copy_decoder_track_action = _logic_cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_TRACK), "Copy Decoder Waveform Data"));
  _copy_decoder_group_action = _logic_cmenu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_GROUP), "Copy Decoder Group Waveform Data"));
_copy_all_channels_action = _logic_cmenu->addAction(
L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ALL_CHANNELS), "Copy All Channels Waveform Data"));
_logic_cmenu->addSeparator();
_export_decoder_wav_action = _logic_cmenu->addAction(
QStringLiteral("导出模拟音频 WAV..."));
_play_decoder_audio_action = _logic_cmenu->addAction(
QStringLiteral("播放模拟音频 / 多通道混音..."));
_logic_cmenu->addSeparator();
_clear_analog_measure_action = _logic_cmenu->addAction(
QStringLiteral("清除模拟区间测量"));
_configure_analog_measure_action = _logic_cmenu->addAction(
QStringLiteral("模拟测量显示项..."));
connect(_copy_this_channel_action, &QAction::triggered,
this, &Viewport::copy_waveform_this_channel);
connect(_copy_decoder_track_action, &QAction::triggered,
this, &Viewport::copy_waveform_decoder_track);
connect(_copy_decoder_group_action, &QAction::triggered,
this, &Viewport::copy_waveform_decoder_group);
connect(_copy_all_channels_action, &QAction::triggered,
this, &Viewport::copy_waveform_all_channels);
connect(_export_decoder_wav_action, &QAction::triggered,
this, &Viewport::export_decoder_audio_wav);
connect(_play_decoder_audio_action, &QAction::triggered,
this, &Viewport::play_decoder_audio);
connect(&DecoderAudioPlayer::instance(), &DecoderAudioPlayer::playbackError,
this, [](const QString &message) { MsgBox::Show(message); });
connect(_clear_analog_measure_action, &QAction::triggered,
this, &Viewport::clear_analog_measurement);
connect(_configure_analog_measure_action, &QAction::triggered,
this, &Viewport::configure_analog_measurement);

  connect(&_fps_timer, &QTimer::timeout, this, [this]() {
    if (_paint_in_this_second > 0) {
      _fps = _max_frame_time;
      _max_frame_time = 0;
      _paint_in_this_second = 0;
    } else {
      _is_idle = true;
    }
  });
  _fps_timer.start(1000);

  _drag_frame_timer.setSingleShot(true);
  connect(&_drag_frame_timer, &QTimer::timeout, this,
          &Viewport::applyDragFrame);

  // Edge navigation buttons
  _prev_edge_btn = new EdgeNavButton(EdgeNavButton::Previous, this);
  _next_edge_btn = new EdgeNavButton(EdgeNavButton::Next, this);
  _prev_edge_btn->hide();
  _next_edge_btn->hide();
  connect(_prev_edge_btn, &EdgeNavButton::clicked, this,
          [this]() { _interaction->navigate_to_edge(EdgeNavButton::Previous); });
  connect(_next_edge_btn, &EdgeNavButton::clicked, this,
          [this]() { _interaction->navigate_to_edge(EdgeNavButton::Next); });

  // Construct delegates (Phase F1/F2/F3). They hold a non-owning back-pointer
  // and access Viewport's private state through friend access.
  _painter = std::make_unique<ViewportPainter>(this);
  _interaction = std::make_unique<ViewportInteraction>(this);
  _drag = std::make_unique<ViewportDrag>(this);

  ADD_UI(this);
}

Viewport::~Viewport() { REMOVE_UI(this); }

int Viewport::get_total_height() {
  int h = 0;
  std::vector<Trace *> traces;
  _view.get_traces(_type, traces);

  if (_view.is_logic_rendering_mode() && _type == TIME_VIEW) {
    const auto &groups = _view.get_signal_groups();
    if (!groups.empty()) {
      // Collect grouped traces so we can identify non-group traces below.
      // signal_groups only contains LOGIC + DECODER traces (see
      // compute_signal_groups). ANALOG/DSO/Math traces are NOT in any
      // group but are still laid out vertically and occupy space.
      // Without accounting for them here, get_total_height() only returns
      // the height of logic/decoder traces — if those fill the viewport,
      // the scrollbar range is 0 and the user cannot scroll to see the
      // analog channels below.
      std::vector<Trace *> grouped_traces;
      for (const auto &group : groups) {
        for (auto gt : group.traces) {
          grouped_traces.push_back(gt);
          h += (int)(gt->get_totalHeight()) + 2 * View::SignalMargin;
        }
        h += View::GroupGap + 5;
      }
      // Add heights for non-group traces (ANALOG, DSO, Math, etc.)
      for (auto t : traces) {
        bool in_group = false;
        for (auto gt : grouped_traces) {
          if (gt == t) {
            in_group = true;
            break;
          }
        }
        if (!in_group) {
          h += (int)(t->get_totalHeight()) + 2 * View::SignalMargin;
        }
      }
      return h;
    }
  }

  for (auto t : traces) {
    h += (int)(t->get_totalHeight()) + 2 * View::SignalMargin;
  }

  return h;
}

QPoint Viewport::get_mouse_point() { return _mouse_point; }

bool Viewport::event(QEvent *event) {
  if (event->type() == QEvent::NativeGesture)
    return gestureEvent(static_cast<QNativeGestureEvent *>(event));
  return QWidget::event(event);
}

//--- Qt override forwarders (Phase F1/F2/F3) -------------------------------

void Viewport::paintEvent(QPaintEvent *event) { _painter->paintEvent(event); }

void Viewport::mousePressEvent(QMouseEvent *event) {
  _interaction->mousePressEvent(event);
}

void Viewport::mouseMoveEvent(QMouseEvent *event) {
  _interaction->mouseMoveEvent(event);
}

void Viewport::mouseReleaseEvent(QMouseEvent *event) {
  _interaction->mouseReleaseEvent(event);
}

void Viewport::mouseDoubleClickEvent(QMouseEvent *event) {
  _interaction->mouseDoubleClickEvent(event);
}

void Viewport::wheelEvent(QWheelEvent *event) {
  _interaction->wheelEvent(event);
}

void Viewport::leaveEvent(QEvent *event) { _interaction->leaveEvent(event); }

void Viewport::keyPressEvent(QKeyEvent *event) {
  _interaction->keyPressEvent(event);
}

void Viewport::forward_keyPressEvent(QKeyEvent *event) {
  QWidget::keyPressEvent(event);
}

bool Viewport::forward_event(QEvent *event) {
  return QWidget::event(event);
}

bool Viewport::gestureEvent(QNativeGestureEvent *event) {
  return _interaction->gestureEvent(event);
}

void Viewport::applyDragFrame() { _drag->applyDragFrame(); }

void Viewport::on_drag_timer() { _drag->on_drag_timer(); }

//--- Retained state helpers -------------------------------------------------

void Viewport::set_action(ActionType action) { _action_type = action; }

void Viewport::get_captured_progress(double &progress, int &progress100) {
  // Use the smoothly interpolated _progress_displayed (0.0–1.0) for
  // fluid animation instead of the raw _sample_received which jumps
  // in discrete steps at each data-packet arrival.
  progress = -(_progress_displayed * 360 * 16);
  progress100 = ceil(_progress_displayed * 100);
}

void Viewport::resizeEvent(QResizeEvent *e) {
  QWidget::resizeEvent(e);
  ViewStatus *vs = _view.get_viewstatus();
  if (vs) {
    int h = vs->height();
    vs->setGeometry(0, height() - h, width(), h);
  }
  clear_measure();
}

void Viewport::set_receive_len(quint64 length) {
  if (length == 0) {
    _sample_received = 0;
    _progress_displayed = 0.0;
    _progress_timer.stop();
    start_trigger_timer(333);
    _tigger_wait_times = 0;
    _is_checked_trig = false;
  } else {
    stop_trigger_timer();

    if (_sample_received + length > _view.session().cur_samplelimits())
      _sample_received = _view.session().cur_samplelimits();
    else
      _sample_received += length;

    // Start smooth-animation timer on first data arrival. It runs at
    // 60 FPS and lerps _progress_displayed toward the actual
    // _sample_received / sample_limits ratio, giving a fluid progress
    // bar regardless of how often data packets arrive.
    if (!_progress_timer.isActive())
      _progress_timer.start();
  }

  if (_view.is_logic_rendering_mode()) {
    auto *dev = _view.data_source()->device();
    if (dev && !dev->is_file()) {
      if (!_is_checked_trig && _view.session().is_triged()) {
        _view.get_viewstatus()->set_trig_time(_view.session().get_trig_time());
        _view.get_viewstatus()->update();
        _is_checked_trig = true;
      }
    }

    if (_view.session().is_repeat_mode()) {
      double progress = 0;
      int progress100 = 0;
      get_captured_progress(progress, progress100);
      _view.show_captured_progress(_transfer_started, progress100);

      // Stream mode + repeat: is_realtime_refresh() returns true, so
      // fall through to the realtime refresh check below (which gates
      // on have_new_realtime_refresh). Buffer mode + repeat (non-stream):
      // is_realtime_refresh() is false, so return to prevent stale updates.
      if (!_view.session().is_realtime_refresh()) {
        if (_view.session().is_single_buffer()) {
          if (_view.session().have_new_realtime_refresh(true) == false) {
            return;
          }
        } else {
          return;
        }
      }
    }
    
    if (_view.session().is_realtime_refresh()) {
      if (_view.session().have_new_realtime_refresh(true) == false) {
        return;
      }
    }
  }

  if (_view.is_logic_rendering_mode() && AppConfig::Instance().appOptions.autoScrollLatestData &&
      _view.session().is_realtime_refresh()) {
    _view.scroll_to_logic_last_data_time();
  }

  // P3-F2: the previous code set _need_update=true here on EVERY data packet
  // (logic + realtime refresh). That was redundant — the actual signal-data
  // refresh and pixmap rebuild are driven by the DataUpdated event
  // (ViewDataSync::data_updated → set_update(time_view, true), including its
  // 16ms dedup path). The redundant flag here made every subsequent paint
  // (e.g. Qt-driven Viewport::update) rebuild the whole signal pixmap
  // (~3-5ms) even during decode growth where the signal data is unchanged —
  // the dominant remaining per-frame cost after the F2 decode cache. Signals
  // are rebuilt only when data_updated (real new data) or a decode-only/full
  // update actually needs them.

  // In DSO mode, the async DataUpdated event (broadcast_async from
  // feed_in_dso) already drives ViewDataSync::data_updated() which calls
  // viewport_update(). Calling update() here as well causes a redundant
  // repaint on every data packet (~40/sec). Skip it for DSO running mode;
  // the progress timer still fires for progress-bar animation.
  if (_view.get_work_mode() == DSO && _view.session().is_running_status()) {
    return;
  }

  // P3-F1: do NOT force a full repaint on every feed packet. This function is
  // called once per data-feed packet (measured ~66/s at decode start), and the
  // unconditional update() here fed the direct-update repaint storm
  // (update_direct=65.9/s in the decode-start window), each paint triggering a
  // signal-pixmap rebuild through the interleaved viewport_update() dirty
  // flag. The 16ms progress timer (when active) already drives the repaints
  // that show incoming data, and DataUpdated → viewport_update() refreshes the
  // waveforms. So a direct repaint here is only justified for the settling
  // edge case (session still working but the animation is momentarily idle);
  // when the session is not working (e.g. decoding a static file), the
  // progress bar has nothing to animate and the per-packet update() is pure
  // waste, so it is skipped entirely.
  if (!_progress_timer.isActive() && _view.session().is_working())
    update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::update(int event) {
#ifdef PXVIEW_DECODE_PERF
  // P3-D: counts EVERY direct viewport update() call (from view_data_sync,
  // view_glitch_filter, viewport.cpp internals, interaction, update_view_port,
  // check_measure...).
  pv::base::perf::record_repaint_update_direct();
#endif
  QWidget::update();
  (void)event;
}

void Viewport::clear_measure() {
  _measure_type = NO_MEASURE;
  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::clear_interaction_state() {
  // 信号重建后旧 Signal/Trace 已销毁,清空拖拽/悬停缓存的裸指针,
  // 避免后续 mouseMove/Release/applyDragFrame 解引用悬垂对象。
  _drag_sig = nullptr;
  _resize_trace_upper = nullptr;
  _resize_trace_lower = nullptr;
  _hover_logic_signal = nullptr;
}

void Viewport::clear_dso_xm() {
  _dso_xm_valid = false;
  _mm_width = View::Unknown_Str;
  _mm_period = View::Unknown_Str;
  _mm_freq = View::Unknown_Str;
  _mm_duty = View::Unknown_Str;

  set_action(NO_ACTION);
}

void Viewport::measure() {
  if (_view.session().is_data_lock())
    return;
  if (_view.session().is_loop_mode() && _view.session().is_working())
    return;

  _measure_type = NO_MEASURE;

  if (_type == TIME_VIEW) {
    // CRITICAL: samplerate must come from document_snapshot_source() to match
    // index2pixel/pixel2index/ruler/cursor/paint_mid_align coordinate mapping.
    // Using _view.session().cur_snap_samplerate() (capture_data) or
    // _data->samplerate() (snapshot) can be out of sync with the document
    // samplerate, causing waveform/ruler/cursor/arrow misalignment.
    const uint64_t sample_rate =
        _view.document_snapshot_source()
            ? _view.document_snapshot_source()->cur_snap_samplerate()
            : _view.session().cur_snap_samplerate();

    // 列映射与 paint_mid_align 完全一致：
    // start_index=floor(offset*spp)，跳变画在列 floor((idx-start_index)/spp)。
    // 箭头必须用同一映射，否则高倍缩放下 offset*spp 的小数部分会被放大成
    // 数像素偏差（= frac(offset*spp)/spp，可达成数十像素）。

    for (auto &s : _view.get_own_signals()) {
      if (s->signal_type() == SR_CHANNEL_LOGIC) {
        view::LogicSignal *logicSig = (view::LogicSignal *)s.get();

        if (_action_type == NO_ACTION) {
          if (logicSig->measure(_mouse_point, _cur_sample, _nxt_sample,
                                _thd_sample)) {
            _measure_type = LOGIC_FREQ;

            _mm_width = _view.get_ruler()->format_real_time(
                _nxt_sample - _cur_sample, sample_rate);
            _mm_period = _thd_sample != 0
                             ? _view.get_ruler()->format_real_time(
                                   _thd_sample - _cur_sample, sample_rate)
                             : View::Unknown_Str;
            _mm_freq = _thd_sample != 0
                           ? _view.get_ruler()->format_real_freq(
                                 _thd_sample - _cur_sample, sample_rate)
                           : View::Unknown_Str;

            _cur_preX = _view.index2pixel(_cur_sample);
            _cur_aftX = _view.index2pixel(_nxt_sample);
            _cur_thdX = _view.index2pixel(_thd_sample);
            _cur_midY = logicSig->get_y();

            _mm_duty =
                _thd_sample != 0
                    ? QString::number((_nxt_sample - _cur_sample) * 100.0 /
                                          (_thd_sample - _cur_sample),
                                      'f', 2) +
                          "%"
                    : View::Unknown_Str;
            break;
          } else {
            _measure_type = NO_MEASURE;
            _mm_width = View::Unknown_Str;
            _mm_period = View::Unknown_Str;
            _mm_freq = View::Unknown_Str;
            _mm_duty = View::Unknown_Str;
          }
        } else if (_action_type == LOGIC_EDGE) {
          if (logicSig->edges(_view.hover_point(), _edge_start, _edge_rising,
                              _edge_falling)) {
            _cur_preX = _view.index2pixel(_edge_start);
            _cur_aftX = _view.hover_point().x();
            _cur_midY = logicSig->get_y() -
                        qRound(logicSig->get_totalHeight() * 0.5) - 5;

            _em_rising = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RISING), "Rising: ") +
                         QString::number(_edge_rising);
            _em_falling =
                L_S(STR_PAGE_DLG, S_ID(IDS_DLG_FALLING), "Falling: ") +
                QString::number(_edge_falling);
            _em_edges = L_S(STR_PAGE_DLG, S_ID(IDS_DLG_Edges_1), "Edges: ") +
                        QString::number(_edge_rising + _edge_falling);

            break;
          }
        } else if (_action_type == LOGIC_JUMP) {
          if (logicSig->edge(_view.hover_point(), _edge_end, 10)) {
            _cur_aftX = _view.index2pixel(_edge_end);
            _cur_aftY = logicSig->get_y();
            _edge_hit = true;
            break;
          } else {
            _cur_preX = _view.index2pixel(_edge_start);
            _cur_aftX = _view.hover_point().x();
            _cur_aftY = _view.hover_point().y();
            _edge_end = _view.pixel2index(_cur_aftX);
            _edge_hit = false;
          }
        }
      } else if (s->signal_type() == SR_CHANNEL_DSO) {
        view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
        if (s->enabled()) {
          if (_measure_en && dsoSig->measure(_view.hover_point())) {
            _measure_type = DSO_VALUE;
          } else {
            _measure_type = NO_MEASURE;
          }
        }
      } else if (s->signal_type() == SR_CHANNEL_ANALOG) {
        view::AnalogSignal *analogSig = (view::AnalogSignal *)s.get();
        if (s->enabled()) {
          if (_measure_en && analogSig->measure(_view.hover_point())) {
            _measure_type = DSO_VALUE;
          } else {
            _measure_type = NO_MEASURE;
          }
        }
      }
    }
    const auto mathTrace = _view.get_own_math_trace();
    if (mathTrace && mathTrace->enabled()) {
      if (_measure_en && mathTrace->measure(_view.hover_point())) {
        _measure_type = DSO_VALUE;
      } else {
        _measure_type = NO_MEASURE;
      }
    }
  } else if (_type == FFT_VIEW) {
    for (auto &t : _view.get_own_spectrum_traces()) {
      if (t->enabled()) {
        t->measure(_mouse_point);
      }
    }
  }

  measure_updated();
}

QString Viewport::get_measure(QString option) {
  if (option.compare("width") == 0)
    return _mm_width;
  else if (option.compare("period") == 0)
    return _mm_period;
  else if (option.compare("frequency") == 0)
    return _mm_freq;
  else if (option.compare("duty") == 0)
    return _mm_duty;
  else
    return View::Unknown_Str;
}

void Viewport::set_measure_en(int enable) {
  if (enable == 0)
    _measure_en = false;
  else
    _measure_en = true;
}

void Viewport::start_trigger_timer(int msec) {
  assert(msec > 0);
  _transfer_started = false;
  _timer_cnt = 0;
  _trigger_timer.start(msec);
}

void Viewport::stop_trigger_timer() {
  _transfer_started = true;
  _timer_cnt = 0;
  _trigger_timer.stop();
}

void Viewport::on_trigger_timer() {
  _timer_cnt++;

  if (!_is_checked_trig) {
    auto *dev = _view.data_source()->device();
    if (_view.is_logic_rendering_mode() &&
        dev && !dev->is_file()) {
      if (_view.session().is_triged()) {
        _is_checked_trig = true;
        _view.get_viewstatus()->set_trig_time(_view.session().get_trig_time());
        _view.get_viewstatus()->update();
      }
    } else {
      _is_checked_trig = true;
    }
  }

  if (_view.get_work_mode() == DSO) {
    update(UpdateEventType::UPDATE_EV_GENERIC);
  }
}

void Viewport::on_progress_timer() {
  // In repeat mode each frame is a discrete acquisition with its own target
  // sample count (get_sample_limit(), e.g. demo 1M). cur_samplelimits() is set
  // to the ring-buffer capacity in stream+repeat (to serve the ruler axis), so
  // using it here would pin the progress bar at ~0.4% (1M/250M) forever even
  // though the frame is complete. Use the per-frame target for repeat mode so
  // the bar reaches 100% each frame.
  const uint64_t sample_limits =
      _view.session().is_repeat_mode()
          ? (_view.session().device() ? _view.session().device()->get_sample_limit() : 0)
          : _view.session().cur_samplelimits();
  if (sample_limits == 0) {
    _progress_displayed = 0.0;
    _progress_timer.stop();
    return;
  }

  const double target =
      static_cast<double>(_sample_received) / static_cast<double>(sample_limits);
  const double diff = target - _progress_displayed;

  // P3-F1: when the session is no longer working (capture ended / stopped /
  // idle), the progress animation has nothing left to animate toward. Snap to
  // the final target and stop immediately instead of lerping at 60 FPS for
  // ~250ms (every tick is a full viewport repaint at ~9ms) or ticking forever
  // if set_receive_len keeps being fed in a stale path. This strictly removes
  // repaints, never adds them, and only affects the progress-bar settle timing.
  if (!_view.session().is_working()) {
    _progress_displayed = target;
    _progress_timer.stop();
    update(UpdateEventType::UPDATE_EV_GENERIC);
    return;
  }

#ifdef PXVIEW_DECODE_PERF
  // P3-D: every remaining tick issues a repaint (snap or lerp branch below).
  pv::base::perf::record_repaint_progress();
#endif

  if (qAbs(diff) < 0.0005) {
    // Close enough — snap to target and stop the timer. It will be
    // restarted by set_receive_len() when the next data packet arrives.
    // This prevents the 60 FPS timer from running idle between packets.
    _progress_displayed = target;
    _progress_timer.stop();
    update(UpdateEventType::UPDATE_EV_GENERIC);
    return;
  }

  // Exponential lerp: 25% of remaining distance per frame.
  // At 60 FPS this reaches 99% of target in ~15 frames (250ms),
  // which feels smooth without noticeable lag.
  _progress_displayed += diff * 0.25;
  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::set_need_update(bool update) { _need_update = update; }

void Viewport::set_decode_dirty() { _need_update = true; }

void Viewport::show_wait_trigger() {
  _waiting_trig %= (WaitLoopTime / SigSession::FeedInterval) * 4;
  _waiting_trig++;
  if (_view.get_work_mode() == DSO)
    update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::unshow_wait_trigger() {
  _waiting_trig = 0;
  if (_view.get_work_mode() == DSO)
    update(UpdateEventType::UPDATE_EV_GENERIC);
}

bool Viewport::get_dso_trig_moved() { return _dso_trig_moved; }

void Viewport::show_contextmenu(const QPoint &pos) {
  if (_cmenu && _view.get_work_mode() == DSO) {
    _cur_preX = pos.x();
    _cur_preY = pos.y();
    _cmenu->exec(QCursor::pos());
  }
}

void Viewport::add_cursor_y() {
  uint64_t index;
  index = _view.pixel2index(_cur_preX);
  _view.add_cursor(index);
  _view.show_cursors(true);
}

void Viewport::add_cursor_x() {
  double ypos =
      (_cur_preY - _view.get_view_rect().top()) * 1.0 / _view.get_view_height();
  _view.add_xcursor(ypos, ypos);
  _view.show_xcursors(true);
}

void Viewport::show_logic_contextmenu(const QPoint &pos) {
  if (!_logic_cmenu || !_view.is_logic_rendering_mode())
    return;

  _logic_menu_pos = pos;

  // Show/hide menu items based on hit test (hide unavailable instead of disable)
  LogicSignal *hit_sig = WaveformCopyHelper::hit_test_signal(
      _view, pos.x(), pos.y());
  DecodeTrace *hit_dt = (hit_sig == nullptr) ?
      WaveformCopyHelper::hit_test_decode_trace(_view, pos.x(), pos.y()) : nullptr;

  // "复制该通道波形数据" only visible when hit LogicSignal
  _copy_this_channel_action->setVisible(hit_sig != nullptr);

  // "复制该解码器波形数据" only visible when hit DecodeTrace
  _copy_decoder_track_action->setVisible(hit_dt != nullptr);

  bool has_decoder = false;
  if (hit_sig)
    has_decoder = WaveformCopyHelper::find_decoder_for_signal(_view, hit_sig) != nullptr;
  if (!has_decoder)
    has_decoder = (hit_dt != nullptr);
  _copy_decoder_group_action->setVisible(has_decoder);

// "复制所有通道波形数据" always visible in logic mode — outputs all logic signals + decoders
_copy_all_channels_action->setVisible(true);

const bool has_audio_decoder =
    hit_dt && hit_dt->decoder() && !hit_dt->decoder()->analog_data_copy().empty();
if (_export_decoder_wav_action)
    _export_decoder_wav_action->setVisible(has_audio_decoder);
if (_play_decoder_audio_action) {
    _play_decoder_audio_action->setVisible(has_audio_decoder);
    _play_decoder_audio_action->setText(
        DecoderAudioPlayer::instance().isPlaying()
            ? QStringLiteral("停止模拟音频播放")
            : QStringLiteral("播放模拟音频 / 多通道混音..."));
}

if (_clear_analog_measure_action)
    _clear_analog_measure_action->setVisible(_analog_measure_valid);
if (_configure_analog_measure_action) {
    const bool has_analog_decoder =
        hit_dt && hit_dt->decoder() &&
        !hit_dt->decoder()->analog_data_copy().empty();
    _configure_analog_measure_action->setVisible(has_analog_decoder);
}

_logic_cmenu->exec(QCursor::pos());
}

void Viewport::clear_analog_measurement() {
  _analog_measure_data.reset();
  _analog_measure_channel = -1;
  _analog_measure_start = 0;
  _analog_measure_end = 0;
  _analog_measure_stats = pv::data::DecoderAnalogStatistics{};
  _analog_measure_cycle = pv::data::DecoderAnalogCycleMetrics{};
  _analog_measure_valid = false;
  if (_action_type == ANALOG_RANGE_DRAG)
    set_action(NO_ACTION);
  setCursor(Qt::ArrowCursor);
  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::configure_analog_measurement() {
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("模拟波形测量显示项"));
  auto *layout = new QVBoxLayout(&dialog);

  auto make_check = [](QWidget *parent, const QString &text, bool checked) {
    auto *check = new QCheckBox(text, parent);
    check->setChecked(checked);
    return check;
  };

  auto *basic_group = new QGroupBox(QStringLiteral("鼠标点测"), &dialog);
  auto *basic_layout = new QGridLayout(basic_group);
  QCheckBox *show_channel = make_check(
      basic_group, QStringLiteral("通道"), _analog_measure_options.show_channel);
  QCheckBox *show_time = make_check(
      basic_group, QStringLiteral("时间 / 采样点"), _analog_measure_options.show_time);
  QCheckBox *show_normalized = make_check(
      basic_group, QStringLiteral("归一化值"), _analog_measure_options.show_normalized);
  QCheckBox *show_value = make_check(
      basic_group, QStringLiteral("工程值"),
      _analog_measure_options.show_engineering_value);
  basic_layout->addWidget(show_channel, 0, 0);
  basic_layout->addWidget(show_time, 0, 1);
  basic_layout->addWidget(show_normalized, 1, 0);
  basic_layout->addWidget(show_value, 1, 1);
  layout->addWidget(basic_group);

  auto *pulse_group = new QGroupBox(QStringLiteral("区间脉冲参数"), &dialog);
  auto *pulse_layout = new QGridLayout(pulse_group);
  QCheckBox *rise_time = make_check(
      pulse_group, QStringLiteral("上升时间 10%-90%"),
      _analog_measure_options.rise_time);
  QCheckBox *fall_time = make_check(
      pulse_group, QStringLiteral("下降时间 90%-10%"),
      _analog_measure_options.fall_time);
  QCheckBox *positive_overshoot = make_check(
      pulse_group, QStringLiteral("正过冲"),
      _analog_measure_options.positive_overshoot);
  QCheckBox *negative_overshoot = make_check(
      pulse_group, QStringLiteral("负过冲"),
      _analog_measure_options.negative_overshoot);
  pulse_layout->addWidget(rise_time, 0, 0);
  pulse_layout->addWidget(fall_time, 0, 1);
  pulse_layout->addWidget(positive_overshoot, 1, 0);
  pulse_layout->addWidget(negative_overshoot, 1, 1);
  layout->addWidget(pulse_group);

  auto *time_group = new QGroupBox(QStringLiteral("区间周期参数"), &dialog);
  auto *time_layout = new QGridLayout(time_group);
  QCheckBox *period = make_check(
      time_group, QStringLiteral("周期"), _analog_measure_options.period);
  QCheckBox *frequency = make_check(
      time_group, QStringLiteral("频率"), _analog_measure_options.frequency);
  QCheckBox *positive_width = make_check(
      time_group, QStringLiteral("正脉宽"), _analog_measure_options.positive_width);
  QCheckBox *negative_width = make_check(
      time_group, QStringLiteral("负脉宽"), _analog_measure_options.negative_width);
  QCheckBox *positive_duty = make_check(
      time_group, QStringLiteral("正占空比"),
      _analog_measure_options.positive_duty_cycle);
  QCheckBox *negative_duty = make_check(
      time_group, QStringLiteral("负占空比"),
      _analog_measure_options.negative_duty_cycle);
  time_layout->addWidget(period, 0, 0);
  time_layout->addWidget(frequency, 0, 1);
  time_layout->addWidget(positive_width, 1, 0);
  time_layout->addWidget(negative_width, 1, 1);
  time_layout->addWidget(positive_duty, 2, 0);
  time_layout->addWidget(negative_duty, 2, 1);
  layout->addWidget(time_group);

  auto *cycle_group = new QGroupBox(QStringLiteral("整周期统计"), &dialog);
  auto *cycle_layout = new QGridLayout(cycle_group);
  QCheckBox *cycle_rms = make_check(
      cycle_group, QStringLiteral("整周期 RMS"), _analog_measure_options.cycle_rms);
  cycle_layout->addWidget(cycle_rms, 0, 0);
  layout->addWidget(cycle_group);

  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
      Qt::Horizontal, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  if (dialog.exec() != QDialog::Accepted)
    return;

  _analog_measure_options.show_channel = show_channel->isChecked();
  _analog_measure_options.show_time = show_time->isChecked();
  _analog_measure_options.show_normalized = show_normalized->isChecked();
  _analog_measure_options.show_engineering_value = show_value->isChecked();
  _analog_measure_options.rise_time = rise_time->isChecked();
  _analog_measure_options.fall_time = fall_time->isChecked();
  _analog_measure_options.positive_overshoot = positive_overshoot->isChecked();
  _analog_measure_options.negative_overshoot = negative_overshoot->isChecked();
  _analog_measure_options.period = period->isChecked();
  _analog_measure_options.frequency = frequency->isChecked();
  _analog_measure_options.positive_width = positive_width->isChecked();
  _analog_measure_options.negative_width = negative_width->isChecked();
  _analog_measure_options.positive_duty_cycle = positive_duty->isChecked();
  _analog_measure_options.negative_duty_cycle = negative_duty->isChecked();
  _analog_measure_options.cycle_rms = cycle_rms->isChecked();
  save_analog_measurement_options(_analog_measure_options);
  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::copy_waveform_this_channel() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::ThisChannel);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::copy_waveform_decoder_track() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::DecoderTrack);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::copy_waveform_decoder_group() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::ThisDecoderGroup);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::copy_waveform_all_channels() {
  QString text = WaveformCopyHelper::format_range(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y(),
      WaveformCopyHelper::Scope::AllChannels);
  if (!text.isEmpty())
    QGuiApplication::clipboard()->setText(text);
}

void Viewport::export_decoder_audio_wav() {
  DecodeTrace *dt = WaveformCopyHelper::hit_test_decode_trace(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y());
  if (!dt) {
    MsgBox::Show(QStringLiteral("鼠标位置没有可用的模拟解码器，请在解码器轨道上右键。"));
    return;
  }

  auto stack = dt->decoder();
  if (!stack)
    return;

  auto analog_data = stack->analog_data_copy();
  if (analog_data.empty()) {
    MsgBox::Show(QStringLiteral("该解码器没有产生模拟/音频数据。"));
    return;
  }

  uint64_t logic_rate = stack->sample_rate();
  uint32_t derived_rate = 0;
  for (const auto &ad : analog_data) {
    if (!ad || ad->get_sample_count() < 2)
      continue;
    auto sample_view = ad->read_samples();
    const auto &samples = sample_view.samples();
    uint64_t span = samples.back().start_sample - samples.front().start_sample;
    if (span > 0) {
      double interval = (double)span / (double)(samples.size() - 1);
      if (interval >= 1.0 && logic_rate > 0) {
        derived_rate = (uint32_t)((double)logic_rate / interval);
        break;
      }
    }
  }
  if (derived_rate == 0 && logic_rate > 0 && logic_rate <= UINT32_MAX)
    derived_rate = (uint32_t)logic_rate;
  if (derived_rate == 0)
    derived_rate = 48000;

  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("导出模拟音频 WAV"));
  auto *form = new QFormLayout(&dlg);

  QComboBox *rate_combo = new QComboBox(&dlg);
  const uint32_t presets[] = {8000, 11025, 16000, 22050, 32000, 44100,
                              48000, 88200, 96000, 176400, 192000};
  int default_idx = 0;
  bool found_preset = false;
  for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
    rate_combo->addItem(QString("%1 Hz").arg(presets[i]), presets[i]);
    if (presets[i] == derived_rate) {
      default_idx = (int)i;
      found_preset = true;
    }
  }
  if (!found_preset) {
    rate_combo->insertItem(0, QString("%1 Hz (auto)").arg(derived_rate),
                           derived_rate);
    default_idx = 0;
  }
  rate_combo->setCurrentIndex(default_idx);
  form->addRow(QStringLiteral("采样率"), rate_combo);

  QComboBox *bits_combo = new QComboBox(&dlg);
  bits_combo->addItem("8 bit", 8);
  bits_combo->addItem("16 bit", 16);
  bits_combo->addItem("24 bit", 24);
  bits_combo->addItem("32 bit", 32);
  bits_combo->setCurrentIndex(1);
  form->addRow(QStringLiteral("位深"), bits_combo);

  QWidget *ch_widget = new QWidget(&dlg);
  QVBoxLayout *ch_layout = new QVBoxLayout(ch_widget);
  ch_layout->setContentsMargins(0, 0, 0, 0);
  std::vector<QCheckBox*> ch_boxes;
  std::vector<int> ch_indices;
  for (const auto &ad : analog_data) {
    if (!ad)
      continue;
    auto *cb = new QCheckBox(
        QString("Ch%1").arg(ad->channel()), ch_widget);
    cb->setChecked(ad->visible());
    ch_layout->addWidget(cb);
    ch_boxes.push_back(cb);
    ch_indices.push_back(ad->channel());
  }
  form->addRow(QStringLiteral("导出通道"), ch_widget);

  QCheckBox *wav_mix_enable = new QCheckBox(QStringLiteral("使用多通道混音矩阵"), &dlg);
  form->addRow(wav_mix_enable);

  QComboBox *wav_out_channels = new QComboBox(&dlg);
  for (int channels = 1; channels <= 8; ++channels)
    wav_out_channels->addItem(QStringLiteral("%1 ch").arg(channels), channels);
  wav_out_channels->setCurrentIndex(1);
  wav_out_channels->setEnabled(false);
  form->addRow(QStringLiteral("混音输出"), wav_out_channels);

  QGroupBox *wav_mix_box = new QGroupBox(QStringLiteral("WAV 混音矩阵"), &dlg);
  auto *wav_mix_layout = new QVBoxLayout(wav_mix_box);
  auto *wav_matrix = new QTableWidget(wav_mix_box);
  wav_matrix->setColumnCount(9);
  wav_matrix->setRowCount(0);
  wav_matrix->setEditTriggers(QAbstractItemView::NoEditTriggers);
  wav_matrix->setSelectionMode(QAbstractItemView::NoSelection);
  wav_matrix->verticalHeader()->setDefaultSectionSize(28);
  QStringList wav_headers;
  wav_headers << QStringLiteral("启用");
  for (int output = 0; output < 8; ++output) {
    QString label = QStringLiteral("O%1 %").arg(output + 1);
    if (output == 0) label = QStringLiteral("L/O1 %");
    if (output == 1) label = QStringLiteral("R/O2 %");
    wav_headers << label;
  }
  wav_matrix->setHorizontalHeaderLabels(wav_headers);

  std::vector<QCheckBox *> wav_mix_on;
  std::vector<std::array<QSpinBox *, 8>> wav_mix_gains;
  std::vector<int> wav_mix_ch;
  for (const auto &ad : analog_data) {
    if (!ad) continue;
    const int ch = ad->channel();
    const int row = wav_matrix->rowCount();
    wav_matrix->insertRow(row);
    wav_matrix->setVerticalHeaderItem(row,
        new QTableWidgetItem(QStringLiteral("CH%1").arg(ch)));
    auto *on = new QCheckBox(wav_matrix);
    on->setChecked(ad->visible());
    std::array<QSpinBox *, 8> gains{};
    for (int output = 0; output < 8; ++output) {
      auto *gain = new QSpinBox(wav_matrix);
      gain->setRange(0, 100);
      gain->setSuffix("%");
      const int default_output = (ch >= 0) ? (ch % 2) : 0;
      const int sources_per_side = std::max(
          1, (static_cast<int>(analog_data.size()) + 1) / 2);
      const int default_gain = std::max(1, 100 / sources_per_side);
      gain->setValue(output == default_output ? default_gain : 0);
      wav_matrix->setCellWidget(row, output + 1, gain);
      gains[(size_t)output] = gain;
    }
    wav_matrix->setCellWidget(row, 0, on);
    wav_mix_on.push_back(on);
    wav_mix_gains.push_back(gains);
    wav_mix_ch.push_back(ch);
  }

  auto update_wav_columns = [wav_matrix, wav_out_channels](int) {
    const int outputs = wav_out_channels->currentData().toInt();
    for (int output = 0; output < 8; ++output)
      wav_matrix->setColumnHidden(output + 1, output >= outputs);
    wav_matrix->resizeColumnsToContents();
  };
  QObject::connect(wav_out_channels, &QComboBox::currentIndexChanged,
                   &dlg, update_wav_columns);
  QObject::connect(wav_mix_enable, &QCheckBox::toggled, &dlg,
                   [ch_widget, wav_out_channels, wav_mix_box](bool on) {
                     ch_widget->setEnabled(!on);
                     wav_out_channels->setEnabled(on);
                     wav_mix_box->setEnabled(on);
                   });
  update_wav_columns(wav_out_channels->currentIndex());
  wav_matrix->setMinimumHeight(wav_matrix->rowCount() * 30 + 30);
  wav_matrix->setMinimumWidth(620);
  wav_mix_layout->addWidget(wav_matrix);
  wav_mix_box->setEnabled(false);
  form->addRow(wav_mix_box);

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted,
                   &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected,
                   &dlg, &QDialog::reject);

  if (dlg.exec() != QDialog::Accepted)
    return;

  WaveformCopyHelper::WavExportConfig cfg;
  cfg.sample_rate = rate_combo->currentData().toUInt();
  cfg.bits = bits_combo->currentData().toInt();
  if (wav_mix_enable->isChecked()) {
    cfg.output_channels = wav_out_channels->currentData().toInt();
    for (size_t i = 0; i < wav_mix_on.size(); ++i) {
      if (!wav_mix_on[i]->isChecked()) continue;
      WaveformCopyHelper::WavExportConfig::MixRow row;
      row.channel = wav_mix_ch[i];
      bool routed = false;
      for (int output = 0; output < cfg.output_channels; ++output) {
        row.outputs[(size_t)output] =
            wav_mix_gains[i][(size_t)output]->value() / 100.0f;
        routed = routed || row.outputs[(size_t)output] > 0.0f;
      }
      row.enabled = routed;
      if (routed) cfg.mix.push_back(row);
    }
    if (cfg.mix.empty()) {
      MsgBox::Show(QStringLiteral("混音矩阵没有启用任何有效路由"));
      return;
    }
  } else {
    for (size_t i = 0; i < ch_boxes.size(); i++) {
      if (ch_boxes[i]->isChecked())
        cfg.channel_indices.push_back(ch_indices[i]);
    }
  }

  QString default_name = QString("decoder_audio_%1Hz_%2bit.wav")
                             .arg(cfg.sample_rate)
                             .arg(cfg.bits);
  QString filepath = QFileDialog::getSaveFileName(
      this, QStringLiteral("保存 WAV 音频文件"),
      default_name, "WAV Audio (*.wav)");
  if (filepath.isEmpty())
    return;

  QString message;
  if (!WaveformCopyHelper::export_decoder_audio_wav(
          dt, filepath, cfg, message)) {
    MsgBox::Show(message);
    return;
  }

  QDesktopServices::openUrl(
      QUrl::fromLocalFile(QFileInfo(filepath).absolutePath()));
}

void Viewport::play_decoder_audio() {
  if (DecoderAudioPlayer::instance().isPlaying()) {
    DecoderAudioPlayer::instance().stop();
    return;
  }

  DecodeTrace *dt = WaveformCopyHelper::hit_test_decode_trace(
      _view, _logic_menu_pos.x(), _logic_menu_pos.y());
  if (!dt) {
    MsgBox::Show(QStringLiteral("鼠标位置没有可用的模拟解码器，请在解码器轨道上右键。"));
    return;
  }

  auto stack = dt->decoder();
  if (!stack)
    return;

  auto analog_data = stack->analog_data_copy();
  if (analog_data.empty()) {
    MsgBox::Show(QStringLiteral("该解码器没有产生模拟/音频数据。"));
    return;
  }

  uint64_t logic_rate = stack->sample_rate();
  uint32_t derived_rate = 0;
  for (const auto &ad : analog_data) {
    if (!ad || ad->get_sample_count() < 2)
      continue;
    auto sample_view = ad->read_samples();
    const auto &samples = sample_view.samples();
    uint64_t span = samples.back().start_sample - samples.front().start_sample;
    if (span > 0) {
      double interval = (double)span / (double)(samples.size() - 1);
      if (interval >= 1.0 && logic_rate > 0) {
        derived_rate = (uint32_t)((double)logic_rate / interval);
        break;
      }
    }
  }
  if (derived_rate == 0 && logic_rate > 0 && logic_rate <= UINT32_MAX)
    derived_rate = (uint32_t)logic_rate;
  if (derived_rate == 0)
    derived_rate = 48000;

  DecoderAudioPlayer::PlayConfig saved_cfg;
  DecoderAudioPlayer::loadPlayConfig(saved_cfg);

  QDialog dlg(this);
  dlg.setWindowTitle(QStringLiteral("模拟音频播放 / 多通道混音"));
  auto *form = new QFormLayout(&dlg);

  QComboBox *device_combo = new QComboBox(&dlg);
  {
    const auto devices = DecoderAudioPlayer::listOutputDevices();
    int saved_idx = 0;
    for (size_t i = 0; i < devices.size(); i++) {
      const QString label = QStringLiteral("%1 (%2 ch)")
                                .arg(devices[i].name)
                                .arg(devices[i].max_channels);
      device_combo->addItem(label, devices[i].id);
      device_combo->setItemData((int)i, devices[i].max_channels,
                                Qt::UserRole + 1);
      if (devices[i].id == saved_cfg.device_id)
        saved_idx = (int)i;
    }
    device_combo->setCurrentIndex(saved_idx);
  }
  form->addRow(QStringLiteral("输出设备"), device_combo);

  QComboBox *rate_combo = new QComboBox(&dlg);
  const uint32_t presets[] = {8000, 11025, 16000, 22050, 32000, 44100,
                              48000, 88200, 96000, 176400, 192000};
  int default_idx = 0;
  bool found_preset = false;
  uint32_t default_rate = saved_cfg.sample_rate > 0
                              ? saved_cfg.sample_rate : derived_rate;
  for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
    rate_combo->addItem(QString("%1 Hz").arg(presets[i]), presets[i]);
    if (presets[i] == default_rate) {
      default_idx = (int)i;
      found_preset = true;
    }
  }
  if (!found_preset) {
    rate_combo->insertItem(0, QString("%1 Hz").arg(default_rate),
                           default_rate);
    default_idx = 0;
  }
  rate_combo->setCurrentIndex(default_idx);
  form->addRow(QStringLiteral("采样率"), rate_combo);

  QComboBox *bits_combo = new QComboBox(&dlg);
  bits_combo->addItem("8 bit", 8);
  bits_combo->addItem("16 bit", 16);
  bits_combo->addItem("24 bit", 24);
  bits_combo->addItem("32 bit", 32);
  {
    const int b = saved_cfg.bits;
    const int idx = (b == 8 || b == 16 || b == 24 || b == 32)
                        ? bits_combo->findData(b) : 1;
    bits_combo->setCurrentIndex(idx >= 0 ? idx : 1);
  }
  form->addRow(QStringLiteral("位深"), bits_combo);

  QComboBox *ch_mode_combo = new QComboBox(&dlg);
  auto refresh_output_channels = [device_combo, ch_mode_combo](int preferred) {
    const int device_max = qBound(
        1, device_combo->currentData(Qt::UserRole + 1).toInt(), 8);
    QSignalBlocker blocker(ch_mode_combo);
    ch_mode_combo->clear();
    for (int channels = 1; channels <= device_max; ++channels) {
      ch_mode_combo->addItem(QStringLiteral("%1 ch").arg(channels),
                             channels);
    }
    const int selected =
        (preferred >= 1 && preferred <= device_max) ? preferred : device_max;
    ch_mode_combo->setCurrentIndex(ch_mode_combo->findData(selected));
  };
  refresh_output_channels(saved_cfg.channels);
  QObject::connect(device_combo, &QComboBox::currentIndexChanged, &dlg,
                   [refresh_output_channels](int) {
                     refresh_output_channels(0);
                   });
  form->addRow(QStringLiteral("输出通道"), ch_mode_combo);

  QCheckBox *repeat_check = new QCheckBox(
      QStringLiteral("循环播放"),
      &dlg);
  repeat_check->setChecked(saved_cfg.repeat);
  form->addRow(repeat_check);

  QGroupBox *mix_box = new QGroupBox(
      QStringLiteral("多通道混音矩阵"),
      &dlg);
  QVBoxLayout *mix_layout = new QVBoxLayout(mix_box);

  auto *matrix = new QTableWidget(&dlg);
  matrix->setColumnCount(9);
  matrix->setRowCount(0);
  matrix->horizontalHeader()->setStretchLastSection(false);
  matrix->verticalHeader()->setDefaultSectionSize(28);
  matrix->setEditTriggers(QAbstractItemView::NoEditTriggers);
  matrix->setSelectionMode(QAbstractItemView::NoSelection);

  QStringList matrix_headers;
  matrix_headers << QStringLiteral("启用");
  for (int output = 0; output < 8; ++output) {
    QString name = QStringLiteral("OUT%1 %").arg(output + 1);
    if (output == 0)
      name = QStringLiteral("OUT1 (L) %");
    else if (output == 1)
      name = QStringLiteral("OUT2 (R) %");
    matrix_headers << name;
  }
  matrix->setHorizontalHeaderLabels(matrix_headers);

  std::vector<QCheckBox *> mix_on;
  std::vector<std::array<QSpinBox *, 8>> mix_gains;
  std::vector<int>         mix_ch;

  for (const auto &ad : analog_data) {
    if (!ad)
      continue;
    const int ch = ad->channel();

    const DecoderAudioPlayer::PlayConfig::MixRow *saved_row = nullptr;
    for (const auto &sr : saved_cfg.mix) {
      if (sr.channel == ch) {
        saved_row = &sr;
        break;
      }
    }

    const int row = matrix->rowCount();
    matrix->insertRow(row);
    matrix->setVerticalHeaderItem(row, new QTableWidgetItem(
        QString("Ch%1").arg(ch)));

    auto *on = new QCheckBox(matrix);
    std::array<QSpinBox *, 8> gains{};
    for (int output = 0; output < 8; ++output) {
      gains[(size_t)output] = new QSpinBox(matrix);
      gains[(size_t)output]->setRange(0, 100);
      gains[(size_t)output]->setSuffix("%");
    }

    if (saved_row) {
      on->setChecked(saved_row->enabled);
      for (int output = 0; output < 8; ++output) {
        gains[(size_t)output]->setValue(
            (int)(saved_row->outputs[(size_t)output] * 100.0f));
      }
    } else {
      const int initial_outputs = qBound(1, ch_mode_combo->currentData().toInt(), 8);
      const int sources_per_bus = std::max(
          1, (static_cast<int>(analog_data.size()) + initial_outputs - 1) / initial_outputs);
      const int default_gain = std::max(1, 100 / sources_per_bus);
      const int default_output = ch >= 0 ? (ch % initial_outputs) : 0;
      on->setChecked(ad->visible());
      for (int output = 0; output < 8; ++output)
        gains[(size_t)output]->setValue(
            output == default_output ? default_gain : 0);
    }

    matrix->setCellWidget(row, 0, on);
    for (int output = 0; output < 8; ++output)
      matrix->setCellWidget(row, output + 1, gains[(size_t)output]);

    mix_on.push_back(on);
    mix_gains.push_back(gains);
    mix_ch.push_back(ch);
  }

  auto update_matrix_columns = [matrix, ch_mode_combo](int) {
    const int outputs = ch_mode_combo->currentData().toInt();
    for (int output = 0; output < 8; ++output)
      matrix->setColumnHidden(output + 1, output >= outputs);
    matrix->resizeColumnsToContents();
  };
  QObject::connect(ch_mode_combo, &QComboBox::currentIndexChanged, &dlg,
                   update_matrix_columns);
  QObject::connect(device_combo, &QComboBox::currentIndexChanged, &dlg,
                   update_matrix_columns);
  update_matrix_columns(ch_mode_combo->currentIndex());

  matrix->setMinimumHeight(matrix->rowCount() * 30 + 30);
  matrix->setMinimumWidth(640);
  mix_layout->addWidget(matrix);
  form->addRow(mix_box);

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dlg);
  form->addRow(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted,
                   &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected,
                   &dlg, &QDialog::reject);

  if (dlg.exec() != QDialog::Accepted)
    return;

  DecoderAudioPlayer::PlayConfig cfg;
  cfg.sample_rate = rate_combo->currentData().toUInt();
  cfg.bits = bits_combo->currentData().toInt();
  cfg.channels = ch_mode_combo->currentData().toInt();
  cfg.device_id = device_combo->currentData().toInt();
  cfg.repeat = repeat_check->isChecked();

  const int output_channels = cfg.channels;
  for (size_t i = 0; i < mix_on.size(); i++) {
    if (!mix_on[i]->isChecked())
      continue;
    DecoderAudioPlayer::PlayConfig::MixRow row;
    row.channel = mix_ch[i];
    bool routed = false;
    for (int output = 0; output < output_channels; ++output) {
      row.outputs[(size_t)output] =
          mix_gains[i][(size_t)output]->value() / 100.0f;
      routed = routed || row.outputs[(size_t)output] > 0.0f;
    }
    row.enabled = routed;
    if (routed)
      cfg.mix.push_back(row);
  }

  if (cfg.mix.empty()) {
    for (const auto &ad : analog_data) {
      if (ad && ad->visible())
        cfg.channel_indices.push_back(ad->channel());
    }
  }

  QString message;
  if (!DecoderAudioPlayer::instance().playDecoder(dt, cfg, message)) {
    MsgBox::Show(message);
    return;
  }

  DecoderAudioPlayer::savePlayConfig(cfg);

  pxv_info("Decoder audio playback started: %u Hz, %d bit, mode=%d, "
           "mix=%d row(s), repeat=%d",
           cfg.sample_rate, cfg.bits, cfg.channels, (int)cfg.mix.size(),
           cfg.repeat ? 1 : 0);
}

void Viewport::UpdateLanguage() {
  _yAction->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_Y_CURSOR), "Add Y-cursor"));
  _xAction->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_ADD_X_CURSOR), "Add X-cursor"));
  _copy_this_channel_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_THIS_CHANNEL), "Copy Channel Waveform Data"));
  _copy_decoder_track_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_TRACK), "Copy Decoder Waveform Data"));
  _copy_decoder_group_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_DECODER_GROUP), "Copy Decoder Group Waveform Data"));
  _copy_all_channels_action->setText(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_COPY_ALL_CHANNELS), "Copy All Channels Waveform Data"));
}

void Viewport::UpdateTheme() {
  _panelBgColor = AppConfig::Instance().GetThemeColor("@panel-bg");
  if (!_panelBgColor.isValid())
    _panelBgColor = QColor("#1a1a1a");
  _panelTextColor = AppConfig::Instance().GetThemeColor("@panel-text");
  if (!_panelTextColor.isValid())
    _panelTextColor = QColor("#f5f0e5");

  _prev_edge_btn->UpdateTheme();
  _next_edge_btn->UpdateTheme();

  update(UpdateEventType::UPDATE_EV_GENERIC);
}

void Viewport::UpdateFont() {
  QFont font = theme_font_cursor();
  _yAction->setFont(font);
  _xAction->setFont(font);
}

int Viewport::get_fps() { return _fps; }

} // namespace view
} // namespace pv
