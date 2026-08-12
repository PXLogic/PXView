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

#include "pv/view/component/header.h"

#include <QApplication>
#include <QColorDialog>
#include <QFont>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QScrollBar>
#include <QScreen>
#include <QStyleOption>
#include <QGuiApplication>
#include <algorithm>
#include <cassert>
#include <set>

#include "pv/mainwindow/appcontrol.h"
#include "pv/config/appconfig.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/fn.h"
#include "pv/ui/langresource.h"
#include "pv/view/signal/analogsignal.h"
#include "pv/view/trace/decodetrace.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/trace/mathtrace.h"
#include "pv/view/trace/trace.h"
#include "pv/view/view.h"

using namespace std;

namespace pv {
namespace view {

Header::Header(View &parent) : QWidget(&parent), _view(parent) {
  _moveFlag = false;
  _colorFlag = false;
  _nameFlag = false;
  _context_trace = nullptr;
  _resize_trace_upper = nullptr;
  _resize_trace_lower = nullptr;
  _resize_mouse_down_y = 0;
  _resize_upper_height = 0;
  _resize_lower_height = 0;
  _mouse_is_down = false;
  _foreColor = QColor();  // 无效色,UpdateTheme 会填充

  nameEdit = new PopupLineEdit(this);
  nameEdit->setFixedWidth(100);
  nameEdit->hide();
  nameEdit->set_instant_mode(true);

  setMouseTracking(true);

  connect(nameEdit, &QLineEdit::editingFinished, this,
          &Header::on_action_set_name_triggered);

  ADD_UI(this);
}

Header::~Header() { REMOVE_UI(this); }

void Header::retranslateUi() { update(); }

int Header::get_nameEditWidth() {
  if (nameEdit->hasFocus())
    return nameEdit->width();
  else
    return 0;
}

pv::view::Trace *Header::get_mTrace(int &action, const QPoint &pt) {
  // pt MUST be in absolute content coordinates (i.e. viewport-relative
  // position + vertical scroll offset). Both get_y() and get_zero_vpos()
  // (used by pt_in_rect for the color/name/label hit-test rects) return
  // absolute coordinates. In DSO mode there is no vertical scroll, so
  // absolute == viewport-relative.
  //
  // Callers:
  // - mousePressEvent / mouseReleaseEvent: pass event->position() + vOffset
  // - contextMenuEvent: already passes event->pos() + vOffset
  const int w = width();
  std::vector<Trace *> traces;
  _view.get_traces(ALL_VIEW, traces);

  for (auto t : traces) {
    action = t->pt_in_rect(t->get_y(), w, pt);
    if (action)
      return t;
  }

  return nullptr;
}

void Header::paintEvent(QPaintEvent *) {
  using pv::view::Trace;

  QStyleOption o;
  o.initFrom(this);
  QPainter painter(this);
  style()->drawPrimitive(QStyle::PE_Widget, &o, &painter, this);

  const int w = width();

  std::vector<Trace *> traces;
  _view.get_traces(ALL_VIEW, traces);

  const bool dragging = !_drag_traces.empty();
  // 优先用 UpdateTheme() 缓存的主题色;主题未加载时回退 palette
  QColor fore = _foreColor.isValid()
                    ? _foreColor
                    : QWidget::palette().color(QWidget::foregroundRole());
  fore.setAlpha(View::ForeAlpha);

  QFont font = theme_font_trace_label();
  painter.setFont(font);
  painter.setRenderHint(QPainter::TextAntialiasing, false);

  painter.save();
  if (_view.get_work_mode() != DSO) {
    painter.translate(0, -_view.get_vOffset());
  }

  if (_view.is_logic_rendering_mode()) {
    const auto &groups = _view.get_signal_groups();
    if (!groups.empty()) {
      std::vector<size_t> group_indices(groups.size());
      for (size_t i = 0; i < groups.size(); i++)
        group_indices[i] = i;
      std::sort(group_indices.begin(), group_indices.end(),
                [&groups](size_t a, size_t b) {
                  if (groups[a].traces.empty())
                    return false;
                  if (groups[b].traces.empty())
                    return true;
                  return groups[a].traces[0]->get_v_offset() <
                         groups[b].traces[0]->get_v_offset();
                });

      for (size_t idx = 0; idx < group_indices.size(); idx++) {
        const auto &group = groups[group_indices[idx]];
        if (group.traces.empty())
          continue;
        double groupTop = 1e9;
        double groupBottom = -1e9;
        for (auto gt : group.traces) {
          // 跳过 disabled 通道：其 v_offset 未被 layout 更新，
          // 会干扰分组卡片的边界计算
          if (!gt->enabled() && !gt->as_dso())
            continue;
          double traceTop = gt->get_v_offset() - gt->get_totalHeight() * 0.5 -
                            View::SignalMargin;
          double traceBottom = gt->get_v_offset() +
                               gt->get_totalHeight() * 0.5 + View::SignalMargin;
          groupTop = min(groupTop, traceTop);
          groupBottom = max(groupBottom, traceBottom);
        }
        // 全部 disabled 时跳过该组卡片
        if (groupTop > groupBottom)
          continue;

        double cardTop = groupTop - View::GroupGap * 0.5;
        double cardHeight = groupBottom - groupTop + View::GroupGap;

        QRectF cardRect(0, cardTop, w + View::GroupCardRadius + 1, cardHeight);
        QPainterPath groupPath;
        groupPath.addRoundedRect(cardRect, View::GroupCardRadius, View::GroupCardRadius);

        if (_view.is_colored_card_mode()) {
          painter.save();
          painter.setClipPath(groupPath);
          painter.setPen(Qt::NoPen);
          
          // 预计算第一个和最后一个 enabled trace 的索引，
          // 用于正确应用 GroupGap 边缘扩展
          int firstEnabled = -1, lastEnabled = -1;
          for (size_t i = 0; i < group.traces.size(); i++) {
            auto gt = group.traces[i];
            if (gt->enabled() || gt->as_dso()) {
              if (firstEnabled < 0)
                firstEnabled = (int)i;
              lastEnabled = (int)i;
            }
          }
          for (size_t i = 0; i < group.traces.size(); i++) {
            auto gt = group.traces[i];
            // 跳过 disabled 通道
            if (!gt->enabled() && !gt->as_dso())
              continue;
            double tTop = gt->get_v_offset() - gt->get_totalHeight() * 0.5 - View::SignalMargin;
            double tBottom = gt->get_v_offset() + gt->get_totalHeight() * 0.5 + View::SignalMargin;
            
            if ((int)i == firstEnabled) tTop -= View::GroupGap * 0.5;
            if ((int)i == lastEnabled) tBottom += View::GroupGap * 0.5;
            
            QRectF traceRect(0, tTop, w + View::GroupCardRadius + 1, tBottom - tTop);
            painter.setBrush(_view.get_trace_card_color(gt));
            painter.drawRect(traceRect);
          }
          painter.restore();
        } else {
          painter.setPen(Qt::NoPen);
          painter.setBrush(_view.get_group_card_color());
          painter.drawPath(groupPath);
        }
      }
    }
  }

  QColor dividerColor = AppConfig::Instance().GetThemeColor("@border-strong");
  if (!dividerColor.isValid()) {
    QColor back(QWidget::palette().color(QWidget::backgroundRole()));
    double lum =
        back.red() * 0.299 + back.green() * 0.587 + back.blue() * 0.114;
    dividerColor =
        lum < 128 ? QColor(0x37, 0x37, 0x3b) : QColor(0xd5, 0xd5, 0xd5);
  }

  std::set<Trace *> lastInGroup;
  if (_view.is_logic_rendering_mode()) {
    const auto &groups = _view.get_signal_groups();
    for (const auto &group : groups) {
      if (group.traces.empty())
        continue;
      Trace *last = nullptr;
      for (auto gt : group.traces) {
        if (gt->enabled())
          last = gt;
      }
      if (last)
        lastInGroup.insert(last);
    }
  }

  // Find the last enabled trace (no divider below it)
  Trace *lastEnabledTrace = nullptr;
  for (auto it = traces.rbegin(); it != traces.rend(); ++it) {
    if ((*it)->enabled() || (*it)->as_dso()) {
      lastEnabledTrace = *it;
      break;
    }
  }

  painter.setPen(QPen(dividerColor, 1));
  for (auto t : traces) {
    if (!t->enabled() && !t->as_dso())
      continue;
    if (lastInGroup.count(t))
      continue;
    if (t == lastEnabledTrace)
      continue;
    int traceBottom =
        t->get_v_offset() + t->get_totalHeight() / 2 + View::SignalMargin;
    painter.drawLine(35, traceBottom, w, traceBottom);
  }

  for (auto t : traces) {
    t->paint_label(painter, w, dragging ? QPoint(-1, -1) : _mouse_point, fore);
  }

  painter.restore();
  painter.end();
}

void Header::mouseDoubleClickEvent(QMouseEvent *event) {
  assert(event);

  std::vector<Trace *> traces;

  _view.get_traces(ALL_VIEW, traces);

  if (_view.is_logic_rendering_mode()) {
    int mouseY = event->position().toPoint().y() + _view.get_vOffset();
    const int HitBorderMargin = 5;

    std::vector<Trace *> enabled_traces;
    for (auto t : traces)
      if (t->enabled())
        enabled_traces.push_back(t);

    for (int i = 0; i < (int)enabled_traces.size() - 1; i++) {
      int traceBottom =
          enabled_traces[i]->get_v_offset() +
          enabled_traces[i]->get_totalHeight() / 2 + View::SignalMargin;

      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        enabled_traces[i]->set_own_height(-1);
        enabled_traces[i + 1]->set_own_height(-1);
        _view.signals_changed(nullptr);
        return;
      }
    }

    // Check bottom border of the last enabled trace
    if (!enabled_traces.empty()) {
      Trace *lastTrace = enabled_traces.back();
      int traceBottom = lastTrace->get_v_offset() +
                        lastTrace->get_totalHeight() / 2 + View::SignalMargin;
      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        lastTrace->set_own_height(-1);
        _view.signals_changed(nullptr);
        return;
      }
    }
  }

