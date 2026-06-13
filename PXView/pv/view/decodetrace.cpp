/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
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

#include "decodetrace.h"
#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../data/decode/annotation.h"
#include "../data/decode/decoder.h"
#include "../data/decode/rowdata.h"
#include "../data/decoderstack.h"
#include "../data/logicsnapshot.h"
#include "../data/sessiondocument.h"
#include "../dialogs/decoderoptionsdlg.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../sigsession.h"
#include "../toolbars/titlebar.h"
#include "../ui/dscombobox.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include "../ui/dockfonts.h"
#include "../view/cursor.h"
#include "../view/logicsignal.h"
#include "../view/view.h"
#include "../widgets/decodergroupbox.h"
#include "../widgets/decodermenu.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <boost/functional/hash.hpp>
#include <libsigrokdecode.h>

using namespace boost;
using namespace std;

namespace pv {
namespace view {
#include "../config/appconfig.h"

const int DecodeTrace::ArrowSize = 4;
const double DecodeTrace::EndCapWidth = 5;
const int DecodeTrace::DrawPadding = 100;
const int DecodeTrace::ControlRectWidth;

QColor DecodeTrace::getChannelColor(int channelIndex) {
    QColor c = AppConfig::Instance().GetThemeColor(QString("@decoder-channel-%1").arg(channelIndex));
    if (c.isValid()) return c;
    
    // Fallback original Colours array
    static const QColor defaultColours[16] = {
        QColor(0xEF, 0x29, 0x29), QColor(0xF6, 0x6A, 0x32),
        QColor(0xFC, 0xAE, 0x3E), QColor(0xFB, 0xCA, 0x47),
        QColor(0xFC, 0xE9, 0x4F), QColor(0xCD, 0xF0, 0x40),
        QColor(0x8A, 0xE2, 0x34), QColor(0x4E, 0xDC, 0x44),
        QColor(0x55, 0xD7, 0x95), QColor(0x64, 0xD1, 0xD2),
        QColor(0x72, 0x9F, 0xCF), QColor(0xD4, 0x76, 0xC4),
        QColor(0x9D, 0x79, 0xB9), QColor(0xAD, 0x7F, 0xA8),
        QColor(0xC2, 0x62, 0x9B), QColor(0xD7, 0x47, 0x6F)
    };
    return defaultColours[channelIndex % 16];
}

QColor DecodeTrace::getErrorBgColor() {
    QColor c = AppConfig::Instance().GetThemeColor("@decoder-error-bg");
    return c.isValid() ? c : QColor(0xEF, 0x29, 0x29);
}

QColor DecodeTrace::getNoDecodeColor() {
    QColor c = AppConfig::Instance().GetThemeColor("@decoder-no-decode");
    return c.isValid() ? c : QColor(0x88, 0x8A, 0x85);
}

QColor DecodeTrace::getAnnColor(int channelIndex) {
    QColor c = AppConfig::Instance().GetThemeColor(QString("@decoder-ann-%1").arg(channelIndex));
    if (c.isValid()) return c;
    return getChannelColor(channelIndex);
}

QColor DecodeTrace::getAnnOutlineColor(int channelIndex) {
    QColor c = AppConfig::Instance().GetThemeColor(QString("@decoder-ann-outline-%1").arg(channelIndex));
    if (c.isValid()) return c;
    return OutlineColours[channelIndex % 16];
}

const QColor DecodeTrace::OutlineColours[16] = {
    QColor(0x77, 0x14, 0x14), QColor(0x7B, 0x35, 0x19),
    QColor(0x7E, 0x57, 0x1F), QColor(0x7D, 0x65, 0x23),
    QColor(0x7E, 0x74, 0x27), QColor(0x66, 0x78, 0x20),
    QColor(0x45, 0x71, 0x1A), QColor(0x27, 0x6E, 0x22),
    QColor(0x2A, 0x6B, 0x4A), QColor(0x32, 0x68, 0x69),
    QColor(0x39, 0x4F, 0x67), QColor(0x6A, 0x3B, 0x62),
    QColor(0x4E, 0x3C, 0x5C), QColor(0x56, 0x3F, 0x54),
    QColor(0x61, 0x31, 0x4D), QColor(0x6B, 0x23, 0x37)};

DecodeTrace::DecodeTrace(pv::SigSession *session,
                         pv::data::DecoderStack *decoder_stack, int index)
    : Trace(QString::fromUtf8(decoder_stack->stack().front()->decoder()->name),
            index, SR_CHANNEL_DECODER) {
  assert(decoder_stack);

  _colour = getChannelColor(index % 16);

  _decode_start = 0;
  _decode_end = INT64_MAX;
  _decoder_stack = decoder_stack;
  _session = session;
  _delete_flag = false;
  _decode_cursor1 = 0;
  _decode_cursor2 = 0;

  connect(_decoder_stack, &data::DecoderStack::new_decode_data, this,
          &DecodeTrace::on_new_decode_data);

  connect(_decoder_stack, &data::DecoderStack::decode_done, this,
          &DecodeTrace::on_decode_done);
}

DecodeTrace::~DecodeTrace() {
  _cur_row_headings.clear();

  DESTROY_OBJECT(_decoder_stack);
}

bool DecodeTrace::enabled() { return true; }

void DecodeTrace::set_view(pv::view::View *view) {
  assert(view);
  Trace::set_view(view);
}

void DecodeTrace::paint_back(QPainter &p, int left, int right, QColor fore,
                             QColor back) {
  (void)back;

  QColor backFore = fore;
  backFore.setAlpha(View::BackAlpha);
  QPen pen(backFore);
  pen.setStyle(Qt::DotLine);
  p.setPen(pen);
  const double sigY = get_y();
  p.drawLine(left, sigY, right, sigY);

  // --draw decode region control
  uint64_t doc_samplerate = 0;
  if (_session->get_active_document()) {
    doc_samplerate = _session->get_active_document()->cur_snap_samplerate();
  }
  const double samples_per_pixel =
      (doc_samplerate > 0 ? doc_samplerate : _session->cur_snap_samplerate()) *
      _view->scale();
  const double startX = _decode_start / samples_per_pixel - _view->offset();
  const double endX = _decode_end / samples_per_pixel - _view->offset();
  const double regionY = get_y() - _totalHeight * 0.5 - ControlRectWidth;

  p.setBrush(View::Blue);
  p.drawLine(startX, regionY, startX,
             regionY + _totalHeight + ControlRectWidth);
  p.drawLine(endX, regionY, endX, regionY + _totalHeight + ControlRectWidth);
  const QPointF start_points[] = {QPointF(startX - ControlRectWidth, regionY),
                                  QPointF(startX + ControlRectWidth, regionY),
                                  QPointF(startX, regionY + ControlRectWidth)};
  const QPointF end_points[] = {QPointF(endX - ControlRectWidth, regionY),
                                QPointF(endX + ControlRectWidth, regionY),
                                QPointF(endX, regionY + ControlRectWidth)};
  p.drawPolygon(start_points, countof(start_points));
  p.drawPolygon(end_points, countof(end_points));

  // --draw headings
  const int row_height = (_totalHeight > 0 && _cur_row_headings.size() > 0)
                             ? _totalHeight / (int)_cur_row_headings.size()
                             : _view->get_signalHeight();
  for (size_t i = 0; i < _cur_row_headings.size(); i++) {
    const int y = i * row_height + get_y() - _totalHeight * 0.5;

    p.setPen(QPen(Qt::NoPen));
    p.setBrush(QApplication::palette().brush(QPalette::WindowText));

    const QRect r(left + ArrowSize * 2, y, right - left, row_height / 2);
    const QString h(_cur_row_headings[i]);
    const int f = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip;
    const QPointF points[] = {QPointF(left, r.center().y() - ArrowSize),
                              QPointF(left + ArrowSize, r.center().y()),
                              QPointF(left, r.center().y() + ArrowSize)};
    p.drawPolygon(points, countof(points));

    // Draw the text
    p.setPen(fore);
    p.drawText(r, f, h);
  }
}

void DecodeTrace::paint_mid(QPainter &p, int left, int right, QColor fore,
                            QColor back) {
  using namespace pv::data::decode;

  assert(_decoder_stack);
  const QString err = _decoder_stack->error_message();
  if (!err.isEmpty()) {
    draw_error(p, err, left, right);
  }

  const double scale = _view->scale();
  if (scale <= 0)
    return;

  double samplerate = _decoder_stack->samplerate();

  _cur_row_headings.clear();

  // Show sample rate as 1Hz when it is unknown
  if (samplerate == 0.0)
    samplerate = 1.0;

  const int64_t pixels_offset = _view->offset();
  const double samples_per_pixel = samplerate * scale;

  uint64_t start_sample =
      (uint64_t)max((left + pixels_offset) * samples_per_pixel, 0.0);
  uint64_t end_sample =
      (uint64_t)max((right + pixels_offset) * samples_per_pixel, 0.0);

  for (auto dec : _decoder_stack->stack()) {
    start_sample = max(dec->decode_start(), start_sample);
    end_sample = min(dec->decode_end(), end_sample);
    break;
  }

  if (end_sample < start_sample)
    return;

  const int row_count = rows_size();
  const int annotation_height =
      (row_count > 0) ? (_totalHeight / row_count) : _view->get_signalHeight();

  // Iterate through the rows
  assert(_view);
  int y = get_y() - (_totalHeight - annotation_height) * 0.5;

  assert(_decoder_stack);

  for (auto dec : _decoder_stack->stack()) {
    if (dec->shown()) {
      const std::map<const pv::data::decode::Row, bool> rows =
          _decoder_stack->get_rows_gshow();
      for (std::map<const pv::data::decode::Row, bool>::const_iterator i =
               rows.begin();
           i != rows.end(); i++) {
        if ((*i).first.decoder() == dec->decoder() &&
            _decoder_stack->has_annotations((*i).first)) {
          if ((*i).second) {
            const Row &row = (*i).first;

            const uint64_t min_annotation =
                _decoder_stack->get_min_annotation(row);
            const double min_annWidth = min_annotation / samples_per_pixel;

            RowData *row_data = _decoder_stack->get_row_data(row);
            if (row_data) {
              if (min_annWidth < 2.0) {
                uint64_t current_sample = start_sample;
                const size_t base_colour = 0;

                while (current_sample <= end_sample) {
                  const Annotation *ann =
                      row_data->get_first_annotation_ending_after(
                          current_sample);
                  if (!ann || ann->start_sample() > end_sample)
                    break;

                  uint64_t block_start =
                      std::max(current_sample, ann->start_sample());
                  uint64_t block_end = std::max(
                      ann->end_sample(),
                      block_start + (uint64_t)std::max(1.0, samples_per_pixel));

                  double x = (block_start / samples_per_pixel) - pixels_offset;
                  double width = (block_end - block_start) / samples_per_pixel;

                  const size_t colour =
                      ((base_colour + ann->type()) % MaxAnnType) % 16;
                  const QColor fill = getAnnColor(colour);

                  p.fillRect(QRectF(x, y - annotation_height * 0.5,
                                    std::max(1.0, width), annotation_height),
                             fill);

                  current_sample = block_end;
                }
              } else {
                auto range =
                    row_data->get_visible_range(start_sample, end_sample);
                const size_t start_idx = range.first;
                const size_t end_idx = range.second;

                if (start_idx < end_idx) {
                  double last_x = -1;
                  for (size_t idx = start_idx; idx < end_idx; idx++) {
                    const Annotation *a = row_data->annotation_at(idx);
                    if (!a)
                      break;
                    draw_annotation(*a, p, get_text_colour(), annotation_height,
                                    left, right, samples_per_pixel,
                                    pixels_offset, y, 0, min_annWidth, fore,
                                    back, last_x);
                  }
                }
              }
            }

            y += annotation_height;
            _cur_row_headings.push_back(row.title());
          }
        }
      }
    } else {
      draw_unshown_row(p, y, annotation_height, left, right,
                       L_S(STR_PAGE_DLG, S_ID(IDS_DLG_UNSHOWN), "Unshown"),
                       fore, back);
      y += annotation_height;
      _cur_row_headings.push_back(dec->decoder()->name);
    }
  }
}

void DecodeTrace::paint_fore(QPainter &p, int left, int right, QColor fore,
                             QColor back) {
  using namespace pv::data::decode;

  (void)p;
  (void)left;
  (void)right;
  (void)fore;
  (void)back;
}

void DecodeTrace::draw_annotation(const pv::data::decode::Annotation &a,
                                  QPainter &p, QColor text_color, int h,
                                  int left, int right, double samples_per_pixel,
                                  double pixels_offset, int y,
                                  size_t base_colour, double min_annWidth,
                                  QColor fore, QColor back, double &last_x) {
  const double start =
      max(a.start_sample() / samples_per_pixel - pixels_offset, (double)left);
  const double end =
      min(a.end_sample() / samples_per_pixel - pixels_offset, (double)right);

  const size_t colour =
      ((base_colour + a.type()) % MaxAnnType) % 16;
  const QColor fill = getAnnColor(colour);
  const QColor outline = getAnnOutlineColor(colour);

  if (start > right + DrawPadding || end < left - DrawPadding) {
    return;
  }

  // 完美无缝隙 LOD 防御：
  // 1. 若当前标注完全被之前绘制的区域覆盖，则不可见，跳过
  if (end <= last_x) {
    return;
  }

  // 2. 针对极高密度的微小标注（它们最终只会画成一根竖线）
  // 如果这根竖线落在我们刚刚画过的区域内（在同一个物理像素列），直接跳过！
  // 这样不仅能把百万次 drawLine
  // 削减到屏幕像素宽度（~1920次），还绝对不会产生视觉断层和缝隙！
  if (start + 2.0 > end && (int)start <= (int)last_x) {
    return;
  }

  if (start + 2.0 > end) {
    last_x = start;
  } else {
    last_x = end;
  }

  if (_decoder_stack->get_mark_index() ==
      (int64_t)(a.start_sample() + a.end_sample()) / 2) {
    p.setPen(View::Blue);
    int xpos = (start + end) / 2;
    int ypos = get_y() + _totalHeight * 0.5 + 1;
    const QPoint triangle[] = {
        QPoint(xpos, ypos),         QPoint(xpos - 1, ypos + 1),
        QPoint(xpos, ypos + 1),     QPoint(xpos + 1, ypos + 1),
        QPoint(xpos - 2, ypos + 2), QPoint(xpos - 1, ypos + 2),
        QPoint(xpos, ypos + 2),     QPoint(xpos + 1, ypos + 2),
        QPoint(xpos + 2, ypos + 2),
    };
    p.drawPoints(triangle, 9);
  }

  if (a.start_sample() == a.end_sample()) {
    draw_instant(a, p, fill, outline, text_color, h, start, y, min_annWidth);
  } else {
    draw_range(a, p, fill, outline, text_color, h, start, end, y, fore, back);

    if ((a.type() / 100 == 2) && (end - start > 20)) {
      for (auto dec : _decoder_stack->stack()) {
        auto probes = dec->binded_probe_list();

        for (auto probe : probes) {
          int type = dec->get_channel_type(probe);

          if ((type == SRD_CHANNEL_COMMON) ||
              ((type % 100 != a.type() % 100) && (type % 100 != 0))) {
            continue;
          }

          const double mark_end =
              a.end_sample() / samples_per_pixel - pixels_offset;
          int mark_end_int = (mark_end > 20000.0 || mark_end < -20000.0)
                                 ? start
                                 : (int)mark_end;

          for (auto s : _session->get_signals()) {
            int binded_index = dec->binded_probe_index(probe);
            if ((s->get_index() == binded_index) &&
                s->signal_type() == SR_CHANNEL_LOGIC) {
              view::LogicSignal *logicSig = (view::LogicSignal *)s;
              logicSig->paint_mark(p, start, mark_end_int, type / 100);
              break;
            }
          }
        }
      }
    }
  }
}

void DecodeTrace::draw_nodetail(QPainter &p, int h, int left, int right, int y,
                                size_t base_colour, QColor fore, QColor back) {
  (void)base_colour;
  (void)back;

  const QRectF nodetail_rect(left, y - h * 0.5 + 0.5, right - left, h);
  QString info =
      L_S(STR_PAGE_DLG, S_ID(ZOOM_IN_FOR_DETAILS), "Zoom in for details");
  int info_left =
      nodetail_rect.center().x() - p.boundingRect(QRectF(), 0, info).width();
  int info_right =
      nodetail_rect.center().x() + p.boundingRect(QRectF(), 0, info).width();
  int height = p.boundingRect(QRectF(), 0, info).height();

  p.setPen(fore);
  p.drawLine(left, y, info_left, y);
  p.drawLine(info_right, y, right, y);
  p.drawLine(info_left, y, info_left + 5, y - height / 2 + 0.5);
  p.drawLine(info_left, y, info_left + 5, y + height / 2 + 0.5);
  p.drawLine(info_right, y, info_right - 5, y - height / 2 + 0.5);
  p.drawLine(info_right, y, info_right - 5, y + height / 2 + 0.5);

  p.setPen(fore);
  p.drawText(nodetail_rect, Qt::AlignCenter | Qt::AlignVCenter, info);
}

void DecodeTrace::draw_instant(const pv::data::decode::Annotation &a,
                               QPainter &p, QColor fill, QColor outline,
                               QColor text_color, int h, double x, int y,
                               double min_annWidth) {
  (void)outline;

  const QString text =
      a.annotations().empty() ? QString() : a.annotations().back();
  //	const double w = min((double)p.boundingRect(QRectF(), 0, text).width(),
  //		0.0) + h;
  const double w = min(min_annWidth, (double)h);
  const QRectF rect(x - w / 2, y - h * 0.5, w, h);

  // p.setPen(outline);
  p.setPen(QPen(Qt::NoPen));
  p.setBrush(fill);
  p.drawRoundedRect(rect, h * 0.5, h * 0.5);

  // Skip extremely expensive DirectWrite text rendering if it's too small to
  // read anyway
  if (w > 4.0) {
    p.setPen(text_color);
    p.setRenderHint(QPainter::TextAntialiasing, false);
    QFont dec_font = theme_font_decoder();
    p.setFont(dec_font);

    p.drawText(rect, Qt::AlignCenter | Qt::AlignVCenter, text);
  }
}

void DecodeTrace::draw_range(const pv::data::decode::Annotation &a, QPainter &p,
                             QColor fill, QColor outline, QColor text_color,
                             int h, double start, double end, int y,
                             QColor fore, QColor back) {
  (void)fore;

  const double top = y + .5 - h * 0.5;
  const double bottom = y + .5 + h * 0.5;

  p.setPen(outline);
  p.setBrush(fill);

  // If the two ends are within 2 pixel, draw a vertical line.
  // Use fill color (not outline) so the visual color stays consistent
  // with zoomed-in annotations — outline is ~50% darker by design.
  if (start + 2.0 > end) {
    p.setPen(fill);
    p.drawLine(QPointF(start, top), QPointF(start, bottom));
    return;
  }

  const std::vector<QString> &annotations = a.annotations();

  double cap_width = min((end - start) / 4, EndCapWidth);

  QPointF pts[] = {
      QPointF(start, y + .5f),          QPointF(start + cap_width, top),
      QPointF(end - cap_width, top),    QPointF(end, y + .5f),
      QPointF(end - cap_width, bottom), QPointF(start + cap_width, bottom)};

  p.setPen(back);
  p.drawConvexPolygon(pts, countof(pts));

  if (annotations.empty())
    return;

  QRectF rect(start + cap_width, y - h * 0.5, end - start - cap_width * 2, h);
  if (rect.width() <= 4)
    return;

  p.setPen(text_color);
  p.setRenderHint(QPainter::TextAntialiasing, false);
  QFont dec_font = theme_font_decoder();
  p.setFont(dec_font);

  // Get best annotation representation using the new high-performance cache
  const QString best_annotation =
      a.get_cached_best_annotation(rect.width(), p.font(), p.fontMetrics());

  const QString elided =
      p.fontMetrics().elidedText(best_annotation, Qt::ElideRight, rect.width());
  p.drawText(rect, Qt::AlignCenter, elided);
}

void DecodeTrace::draw_error(QPainter &p, const QString &message, int left,
                             int right) {
  const int y = get_y();
  const int h = get_totalHeight();

  const QRectF text_rect(left, y - h * 0.5 + 0.5, right - left, h);
  const QRectF bounding_rect =
      p.boundingRect(text_rect, Qt::AlignCenter, message);
  p.setPen(Qt::red);

  if (bounding_rect.width() < text_rect.width())
    p.drawText(text_rect, Qt::AlignCenter,
               L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODETRACE_ERROR1), "Error:") +
                   message);
  else
    p.drawText(
        text_rect, Qt::AlignCenter,
        L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DECODETRACE_ERROR2), "Error: ..."));
}

