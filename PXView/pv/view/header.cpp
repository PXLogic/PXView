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

#include "header.h"

#include <QApplication>
#include <QColorDialog>
#include <QFont>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QScrollBar>
#include <QStyleOption>
#include <algorithm>
#include <assert.h>
#include <set>

#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../sigsession.h"
#include "../ui/dockfonts.h"
#include "../ui/fn.h"
#include "../ui/langresource.h"
#include "analogsignal.h"
#include "decodetrace.h"
#include "dsosignal.h"
#include "groupsignal.h"
#include "logicsignal.h"
#include "trace.h"
#include "view.h"

using namespace std;

namespace pv {
namespace view {

Header::Header(View &parent) : QWidget(&parent), _view(parent) {
  _moveFlag = false;
  _colorFlag = false;
  _nameFlag = false;
  _context_trace = NULL;
  _resize_trace_upper = NULL;
  _resize_trace_lower = NULL;
  _resize_mouse_down_y = 0;
  _resize_upper_height = 0;
  _resize_lower_height = 0;
  _mouse_is_down = false;

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
  const int w = width();
  std::vector<Trace *> traces;
  _view.get_traces(ALL_VIEW, traces);

  for (auto t : traces) {
    if ((action = t->pt_in_rect(t->get_y(), w, pt)))
      return t;
  }

  return NULL;
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

  int dso_count = 0;
  for (auto t : traces) {
    if (t->signal_type() == SR_CHANNEL_DSO) {
      dso_count++;
      pxv_info("[DEBUG-DSO] Header trace: name=%s, y=%d, enabled=%d, visible=%d, totalH=%d",
               t->get_name().toUtf8().data(), t->get_y(), t->enabled(), t->visible(), t->get_totalHeight());
    }
  }
  pxv_info("[DEBUG-DSO] Header::paintEvent: traces=%d, dso_count=%d, work_mode=%d, vOffset=%d, header_h=%d, header_w=%d",
           (int)traces.size(), dso_count, _view.get_work_mode(), _view.get_vOffset(), height(), width());

  const bool dragging = !_drag_traces.empty();
  QColor fore(QWidget::palette().color(QWidget::foregroundRole()));
  fore.setAlpha(View::ForeAlpha);

  QFont font = theme_font_trace_label();
  painter.setFont(font);
  painter.setRenderHint(QPainter::TextAntialiasing, false);

  painter.save();
  pxv_info("[DEBUG-DSO] Header::paintEvent: work_mode=%d, vOffset=%d, header_h=%d",
           _view.get_work_mode(), _view.get_vOffset(), height());
  if (_view.get_work_mode() != DSO) {
    painter.translate(0, -_view.get_vOffset());
  }

  if (_view.get_work_mode() == LOGIC) {
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
          double traceTop = gt->get_v_offset() - gt->get_totalHeight() * 0.5 -
                            View::SignalMargin;
          double traceBottom = gt->get_v_offset() +
                               gt->get_totalHeight() * 0.5 + View::SignalMargin;
          groupTop = min(groupTop, traceTop);
          groupBottom = max(groupBottom, traceBottom);
        }

        double cardTop = groupTop - View::GroupGap * 0.5;
        double cardHeight = groupBottom - groupTop + View::GroupGap;

        QRectF cardRect(0, cardTop, w + View::GroupCardRadius + 1, cardHeight);
        QPainterPath groupPath;
        groupPath.addRoundedRect(cardRect, View::GroupCardRadius, View::GroupCardRadius);

        if (_view.is_colored_card_mode()) {
          painter.save();
          painter.setClipPath(groupPath);
          painter.setPen(Qt::NoPen);
          
          for (size_t i = 0; i < group.traces.size(); i++) {
            auto gt = group.traces[i];
            double tTop = gt->get_v_offset() - gt->get_totalHeight() * 0.5 - View::SignalMargin;
            double tBottom = gt->get_v_offset() + gt->get_totalHeight() * 0.5 + View::SignalMargin;
            
            if (i == 0) tTop -= View::GroupGap * 0.5;
            if (i == group.traces.size() - 1) tBottom += View::GroupGap * 0.5;
            
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
  if (_view.get_work_mode() == LOGIC) {
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
    if ((*it)->enabled() || dynamic_cast<DsoSignal *>(*it)) {
      lastEnabledTrace = *it;
      break;
    }
  }

  painter.setPen(QPen(dividerColor, 1));
  for (auto t : traces) {
    if (!t->enabled() && !dynamic_cast<DsoSignal *>(t))
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

  if (_view.get_work_mode() == LOGIC) {
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
        _view.signals_changed(NULL);
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
        _view.signals_changed(NULL);
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

  const bool instant = _view.session().is_instant();
  if (instant && _view.session().is_running_status()) {
    return;
  }

  if (_view.get_work_mode() == LOGIC) {
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
        _resize_trace_lower = NULL;
        _resize_mouse_down_y = event->position().toPoint().y();
        _resize_upper_height = lastTrace->get_totalHeight();
        _resize_lower_height = 0;
        return;
      }
    }
  }

  if (event->button() & Qt::LeftButton) {
    _mouse_down_point =
        event->position().toPoint() + QPoint(0, _view.get_vOffset());

    // Save the offsets of any Traces which will be dragged
    for (auto t : traces) {
      if (t->selected())
        _drag_traces.push_back(make_pair(t, t->get_v_offset()));
    }

    // Select the Trace if it has been clicked
    const auto mTrace = get_mTrace(action, event->position().toPoint());
    if (action == Trace::COLOR && mTrace) {
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

    for (auto t : traces) {
      if (t->signal_type() == SR_CHANNEL_LOGIC &&
          _view.session().is_working()) {
        // Disable set trigger from left pannel when capturing.
        continue;
      }
      if (t->mouse_press(width(), event->position().toPoint()))
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
    _resize_trace_upper = NULL;
    _resize_trace_lower = NULL;
    return;
  }

  // judge for color / name / trigger / move
  int action;
  const auto mTrace = get_mTrace(action, event->position().toPoint());

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
  int mode = _view.get_work_mode();
  if (_moveFlag && mode == LOGIC) {
    const auto &groups = _view.get_signal_groups();

    if (groups.size() <= 1) {
      std::vector<Trace *> traces;
      for (auto s : _view.effective_data_source()->get_decode_signals()) {
        traces.push_back(s);
      }
      for (auto s : _view.get_own_signals()) {
        traces.push_back(s);
      }
      sort(traces.begin(), traces.end(), View::compare_trace_y);
      int index = 0;
      for (auto t : traces) {
        t->set_view_index(index++);
      }
    } else {
      Trace *draggedTrace = NULL;
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
    _drag_traces.clear();
    _view.signals_changed(mTrace);
    _view.set_all_update(true);

    std::vector<Trace *> traces;
    _view.get_traces(ALL_VIEW, traces);

    for (auto t : traces) {
      t->select(false);
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

    for (auto t : traces) {
      if (t->mouse_wheel(width(), pos, shift))
        break;
    }

    update();
  }
}

void Header::changeName(QMouseEvent *event) {
  if (_context_trace != NULL && _context_trace->get_type() != SR_CHANNEL_DSO &&
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
      _view.get_work_mode() == LOGIC) {
    // Disable the hover status of trig button on left pannel.
    return;
  }

  _mouse_point = event->position().toPoint() + QPoint(0, _view.get_vOffset());

  if (_resize_trace_upper) {
    int deltaY = event->position().toPoint().y() - _resize_mouse_down_y;
    int newUpperHeight = _resize_upper_height + deltaY;

    if (newUpperHeight >= View::MinSignalHeight &&
        _view.get_work_mode() == LOGIC) {
      _resize_trace_upper->set_own_height(newUpperHeight);
      _view.signals_changed(NULL);
    }
    return;
  }

  if (_view.get_work_mode() == LOGIC) {
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
        if (t->get_type() == SR_CHANNEL_DSO) {
          DsoSignal *dsoSig = NULL;
          if ((dsoSig = dynamic_cast<DsoSignal *>(t))) {
            dsoSig->set_zero_vpos(y);
            _moveFlag = true;
            traces_moved();
          }
        } else if (t->get_type() == SR_CHANNEL_MATH) {
          MathTrace *mathTrace = NULL;
          if ((mathTrace = dynamic_cast<MathTrace *>(t))) {
            mathTrace->set_zero_vpos(y);
            _moveFlag = true;
            traces_moved();
          }
        } else if (t->get_type() == SR_CHANNEL_ANALOG) {
          AnalogSignal *analogSig = NULL;
          if ((analogSig = dynamic_cast<AnalogSignal *>(t))) {
            analogSig->set_zero_vpos(y);
            _moveFlag = true;
            traces_moved();
          }
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

void Header::contextMenuEvent(QContextMenuEvent *event) {
  (void)event;

  if (_view.get_work_mode() != LOGIC)
    return;

  int action;
  const auto t = get_mTrace(action, _mouse_point);

  if (!t || action != Trace::LABEL)
    return;

  _context_trace = t;

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

  menu.exec(event->globalPos());
}

void Header::on_reset_row_height() {
  if (!_context_trace)
    return;
  _context_trace->set_own_height(-1);
  _view.signals_changed(NULL);
}

void Header::on_reset_all_row_height() {
  std::vector<Trace *> traces;
  _view.get_traces(ALL_VIEW, traces);
  for (auto t : traces) {
    t->set_own_height(-1);
  }
  _view.signals_changed(NULL);
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
  _view.signals_changed(NULL);
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
  _view.signals_changed(NULL);
}

void Header::on_action_set_name_triggered() {
  auto context_Trace = _context_trace;
  if (!context_Trace)
    return;

  if (nameEdit->isModified()) {
    QString v = nameEdit->text().trimmed();
    if (v == "")
      v = QString::number(context_Trace->get_index());

    _view.session().set_trace_name(context_Trace, v);
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

void Header::UpdateTheme() { retranslateUi(); }

void Header::UpdateFont() {}

void Header::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); }

} // namespace view
} // namespace pv