  if (event->button() & Qt::LeftButton) {
    _mouse_down_point = event->position().toPoint();

    // Save the offsets of any Traces which will be dragged
    for (auto t : traces) {
      if (t->selected())
        _drag_traces.push_back(make_pair(t, t->get_v_offset()));
    }

    // Select the Trace if it has been clicked
    for (auto t : traces) {
      if (t->mouse_double_click(width(), event->position().toPoint()))
        break;
    }
  }
}

void Header::mousePressEvent(QMouseEvent *event) {
  assert(event);

  _mouse_is_down = true;

  std::vector<Trace *> traces;
  _view.get_traces(ALL_VIEW, traces);
  int action;

  // DSO 模式下 vDial/ACDC/EN 等控件在采集运行时也需要可调节
  // (旧逻辑在 instant+running 时直接 return 阻止所有 Header 鼠标交互，
  //  导致 DSO 模式下 vDial 不能转动)。仅对 LOGIC/MSO 模式保留此守卫。
  const bool instant = _view.session().is_instant();
  const bool is_dso_mode = (_view.get_work_mode() == DSO);
  const bool is_running = _view.session().is_running_status();
  if (instant && is_running && !is_dso_mode) {
    pxv_info("Header::mousePressEvent: blocked by instant+running guard "
             "(instant=%d, running=%d, dso=%d)", instant, is_running, is_dso_mode);
    return;
  }
  pxv_info("Header::mousePressEvent: passed guard (instant=%d, running=%d, dso=%d, "
           "traces=%d, button=%d, pos=(%d,%d))",
           instant, is_running, is_dso_mode, (int)traces.size(),
           (int)event->button(), event->position().toPoint().x(),
           event->position().toPoint().y());

  if (_view.is_logic_rendering_mode()) {
    std::vector<Trace *> traces;
    _view.get_traces(ALL_VIEW, traces);
    int mouseY = event->position().toPoint().y() + _view.get_vOffset();
    const int HitBorderMargin = 5;

    std::vector<Trace *> enabled_traces;
    for (auto t : traces)
      if (t->enabled())
        enabled_traces.push_back(t);

    for (int i = 0; i < (int)enabled_traces.size() - 1; i++) {
      int traceBottom =
          enabled_traces[i]->get_v_offset() +
          enabled_traces[i]->get_totalHeight() / 2 + View::SignalMargin;

      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        _resize_trace_upper = enabled_traces[i];
        _resize_trace_lower = enabled_traces[i + 1];
        _resize_mouse_down_y = event->position().toPoint().y();
        _resize_upper_height = enabled_traces[i]->get_totalHeight();
        _resize_lower_height = enabled_traces[i + 1]->get_totalHeight();
        return;
      }
    }

    // Check bottom border of the last enabled trace
    if (!enabled_traces.empty()) {
      Trace *lastTrace = enabled_traces.back();
      int traceBottom = lastTrace->get_v_offset() +
                        lastTrace->get_totalHeight() / 2 + View::SignalMargin;
      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        _resize_trace_upper = lastTrace;
        _resize_trace_lower = nullptr;
        _resize_mouse_down_y = event->position().toPoint().y();
        _resize_upper_height = lastTrace->get_totalHeight();
        _resize_lower_height = 0;
        return;
      }
    }
  }

  if (event->button() & Qt::LeftButton) {
    // Store viewport-relative press position (NOT + vOffset). The delta in
    // mouseMoveEvent is computed as (current_y - _mouse_down_point.y()),
    // where current_y is also viewport-relative. Adding vOffset here would
    // make the delta off by -vOffset, causing traces to jump or move in
    // the wrong direction when vertically scrolled.
    _mouse_down_point = event->position().toPoint();

    // Save the offsets of any Traces which will be dragged
    for (auto t : traces) {
      if (t->selected())
        _drag_traces.push_back(make_pair(t, t->get_v_offset()));
    }

    // Select the Trace if it has been clicked.
    // get_mTrace requires absolute coordinates (pt + vOffset) because
    // pt_in_rect uses get_y() and get_zero_vpos() which are absolute.
    const int vOff_hit = (_view.get_work_mode() != DSO) ? _view.get_vOffset() : 0;
    const auto mTrace = get_mTrace(action, event->position().toPoint() + QPoint(0, vOff_hit));
    if (action == Trace::COLOR && mTrace) {
      // 解码通道:单击 COLOR 区打开解码器设置对话框(与 ProtocolDock 齿轮入口一致)
      if (auto *dt = mTrace->as_decode()) {
        _context_trace = mTrace;
        // 模态对话框(QDialog::exec)会捕获鼠标,导致后续 mouseReleaseEvent 不会被调用,
        // 必须在此重置按下态/拖拽缓存,否则 header_is_draging() 恒为 true,
        // 进而阻断 viewport 的 wheelEvent 缩放。
        _mouse_is_down = false;
        _drag_traces.clear();
        // 锚点定位(与毛刺滤波浮窗相同的弹出逻辑):基于 DecodeTrace 位置计算,
        // 映射到全局坐标并按屏幕边界钳制,避免 QDialog 默认居中。
        int name_right = width() - dt->get_rightWidth();
        int anchor_x = name_right + 8;
        int anchor_y = dt->get_y() - dt->get_totalHeight() / 2;
        QPoint anchor = mapToGlobal(QPoint(anchor_x, anchor_y));
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
        _view.rst_decoder_by_key_handel(dt->get_key_handel(), anchor);
        return;
      }
      // LOGIC 模式:单击 COLOR 区直接打开滤波浮窗(对齐 HTML 原型交互)
      // ANALOG/DSO 模式:保留原选色流程(主题菜单的 token 改色只覆盖
      // LOGIC 全局色板,模拟/DSO 单通道颜色仍需此入口)
      if (_view.is_logic_rendering_mode()) {
        auto *sig = mTrace->as_logic();
        if (sig) {
          _context_trace = mTrace;
          // 同上:弹窗捕获鼠标,需重置按下态,避免滚轮缩放被锁死。
          _mouse_is_down = false;
          _drag_traces.clear();
          emit show_glitch_filter_popup(sig);
          return;
        }
      }
      _colorFlag = true;
    } else if (action == Trace::NAME && mTrace) {
      _nameFlag = true;
      mTrace->select(true);

      if (~QApplication::keyboardModifiers() & Qt::ControlModifier)
        _drag_traces.clear();

      _drag_traces.push_back(make_pair(mTrace, mTrace->get_zero_vpos()));
      mTrace->set_old_v_offset(mTrace->get_v_offset());
    } else if (action == Trace::LABEL && mTrace) {
      mTrace->select(true);

      if (~QApplication::keyboardModifiers() & Qt::ControlModifier)
        _drag_traces.clear();

      _drag_traces.push_back(make_pair(mTrace, mTrace->get_zero_vpos()));
      mTrace->set_old_v_offset(mTrace->get_v_offset());
    }

    // DsoSignal::mouse_press internally uses get_y() (absolute content
    // coordinate) to build hit-test rects, but event->position() is
    // viewport-widget-relative. In non-DSO modes with vertical scroll,
    // convert pt to absolute so the rects match.
    const int vOff_press = (_view.get_work_mode() != DSO) ? _view.get_vOffset() : 0;
    const QPoint pt_abs = event->position().toPoint() + QPoint(0, vOff_press);

    for (auto t : traces) {
      if (t->signal_type() == SR_CHANNEL_LOGIC &&
          _view.session().is_working()) {
        // Disable set trigger from left pannel when capturing.
        continue;
      }
      if (t->mouse_press(width(), pt_abs))
        break;
    }

    if (~QApplication::keyboardModifiers() & Qt::ControlModifier) {
      // Unselect all other Traces because the Ctrl is not
      // pressed
      for (auto t : traces) {
        if (t != mTrace)
          t->select(false);
      }
    }
    update();
  }
}