void DecodeTrace::draw_unshown_row(QPainter &p, int y, int h, int left,
                                   int right, QString info, QColor fore,
                                   QColor back) {
  (void)back;

  // Skip if this row doesn't intersect the visible clip region at all
  QRect rowRect(left, y - h / 2, right - left, h);
  if (!rowRect.intersects(p.clipBoundingRect().toRect()))
    return;

  // Use QFontMetrics instead of p.boundingRect() — the latter triggers
  // Qt's full text layout engine on every call, which is very expensive
  // (20-30ms) when done 60fps × N rows across the full viewport width.
  QFontMetrics fm(p.font());
  int textWidth = fm.horizontalAdvance(info);
  int textHeight = fm.height();

  const QRectF unshown_rect(left, y - h * 0.5 + 0.5, right - left, h);
  int info_left = unshown_rect.center().x() - textWidth;
  int info_right = unshown_rect.center().x() + textWidth;

  p.setPen(fore);
  p.drawLine(left, y, info_left, y);
  p.drawLine(info_right, y, right, y);
  p.drawLine(info_left, y, info_left + 5, y - textHeight / 2 + 0.5);
  p.drawLine(info_left, y, info_left + 5, y + textHeight / 2 + 0.5);
  p.drawLine(info_right, y, info_right - 5, y - textHeight / 2 + 0.5);
  p.drawLine(info_right, y, info_right - 5, y + textHeight / 2 + 0.5);

  p.drawText(unshown_rect, Qt::AlignCenter | Qt::AlignVCenter, info);
}