void Header::mouseReleaseEvent(QMouseEvent *event) {
  assert(event);

  _mouse_is_down = false;

  if (_resize_trace_upper || _resize_trace_lower) {
    // Height adjustment completed - persist the new layout to SessionDocument
    pxv_info("Header::mouseReleaseEvent: HEIGHT ADJUSTMENT completed, persisting layout");
    auto &session = _view.session();
    auto *dev = _view.data_source()->device();
    auto *doc = session.get_active_document();
    if (doc && dev && dev->have_instance()) {
      std::map<int, pv::data::ChannelLayoutState> channel_layout;
      for (auto &sig : _view.get_own_signals()) {
        pv::data::ChannelLayoutState layout;
        layout.view_index = sig->get_view_index();
        layout.v_offset = sig->get_v_offset();
        layout.own_height = sig->get_own_height();
        channel_layout[sig->get_index()] = layout;
        pxv_info("  sig index=%d, view_index=%d, v_offset=%d, own_height=%d",
                 sig->get_index(), layout.view_index, layout.v_offset,
                 layout.own_height);
      }
      doc->save_signal_config(session.get_signal_models_snapshot(), channel_layout);
      pxv_info("Header::mouseReleaseEvent: save_signal_config called, saved %d channels",
               (int)channel_layout.size());
    } else {
      pxv_info("Header::mouseReleaseEvent: SKIPPED save_signal_config (doc=%p, device=%p, have_instance=%d)",
               doc, dev,
               dev ? dev->have_instance() : 0);
    }
    _resize_trace_upper = nullptr;
    _resize_trace_lower = nullptr;
    return;
  }

  // judge for color / name / trigger / move
  // get_mTrace requires absolute coordinates (pt + vOffset).
  int action;
  const int vOff_rel = (_view.get_work_mode() != DSO) ? _view.get_vOffset() : 0;
  const auto mTrace = get_mTrace(action, event->position().toPoint() + QPoint(0, vOff_rel));

  if (mTrace) {
    if (action == Trace::COLOR && _colorFlag) {
      _context_trace = mTrace;
      changeColor(event);
      _view.set_all_update(true);
    } else if (action == Trace::NAME && _nameFlag && !_moveFlag) {
      _context_trace = mTrace;
      changeName(event);
    }
  }

  // Make view index by Y value;
  if (_moveFlag && _view.is_logic_rendering_mode()) {
    const auto &groups = _view.get_signal_groups();

    if (groups.size() <= 1) {
      std::vector<Trace *> traces;
      for (auto &s : _view.get_own_decode_traces()) {
        traces.push_back(s.get());
      }
      for (auto &s : _view.get_own_signals()) {
        traces.push_back(s.get());
      }
      sort(traces.begin(), traces.end(), View::compare_trace_y);
      int index = 0;
      for (auto t : traces) {
        t->set_view_index(index++);
      }
    } else {
      Trace *draggedTrace = nullptr;
      if (!_drag_traces.empty())
        draggedTrace = _drag_traces.front().first;

      int draggedGroupIndex = -1;
      if (draggedTrace) {
        for (int gi = 0; gi < (int)groups.size(); gi++) {
          for (auto gt : groups[gi].traces) {
            if (gt == draggedTrace) {
              draggedGroupIndex = gi;
              break;
            }
          }
          if (draggedGroupIndex != -1)
            break;
        }
      }

      std::vector<int> groupOrder;
      for (int i = 0; i < (int)groups.size(); i++)
        groupOrder.push_back(i);

      sort(groupOrder.begin(), groupOrder.end(), [&groups](int a, int b) {
        int minA = INT_MAX, minB = INT_MAX;
        for (auto gt : groups[a].traces)
          minA = min(minA, gt->get_v_offset());
        for (auto gt : groups[b].traces)
          minB = min(minB, gt->get_v_offset());
        return minA < minB;
      });

      int index = 0;
      for (int gi : groupOrder) {
        std::vector<Trace *> groupTraces = groups[gi].traces;
        sort(groupTraces.begin(), groupTraces.end(), View::compare_trace_y);
        for (auto t : groupTraces) {
          t->set_view_index(index++);
        }
      }
    }
  }

  if (_moveFlag) {
    pxv_info("Header::mouseReleaseEvent: MOVE FLAG set, persisting layout");
    _drag_traces.clear();
    _view.signals_changed(mTrace);
    _view.set_all_update(true);

    std::vector<Trace *> traces;
    _view.get_traces(ALL_VIEW, traces);

    for (auto t : traces) {
      t->select(false);
    }

    // Persist channel layout (view_index/v_offset/own_height) to
    // SessionDocument so that subsequent capture-triggered rebuilds can
    // restore the user's custom layout. Without this, every reload() wipes
    // the layout state and resets to default.
    auto &session = _view.session();
    auto *dev = _view.data_source()->device();
    auto *doc = session.get_active_document();
    if (doc && dev && dev->have_instance()) {
      std::map<int, pv::data::ChannelLayoutState> channel_layout;
      for (auto &sig : _view.get_own_signals()) {
        pv::data::ChannelLayoutState layout;
        layout.view_index = sig->get_view_index();
        layout.v_offset = sig->get_v_offset();
        layout.own_height = sig->get_own_height();
        channel_layout[sig->get_index()] = layout;
        pxv_info("  sig index=%d, view_index=%d, v_offset=%d, own_height=%d",
                 sig->get_index(), layout.view_index, layout.v_offset,
                 layout.own_height);
      }
      doc->save_signal_config(session.get_signal_models_snapshot(), channel_layout);
      pxv_info("Header::mouseReleaseEvent: save_signal_config called, saved %d channels",
               (int)channel_layout.size());
    } else {
      pxv_info("Header::mouseReleaseEvent: SKIPPED save_signal_config (doc=%p, device=%p, have_instance=%d)",
               doc, dev,
               dev ? dev->have_instance() : 0);
    }
  } else if (!_drag_traces.empty()) {
    _drag_traces.clear();
  }

  _colorFlag = false;
  _nameFlag = false;
  _moveFlag = false;

  _view.normalize_layout();
}

void Header::wheelEvent(QWheelEvent *event) {
  assert(event);

  int x = 0;
  int y = 0;
  int delta = 0;
  bool isVertical = true;
  QPoint pos;
  (void)x;
  (void)y;

  x = (int)event->position().x();
  y = (int)event->position().y();
  int anglex = event->angleDelta().x();
  int angley = event->angleDelta().y();

  pos.setX(x);
  pos.setY(y);

  if (anglex == 0 || ABS_VAL(angley) >= ABS_VAL(anglex)) {
    delta = angley;
    isVertical = true;
  } else {
    delta = anglex;
    isVertical = false;
  }

  if (isVertical) {
    if (event->modifiers() & Qt::ShiftModifier) {
      int vOffset = _view.get_vOffset();
      vOffset -= delta;
      vOffset = max(0, vOffset);
      _view.verticalScrollBar()->setSliderPosition(vOffset);
      return;
    }

    std::vector<Trace *> traces;
    _view.get_traces(ALL_VIEW, traces);
    // Vertical scrolling
    double shift = 0;

#ifdef Q_OS_DARWIN
    static bool active = true;
    static int64_t last_time;
    if (event->source() == Qt::MouseEventSynthesizedBySystem) {
      if (active) {
        last_time = QDateTime::currentMSecsSinceEpoch();
        shift = delta > 1.5 ? -1 : delta < -1.5 ? 1 : 0;
      }
      int64_t cur_time = QDateTime::currentMSecsSinceEpoch();
      if (cur_time - last_time > 100)
        active = true;
      else
        active = false;
    } else {
      shift = -delta / 80.0;
    }
#else
    shift = delta / 80.0;
#endif

    // Same coordinate fix as mouse_press: DsoSignal::mouse_wheel uses
    // get_y() (absolute) for hit-test rects, but pos is viewport-relative.
    const int vOff_wheel = (_view.get_work_mode() != DSO) ? _view.get_vOffset() : 0;
    const QPoint pos_abs = pos + QPoint(0, vOff_wheel);

    for (auto t : traces) {
      if (t->mouse_wheel(width(), pos_abs, shift))
        break;
    }

    update();
  }
}