void DecodeTrace::on_new_decode_data() {
  bool is_running = _decoder_stack->IsRunning();

  // Start tracking decode duration on first call
  if (!_decode_elapsed_timer.isValid())
    _decode_elapsed_timer.start();

  // Adaptive throttle: longer decode sessions get lower refresh rates.
  // Short decodes (< 2s) stay responsive at 30 FPS.
  // Long decodes progressively reduce to avoid O(N) repaint overhead.
  const qint64 decode_duration_ms = _decode_elapsed_timer.elapsed();
  qint64 throttle_ms;
  if (decode_duration_ms < 500)
    throttle_ms = 33; // ~30 FPS for first 500ms
  else if (decode_duration_ms < 2000)
    throttle_ms = 200; // 5 FPS for 500ms-2s
  else
    throttle_ms = 1000; // 1 FPS for all longer decodes

  qint64 elapsed = _update_timer.isValid() ? _update_timer.elapsed() : 999999;
  if (is_running && elapsed < throttle_ms) {
    return;
  }
  _update_timer.start();

  // 1. Update progress
  decoded_progress(_decoder_stack->get_progress());

  // 2. Trigger geometry layout updates if height changed
  const int expectedHeight = rows_size() * _view->get_signalHeight();
  if (_totalHeight != expectedHeight) {
    _view->signals_changed(NULL);
  }

  // 3. Request lightweight viewport repaint only
  // Do NOT call data_updated() which rebuilds headers, margins, scrollbars,
  // and marks the entire pixmap cache dirty. Decode data changes only affect
  // the decode trace rendering, not view layout or logic signal cache.
  if (_view && _view->session().is_stopped_status()) {
    _view->viewport_update();
  }
}

int DecodeTrace::get_progress() { return _decoder_stack->get_progress(); }

void DecodeTrace::on_decode_done() {
  // Reset decode duration timer so next decode session starts fresh at 30 FPS
  _decode_elapsed_timer.invalidate();

  // Always emit the final progress (100%) and update the viewport,
  // bypassing the throttle in on_new_decode_data() which can swallow
  // the final progress update when is_running is still true and
  // elapsed < 20ms, leaving the progress bar stuck at 99%.
  decoded_progress(_decoder_stack->get_progress());

  // Always recalculate layout after decode completes.
  // This is critical for MCP-initiated decodes where set_data_document()
  // may have been called without triggering signals_changed(), leaving
  // the DecodeTrace at an incorrect y position (e.g. off-screen).
  // Without this, the decode track appears empty until the user manually
  // triggers a layout recalculation (e.g. by re-capturing).
  if (_view) {
    _view->signals_changed(NULL);
  }

  if (_view && _view->session().is_stopped_status()) {
    _view->viewport_update();
  }

  _session->decode_done();
}

int DecodeTrace::rows_size() {
  using pv::data::decode::Decoder;
  int size = 0;

  for (auto dec : _decoder_stack->stack()) {
    if (dec->shown()) {
      auto rows = _decoder_stack->get_rows_gshow();

      for (auto i = rows.begin(); i != rows.end(); i++) {
        pv::data::decode::Row _row = (*i).first;
        if (_row.decoder() == dec->decoder() &&
            _decoder_stack->has_annotations((*i).first) && (*i).second)
          size++;
      }
    } else {
      size++;
    }
  }
  return size == 0 ? 1 : size;
}