void Header::changeName(QMouseEvent *event) {
  if (_context_trace != nullptr && _context_trace->get_type() != SR_CHANNEL_DSO &&
      event->button() == Qt::LeftButton) {
    header_resize();
    QFont font = theme_font_trace_label();
    nameEdit->setFont(font);

    nameEdit->setText(_context_trace->get_name());
    nameEdit->selectAll();
    nameEdit->setFocus();
    nameEdit->show();
    header_updated();
  }
}

void Header::changeColor(QMouseEvent *event) {
  if (_view.session().is_working() &&
      _view.get_work_mode() == ANALOG) {
    // Disable to select color when working on analog mode.
    return;
  }

  if ((event->button() == Qt::LeftButton)) {
    const QColor new_color = QColorDialog::getColor(
        _context_trace->get_colour(), this,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SET_CHANNEL_COLOUR),
            "Set Channel Colour"));
    if (new_color.isValid())
      _context_trace->set_colour(new_color);
  }
}

void Header::mouseMoveEvent(QMouseEvent *event) {
  assert(event);

  if (_view.session().is_working() &&
      _view.is_logic_rendering_mode()) {
    // Disable the hover status of trig button on left pannel.
    return;
  }

  _mouse_point = event->position().toPoint() + QPoint(0, _view.get_vOffset());

  if (_resize_trace_upper) {
    int deltaY = event->position().toPoint().y() - _resize_mouse_down_y;
    int newUpperHeight = _resize_upper_height + deltaY;

    if (newUpperHeight >= View::MinSignalHeight &&
        _view.is_logic_rendering_mode()) {
      _resize_trace_upper->set_own_height(newUpperHeight);
      _view.signals_changed(nullptr);
    }
    return;
  }

  if (_view.is_logic_rendering_mode()) {
    std::vector<Trace *> traces;
    _view.get_traces(ALL_VIEW, traces);
    int mouseY = event->position().toPoint().y() + _view.get_vOffset();
    const int HitBorderMargin = 5;
    bool onBorder = false;

    std::vector<Trace *> enabled_traces;
    for (auto t : traces)
      if (t->enabled())
        enabled_traces.push_back(t);

    for (int i = 0; i < (int)enabled_traces.size() - 1; i++) {
      int traceBottom =
          enabled_traces[i]->get_v_offset() +
          enabled_traces[i]->get_totalHeight() / 2 + View::SignalMargin;

      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        onBorder = true;
        break;
      }
    }

    // Check bottom border of the last enabled trace
    if (!onBorder && !enabled_traces.empty()) {
      Trace *lastTrace = enabled_traces.back();
      int traceBottom = lastTrace->get_v_offset() +
                        lastTrace->get_totalHeight() / 2 + View::SignalMargin;
      if (abs(mouseY - traceBottom) < HitBorderMargin) {
        onBorder = true;
      }
    }
    setCursor(onBorder ? Qt::SplitVCursor : Qt::ArrowCursor);
  }

  // Move the Traces if we are dragging
  if (!_drag_traces.empty()) {
    const int delta = event->position().toPoint().y() - _mouse_down_point.y();

    for (auto i = _drag_traces.begin(); i != _drag_traces.end(); i++) {
      const auto t = (*i).first;
      if (t) {
        int y = (*i).second + delta;
        if (auto *dsoSig = t->as_dso()) {
          dsoSig->set_zero_vpos(y);
          _moveFlag = true;
          traces_moved();
        } else if (auto *mathTrace = t->as_math()) {
          mathTrace->set_zero_vpos(y);
          _moveFlag = true;
          traces_moved();
        } else if (auto *analogSig = t->as_analog()) {
          analogSig->set_zero_vpos(y);
          _moveFlag = true;
          traces_moved();
        } else {
          if (~QApplication::keyboardModifiers() & Qt::ControlModifier) {
            const int y_snap = ((y + View::SignalSnapGridSize / 2) /
                                View::SignalSnapGridSize) *
                               View::SignalSnapGridSize;
            if (y_snap != t->get_v_offset()) {
              _moveFlag = true;
              t->set_v_offset(y_snap);
              traces_moved();
            }
          }
        }
      }
    }
  }
  update();
}

void Header::leaveEvent(QEvent *) {
  _mouse_point = QPoint(-1, -1);
  update();
}

QMenu *Header::create_height_submenu(bool is_batch) {
  QMenu *menu = new QMenu(this);

  static const int preset_heights[] = {24, 48, 72, 96, 120};
  for (int h : preset_heights) {
    QAction *act = menu->addAction(QString::number(h) + "px");
    act->setData(h);
    if (is_batch)
      connect(act, &QAction::triggered, this, &Header::on_batch_set_height);
    else
      connect(act, &QAction::triggered, this, &Header::on_set_channel_height);
  }

  menu->addSeparator();

  QAction *customAct = menu->addAction(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CUSTOM_HEIGHT), "Custom..."));
  customAct->setData(-1);
  if (is_batch)
    connect(customAct, &QAction::triggered, this, &Header::on_batch_set_height);
  else
    connect(customAct, &QAction::triggered, this,
            &Header::on_set_channel_height);

  return menu;
}

void Header::keyPressEvent(QKeyEvent *event) {
  // F/I shortcuts (Task 4.6): only when no popup is currently open, and
  // only when a LogicSignal is selected as the context trace. The Glitch
  // Filter popup itself grabs keyboard focus while open, so these shortcuts
  // are naturally disabled while it is visible; the activePopupWidget check
  // is an extra guard against any other modal popup (QMenu, QInputDialog,
  // QColorDialog, etc.) that may be open.
  if (QApplication::activePopupWidget() == nullptr && _context_trace) {
    auto *sig = _context_trace->as_logic();
    if (sig) {
      if (event->key() == Qt::Key_F) {
        emit show_glitch_filter_popup(sig);
        event->accept();
        return;
      }
      if (event->key() == Qt::Key_I) {
        emit toggle_signal_invert_requested(sig);
        event->accept();
        return;
      }
    }
  }
  QWidget::keyPressEvent(event);
}