void DecodeTrace::paint_type_options(QPainter &p, int right, const QPoint pt,
                                     QColor fore) {
  (void)pt;

  int y = get_y();
  const QRectF group_index_rect = get_rect(CHNLREG, y, right);
  QString index_string;
  int last_index;
  p.setPen(fore);
  _index_list.sort();
  std::list<int>::iterator i = _index_list.begin();
  last_index = (*i);
  index_string = QString::number(last_index);

  while (++i != _index_list.end()) {
    if ((*i) == last_index + 1 && index_string.indexOf("-") < 3 &&
        index_string.indexOf("-") > 0)
      index_string.replace(QString::number(last_index), QString::number((*i)));
    else if ((*i) == last_index + 1)
      index_string = index_string + "-" + QString::number((*i));
    else
      index_string = index_string + "," + QString::number((*i));
    last_index = (*i);
  }

  p.setPen(fore);
  p.drawText(group_index_rect, Qt::AlignRight | Qt::AlignVCenter, index_string);
}

QRectF DecodeTrace::get_rect(DecodeSetRegions type, int y, int right) {
  const QSizeF name_size(right - get_leftWidth() - get_rightWidth(),
                         SquareWidth);

  if (type == CHNLREG)
    return QRectF(get_leftWidth() + name_size.width() + Margin,
                  y - SquareWidth / 2, SquareWidth * SquareNum, SquareWidth);
  else
    return QRectF(0, 0, 0, 0);
}

void DecodeTrace::frame_ended() {
  const uint64_t last_samples = _session->cur_samplelimits() - 1;

  if (_decode_start > last_samples) {
    _decode_start = 0;
    _decode_cursor1 = 0;
  }

  if (_decode_cursor2 == 0 || _decode_end > last_samples) {
    _decode_end = last_samples;
    _decode_cursor2 = 0;
  }

  decoder()->frame_ended();

  for (auto dec : _decoder_stack->stack()) {
    dec->set_decode_region(_decode_start, _decode_end);
    dec->commit();
  }
}

void *DecodeTrace::get_key_handel() { return _decoder_stack->get_key_handel(); }

// to show decoder's property setting dialog
bool DecodeTrace::create_popup(bool isnew) {
  (void)isnew;

  int ret = false; // setting have changed flag
  bool bOpenDlg = true;

  while (bOpenDlg) {
    bOpenDlg = false;
    QWidget *top = AppControl::Instance()->GetTopWindow();
    dialogs::DecoderOptionsDlg dlg(top);
    dlg.set_cursor_range(_decode_cursor1, _decode_cursor2);
    dlg.load_options(this);

    int dlg_ret = dlg.exec();

    if (QDialog::Accepted == dlg_ret) {
      dlg.apply_setting();

      for (auto dec : _decoder_stack->stack()) {
        if (dec->commit() || _decoder_stack->options_changed()) {
          _decoder_stack->set_options_changed(true);
          _decode_start = dec->decode_start();
          _decode_end = dec->decode_end();
          ret = true;
        }
      }

      dlg.get_cursor_range(_decode_cursor1, _decode_cursor2);

      // Reopen the dialog to select the required probes.
      if (ret && _decoder_stack->check_required_probes() == false) {
        QString errMsg =
            L_S(STR_PAGE_MSG, S_ID(IDS_MSG_DECODERSTACK_DECODE_WORK_ERROR),
                "One or more required channels have not been specified");
        MsgBox::Show(errMsg);

        ret = false;
        bOpenDlg = true;
      }
    }

    if (dlg.is_reload_form()) {
      ret = false;
      bOpenDlg = true;
    }
  }

  return ret;
}

} // namespace view
} // namespace pv