void Header::contextMenuEvent(QContextMenuEvent *event) {
  // 统一菜单样式:背景/文字/选中态/边框全部跟随主题 token,
  // Zone A 滤波菜单与 Zone B 行高菜单(含子菜单)共用同一外观。
  auto apply_menu_style = [](QMenu *m) {
    if (!m)
      return;
    const auto token = [](const char *name) {
      return AppConfig::Instance().GetThemeTokenValue(name);
    };
    const QString bg = token("@bg-overlay");
    const QString fg = token("@fg-base");
    const QString fgMuted = token("@fg-muted");
    const QString border = token("@border-strong");
    QString accent = token("@accent");
    if (accent.isEmpty())
      accent = token("@toolbtn-hover");
    if (bg.isEmpty() || fg.isEmpty())
      return;  // 主题未加载,回退系统默认外观
    QString sheet =
        QString(
            "QMenu { background: %1; color: %2; border: 1px solid %3; }"
            "QMenu::item { padding: 4px 18px; background: transparent; }"
            "QMenu::item:selected { background: %4; color: %2; }"
            "QMenu::item:disabled { color: %5; }"
            "QMenu::separator { height: 1px; background: %3; margin: 4px 8px; }")
            .arg(bg, fg, border, accent, fgMuted.isEmpty() ? fg : fgMuted);
    m->setStyleSheet(sheet);
  };

  const QPoint pt = event->pos() + QPoint(0, _view.get_vOffset());
  int action = 0;
  const auto t = get_mTrace(action, pt);

  // 解码通道:任何模式下都弹"更改颜色"菜单(左键改色已删除,统一右键入口)
  if (t && t->as_decode()) {
    _context_trace = t;
    QMenu menu(this);
    menu.addAction(
        L_S(STR_PAGE_SIGNAL_PROC, "IDS_CHANGE_COLOR", "Change Color"),
        this, &Header::on_change_color_triggered);
    apply_menu_style(&menu);
    menu.exec(event->globalPos());
    return;
  }

  // 非 LOGIC 模式的波形通道(ANALOG/DSO):右键改色菜单
  // (LOGIC 模式波形通道走 Zone A 滤波菜单,内含改色项)
  if (!_view.is_logic_rendering_mode()) {
    if (!t)
      return;
    _context_trace = t;
    QMenu menu(this);
    menu.addAction(
        L_S(STR_PAGE_SIGNAL_PROC, "IDS_CHANGE_COLOR", "Change Color"),
        this, &Header::on_change_color_triggered);
    apply_menu_style(&menu);
    menu.exec(event->globalPos());
    return;
  }

  // 两段区域分别弹不同菜单:
  //  - LABEL (右侧边缘小方块):行高菜单(还原原始行为)
  //  - NAME/COLOR 或行内其他位置 (D0 名称区域):滤波菜单
  // pt_in_rect 的 NAME 矩形较小可能漏判,action==0 时用 y 坐标兜底
  // 判定为名称区域(滤波菜单)。
  Trace *target = t;
  if (!target || action == 0) {
    const int clickY = event->pos().y() + _view.get_vOffset();
    std::vector<Trace *> traces;
    _view.get_traces(ALL_VIEW, traces);
    for (auto tr : traces) {
      const int y = tr->get_v_offset();
      const int halfH = tr->get_totalHeight() / 2 + View::SignalMargin;
      if (clickY >= y - halfH && clickY <= y + halfH) {
        target = tr;
        action = Trace::NAME;  // 兜底归为名称区域
        break;
      }
    }
    if (!target)
      return;
  }

  _context_trace = target;

  // ===== Zone B: 右侧 LABEL 区域 → 行高菜单(原始行为) =====
  if (action == Trace::LABEL) {
    QMenu menu(this);
    menu.addAction(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RESET_ROW_HEIGHT), "Reset Row Height"),
        this, &Header::on_reset_row_height);
    menu.addAction(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_RESET_ALL_ROW_HEIGHT),
            "Reset All Row Heights"),
        this, &Header::on_reset_all_row_height);
    menu.addSeparator();

    QMenu *channelMenu = create_height_submenu(false);
    channelMenu->setTitle(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SET_CHANNEL_HEIGHT),
            "Set Channel Height"));
    menu.addMenu(channelMenu);

    QMenu *batchMenu = create_height_submenu(true);
    batchMenu->setTitle(
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_BATCH_SET_HEIGHT), "Batch Set"));
    menu.addMenu(batchMenu);

    apply_menu_style(&menu);
    apply_menu_style(channelMenu);
    apply_menu_style(batchMenu);
    menu.exec(event->globalPos());
    return;
  }

  // ===== Zone A: D0 名称区域 → 滤波菜单 =====
  auto *logic_sig = target->as_logic();
  if (!logic_sig)
    return;

  auto &session = _view.session();
  const bool any_filtered = session.is_glitch_filter_active();

  QMenu menu(this);
  menu.addAction(
      L_S(STR_PAGE_SIGNAL_PROC, "IDS_FILTER_GLITCHES",
          "Filter Glitches..."),
      this, &Header::on_filter_glitches_triggered);
  menu.addAction(
      L_S(STR_PAGE_SIGNAL_PROC, "IDS_TOGGLE_SIGNAL_INVERT",
          "Invert Signal"),
      this, &Header::on_toggle_invert_triggered);

  auto *clear_act = menu.addAction(
      L_S(STR_PAGE_SIGNAL_PROC, "IDS_CLEAR_CHANNEL_FILTER",
          "Clear Channel Filter"),
      this, &Header::on_clear_channel_filter_triggered);
  const bool channel_filtered = [&logic_sig]() {
    if (!logic_sig || !logic_sig->data())
      return false;
    const auto model = logic_sig->model();
    if (!model)
      return false;
    const int sig_index = model->index();
    return !logic_sig->data()->get_filtered_ranges(sig_index).empty();
  }();
  clear_act->setEnabled(channel_filtered);

  auto *clear_all_act = menu.addAction(
      L_S(STR_PAGE_SIGNAL_PROC, "IDS_CLEAR_ALL_FILTER",
          "Clear All Filters"),
      this, &Header::on_clear_all_filter_triggered);
  clear_all_act->setEnabled(any_filtered);

  menu.addSeparator();
  menu.addAction(
      L_S(STR_PAGE_SIGNAL_PROC, "IDS_CHANGE_COLOR", "Change Color"),
      this, &Header::on_change_color_triggered);

  apply_menu_style(&menu);
  menu.exec(event->globalPos());
}

void Header::on_filter_glitches_triggered() {
  if (!_context_trace)
    return;
  auto *sig = _context_trace->as_logic();
  if (!sig)
    return;
  emit show_glitch_filter_popup(sig);
}

void Header::on_clear_channel_filter_triggered() {
  if (!_context_trace)
    return;
  emit clear_glitch_filter_requested(false);
}

void Header::on_clear_all_filter_triggered() {
  emit clear_glitch_filter_requested(true);
}

void Header::on_toggle_invert_triggered() {
  if (!_context_trace)
    return;
  auto *sig = _context_trace->as_logic();
  if (!sig)
    return;
  emit toggle_signal_invert_requested(sig);
}

void Header::on_change_color_triggered() {
  // 解码通道右键"更改颜色":复用 changeColor 的 QColorDialog 流程,
  // 但不依赖 QMouseEvent(右键菜单触发,无 mouse event 参数)
  if (!_context_trace)
    return;
  const QColor new_color = QColorDialog::getColor(
      _context_trace->get_colour(), this,
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SET_CHANNEL_COLOUR),
          "Set Channel Colour"));
  if (new_color.isValid()) {
    _context_trace->set_colour(new_color);
    _view.set_all_update(true);
  }
}

void Header::on_reset_row_height() {
  if (!_context_trace)
    return;
  _context_trace->set_own_height(-1);
  _view.signals_changed(nullptr);
}

void Header::on_reset_all_row_height() {
  std::vector<Trace *> traces;
  _view.get_traces(ALL_VIEW, traces);
  for (auto t : traces) {
    t->set_own_height(-1);
  }
  _view.signals_changed(nullptr);
}

void Header::on_set_channel_height() {
  QAction *act = qobject_cast<QAction *>(sender());
  if (!act || !_context_trace)
    return;

  int h = act->data().toInt();
  if (h == -1) {
    bool ok = false;
    h = QInputDialog::getInt(this,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SET_CHANNEL_HEIGHT),
            "Set Channel Height"),
        QString(), _context_trace->get_totalHeight(),
        View::MinSignalHeight, View::MaxSignalHeight, 1, &ok);
    if (!ok)
      return;
  }

  h = max(View::MinSignalHeight, min(h, View::MaxSignalHeight));
  _context_trace->set_own_height(h);
  _view.signals_changed(nullptr);
}

void Header::on_batch_set_height() {
  QAction *act = qobject_cast<QAction *>(sender());
  if (!act)
    return;

  int h = act->data().toInt();
  if (h == -1) {
    bool ok = false;
    h = QInputDialog::getInt(this,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_BATCH_SET_HEIGHT), "Batch Set"),
        QString(), 24,
        View::MinSignalHeight, View::MaxSignalHeight, 1, &ok);
    if (!ok)
      return;
  }

  h = max(View::MinSignalHeight, min(h, View::MaxSignalHeight));

  std::vector<Trace *> traces;
  _view.get_traces(ALL_VIEW, traces);
  for (auto t : traces) {
    t->set_own_height(h);
  }
  _view.signals_changed(nullptr);
}

void Header::on_action_set_name_triggered() {
  auto context_Trace = _context_trace;
  if (!context_Trace)
    return;

  if (nameEdit->isModified()) {
    QString v = nameEdit->text().trimmed();
    if (v == "")
      v = QString::number(context_Trace->get_index());

    // Update Core layer (SignalModel + sr_channel) via SigSession.
    auto model = _view.session().get_signal_by_index(context_Trace->get_index());
    if (model)
      _view.session().set_trace_name(model, v);

    // Also update View layer (Trace::_name) so that gen_config_json()
    // saves the correct name and the header repaints with the new name.
    // Without this, only the Core model is updated but Trace::_name
    // remains stale — the saved config file contains the old name.
    context_Trace->set_name(v);
  }

  nameEdit->hide();
  header_updated();
}

void Header::header_resize() {
  if (_context_trace) {
    const int y = _context_trace->get_y();
    nameEdit->move(
        QPoint(_context_trace->get_leftWidth(), y - nameEdit->height() / 2));
  }
}

void Header::UpdateLanguage() { retranslateUi(); }

void Header::UpdateTheme() {
  // 主动从主题 token 读取前景色,不再被动依赖 QWidget::palette()。
  // QSS 的 color 属性 → palette 传播在以下场景不可靠:
  //  1) Header 在 switchTheme() 之前构造,palette 仍是默认黑色;
  //  2) 父级 View 自带 setStyleSheet,阻断 qApp 级 palette 传播;
  //  3) setStyleSheet 与 update() 都 post 事件,处理顺序不确定。
  // 一旦 palette 拿到黑色,logicsignal.cpp 的触发图标(用 fore 画)
  // 在暗色背景上就是黑字,且改主题走同一路径仍会失败。
  _foreColor = AppConfig::Instance().GetThemeColor("@fg-base");
  retranslateUi();
}

void Header::UpdateFont() {}

void Header::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); }

} // namespace view
} // namespace pv
