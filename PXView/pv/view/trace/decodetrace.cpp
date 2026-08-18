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

#include "pv/view/trace/decodetrace.h"
#include "pv/config/appconfig.h"
#include "pv/data/decode/annotation.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/rowdata.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/dialogs/decoderoptionsdlg.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"
#include "pv/toolbars/titlebar.h"
#include "pv/ui/dockfonts.h"
#include "pv/ui/dscombobox.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/view/cursor/cursor.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/view.h"
#include "pv/widgets/decodergroupbox.h"
#include "pv/widgets/decodermenu.h"
#include "pv/base/perflog.h"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <climits>
#include <libsigrokdecode.h>
#include <QDir>
#include <cstdio>
#include <cstdarg>

using namespace std;

namespace pv {
namespace view {
#include "pv/config/appconfig.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <limits>
#include <vector>

const int DecodeTrace::ArrowSize = 4;
const double DecodeTrace::EndCapWidth = 5;
const int DecodeTrace::DrawPadding = 100;
const int DecodeTrace::ControlRectWidth;

// Debug instrumentation: OFF by default. Enable temporarily while diagnosing
// annotation-visibility bugs by compiling with -DPXVIEW_DECODE_DEBUG (or by
// flipping the #if below). When ON it overlays diagnostic drawing on the
// decoder tracks AND writes a per-row numeric summary to
// %TEMP%/pxv_decode_dbg.log. The overlay brute-forces every annotation in
// the row every frame (O(total annotations)), so it must stay OFF in normal
// builds.
static bool pxv_decode_debug() {
#if defined(PXVIEW_DECODE_DEBUG)
  return true;
#else
  return false;
#endif
}
static void pxv_decode_log(const char *fmt, ...) {
#if defined(PXVIEW_DECODE_DEBUG)
  static long fcount = 0;
  fcount++;
  const char *mode = (fcount == 1) ? "w" : "a";
  QString path = QDir::temp().filePath("pxv_decode_dbg.log");
  FILE *lf = fopen(path.toUtf8().constData(), mode);
  if (!lf)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(lf, fmt, ap);
  va_end(ap);
  fclose(lf);
#else
  (void)fmt;
#endif
}

// ----------------------------------------------------------------------------
// Decode / viewport render performance instrumentation.
// Implemented in pv/base/perflog.h (included at global scope above). OFF
// unless the build defines PXVIEW_DECODE_PERF (CMake option
// ENABLE_DECODE_PERF). The local macros below keep call sites readable.
// ----------------------------------------------------------------------------
#define PXV_PERF_FRAME_START()  PXV_PERF_PAINTMID_START()
#define PXV_PERF_FRAME_END()    PXV_PERF_PAINTMID_END()


QColor DecodeTrace::getChannelColor(int channelIndex) {
  QColor c = AppConfig::Instance().GetThemeColor(
      QString("@decoder-channel-%1").arg(channelIndex));
  if (c.isValid())
    return c;

  // Fallback original Colours array
  static const QColor defaultColours[16] = {
      QColor(0xEF, 0x29, 0x29), QColor(0xF6, 0x6A, 0x32),
      QColor(0xFC, 0xAE, 0x3E), QColor(0xFB, 0xCA, 0x47),
      QColor(0xFC, 0xE9, 0x4F), QColor(0xCD, 0xF0, 0x40),
      QColor(0x8A, 0xE2, 0x34), QColor(0x4E, 0xDC, 0x44),
      QColor(0x55, 0xD7, 0x95), QColor(0x64, 0xD1, 0xD2),
      QColor(0x72, 0x9F, 0xCF), QColor(0xD4, 0x76, 0xC4),
      QColor(0x9D, 0x79, 0xB9), QColor(0xAD, 0x7F, 0xA8),
      QColor(0xC2, 0x62, 0x9B), QColor(0xD7, 0x47, 0x6F)};
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
  QColor c = AppConfig::Instance().GetThemeColor(
      QString("@decoder-ann-%1").arg(channelIndex));
  if (c.isValid())
    return c;
  return getChannelColor(channelIndex);
}

QColor DecodeTrace::getAnalogChannelColor(int channelIndex) {
  // Keep the decoder-generated analog row title and its waveform on exactly
  // the same colour source. CH0 keeps the established bright-green waveform
  // colour; the remaining channels follow the decoder annotation palette.
  return channelIndex == 0
      ? QColor(0x4E, 0xDC, 0x44)
      : getAnnColor((channelIndex + 1) % 16);
}

QColor DecodeTrace::getAnnOutlineColor(int channelIndex) {
  QColor c = AppConfig::Instance().GetThemeColor(
      QString("@decoder-ann-outline-%1").arg(channelIndex));
  if (c.isValid())
    return c;
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
                         std::shared_ptr<pv::data::DecoderStack> decoder_stack,
                         int index)
    : Trace([&]() {
            assert(decoder_stack);
            QString name = QString::fromUtf8(
                decoder_stack->stack().front()->decoder()->name);
            QString lbl = decoder_stack->label();
            if (lbl.isEmpty())
                lbl = decoder_stack->auto_label();
            if (!lbl.isEmpty())
                name += "(" + lbl + ")";
            return name;
          }(),
            index, SR_CHANNEL_DECODER) {

  _colour = getChannelColor(index % 16);

  _decoder_stack = decoder_stack;
_data_source = session;
// P0-3 fix: _delete_flag removed.
  _decode_cursor1 = 0;
  _decode_cursor2 = 0;

  connect(_decoder_stack.get(), &data::DecoderStack::new_decode_data, this,
          &DecodeTrace::on_new_decode_data);

  connect(_decoder_stack.get(), &data::DecoderStack::decode_done, this,
          &DecodeTrace::on_decode_done);

  connect(_decoder_stack.get(), &data::DecoderStack::error_message_changed,
          this, &DecodeTrace::on_error_message_changed);
}

DecodeTrace::~DecodeTrace() {
  _cur_row_headings.clear();

  // NOTE: The DecoderStack is owned by the Core layer (SigSession /
  // SessionDocument) and is deleted by SigSession::remove_decoder() /
  // clear_all_decoder() / clear_all_documents_decoders(). The View only
  // holds a non-owning pointer (_decoder_stack) for rendering purposes.
  // Deleting it here would cause a double-free.
  // The Qt signal/slot connections (new_decode_data / decode_done) are
  // automatically disconnected by Qt when either sender or receiver is
  // destroyed.
  _decoder_stack = nullptr;
}

bool DecodeTrace::enabled() { return visible(); }

void DecodeTrace::set_view(pv::view::View *view) {
  assert(view);
  Trace::set_view(view);
}

void DecodeTrace::paint_back(QPainter &p, int left, int right, QColor fore,
                             QColor back, const PaintContext &ctx) {
  (void)back;
  (void)ctx;

  // Skip paint if this trace has not been laid out yet. _v_offset is
  // initialized to INT_MAX by the Trace constructor and set to a real
  // pixel position by layout_time_signals(). Painting with INT_MAX
  // draws at an absurd Y coordinate (visible as a stray line at the
  // viewport edge or silently clipped, depending on platform).
  if (get_v_offset() == INT_MAX)
    return;

  QColor backFore = fore;
  backFore.setAlpha(View::BackAlpha);
  QPen pen(backFore);
  pen.setStyle(Qt::DotLine);
  p.setPen(pen);
  const double sigY = get_y();
  p.drawLine(left, sigY, right, sigY);

  // --draw decode region control
  uint64_t doc_samplerate = 0;
  if (_data_source->get_active_document()) {
    doc_samplerate = _data_source->get_active_document()->cur_snap_samplerate();
  }
  const double samples_per_pixel =
      (doc_samplerate > 0 ? doc_samplerate : _data_source->cur_snap_samplerate()) *
      ctx.scale;

  uint64_t d_start = 0;
  uint64_t d_end = INT64_MAX;
  if (!_decoder_stack->stack().empty()) {
    d_start = _decoder_stack->stack().front()->decode_start();
    d_end = _decoder_stack->stack().front()->decode_end();
    if (d_end == 0) d_end = INT64_MAX;
  }

  const double startX = d_start / samples_per_pixel - ctx.offset;
  const double endX = d_end / samples_per_pixel - ctx.offset;
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

  // --draw headings. Analog channels occupy two normal height units, so use
  // the same mixed-height geometry as paint_mid().
  const int unit_count = rows_size();
  const int base_h = unit_count > 0 ? _totalHeight / unit_count
                                     : ctx.signal_height;
  const int analog_h = base_h * 2;
  std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> visible_analog;
  for (const auto &ch : _decoder_stack->analog_data_copy())
    if (ch && ch->visible())
      visible_analog.push_back(ch);
  const int analog_count = static_cast<int>(visible_analog.size());
  const int annotation_rows = std::max(
      0, static_cast<int>(_cur_row_headings.size()) - analog_count);
  int cur_y = get_y() - _totalHeight / 2;
  _indicator_button_rect = QRectF();

  for (size_t i = 0; i < _cur_row_headings.size(); i++) {
    const bool is_analog = static_cast<int>(i) >= annotation_rows;
    const int row_h = is_analog ? analog_h : base_h;
    p.setPen(QPen(Qt::NoPen));
    p.setBrush(QApplication::palette().brush(QPalette::WindowText));

    const QRect r(left + ArrowSize * 2, cur_y, right - left, row_h);
    const QString h(_cur_row_headings[i]);
    const int f = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip;
    const QPointF points[] = {QPointF(left, r.center().y() - ArrowSize),
                              QPointF(left + ArrowSize, r.center().y()),
                              QPointF(left, r.center().y() + ArrowSize)};
    p.drawPolygon(points, countof(points));
    QColor heading_color = fore;
    if (is_analog) {
      // Analog headings are appended in the same visible-channel order used
      // by paint_mid(). Resolve the real decoder channel and reuse the exact
      // waveform colour so CH0/CH1/... labels visually match their curves.
      const int analog_index = static_cast<int>(i) - annotation_rows;
      if (analog_index >= 0 &&
          analog_index < static_cast<int>(visible_analog.size()) &&
          visible_analog[analog_index]) {
        heading_color = getAnalogChannelColor(
            visible_analog[analog_index]->channel());
      }
    }
    p.setPen(heading_color);
    p.drawText(r, f, h);

    // V-ZOOM/V-POS indicator buttons for analog channels
    if (is_analog && _indicator_heading_row == static_cast<int>(i)) {
      const int cy = r.center().y();
      const int cx = r.left() + p.fontMetrics().horizontalAdvance(h) + 8;
      const int gap = 4;
      const int ah = 5;
      QColor c(heading_color);
      c.setAlpha(210);
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      const QPointF up[] = {QPointF(cx, cy-gap), QPointF(cx-ah, cy-gap-6),
                            QPointF(cx+ah, cy-gap-6)};
      const QPointF dn[] = {QPointF(cx, cy+gap), QPointF(cx-ah, cy+gap+6),
                            QPointF(cx+ah, cy+gap+6)};
      p.drawPolygon(up, 3);
      p.drawPolygon(dn, 3);

      const int bx = cx + ah + 6;
      const int bw = 14;
      const int bh = std::max(12, row_h - 6);
      const int by = cy - bh/2;
      _indicator_button_rect = QRectF(bx, by, bw, bh);
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(c, 1, Qt::DashLine));
      p.drawRoundedRect(_indicator_button_rect, 2, 2);
      const int mx = bx + bw/2, my = by + bh/2, rr = 3;
      p.setPen(QPen(c, 1));
      p.drawLine(mx-rr, my, mx+rr, my);
      p.drawLine(mx, my-rr, mx, my+rr);
      p.drawEllipse(QPointF(mx, my), rr, rr);
    }
    cur_y += row_h;
  }
}

void DecodeTrace::paint_mid(QPainter &p, int left, int right, QColor fore,
                            QColor back, const PaintContext &ctx) {
  using namespace pv::data::decode;

  PXV_PERF_FRAME_START();

  // Skip paint if not yet laid out (see paint_back for rationale).
  if (get_v_offset() == INT_MAX) {
    PXV_PERF_FRAME_END();
    return;
  }

  (void)back;

  assert(_decoder_stack);
  const QString err = _decoder_stack->error_message();
  if (!err.isEmpty()) {
    draw_error(p, err, left, right);
  }

  const double scale = ctx.scale;
  if (scale <= 0)
    return;

  double samplerate = _decoder_stack->samplerate();

  _cur_row_headings.clear();

  // Show sample rate as 1Hz when it is unknown
  if (samplerate == 0.0)
    samplerate = 1.0;

  const int64_t pixels_offset = ctx.offset;
  const double samples_per_pixel = samplerate * scale;

  uint64_t start_sample =
      (uint64_t)max((left + pixels_offset) * samples_per_pixel, 0.0);
  uint64_t end_sample =
      (uint64_t)max((right + pixels_offset) * samples_per_pixel, 0.0);

  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    start_sample = max(dec->decode_start(), start_sample);
    uint64_t d_end = dec->decode_end();
    if (d_end == 0) d_end = UINT64_MAX;
    end_sample = min(d_end, end_sample);
    break;
  }

  if (end_sample < start_sample)
    return;

  const int row_count = rows_size();
  const int annotation_height =
      (row_count > 0) ? (_totalHeight / row_count) : ctx.signal_height;

  // Iterate through the rows
  assert(_view);
  int y = get_y() - (_totalHeight - annotation_height) * 0.5;

  assert(_decoder_stack);

  // Scheme A: grab the immutable published snapshot once per frame. All
  // row data (annotation deque, gshow, max/min) is read lock-free from this
  // snapshot, so the render path never contends with the decode thread on
  // _rows_mutex / _visitor_mutex.
  const auto snap = _decoder_stack->published_snapshot();
  // Live-decode LOD state (atomic, lock-free). While decoding is in progress
  // and the viewport rides the decode frontier, rows are rendered as dense
  // colour blocks instead of per-annotation text; once decoding stops, the
  // detailed per-annotation text rendering is restored.
  const bool decoding = _decoder_stack->IsRunning();

  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    if (dec->shown()) {
      // Iterate only the snapshot rows belonging to this decoder.
      if (snap) {
        for (const auto &i : *snap) {
          const Row &row = i.first;
          if (row.decoder() != dec->decoder())
            continue;
          const pv::data::DecoderStack::SnapshotRow &srow = i.second;
          if (!srow.data || srow.data->empty())
            continue;  // was: has_annotations() == false
          if (!srow.gshow)
            continue;

          // Per-track-row timing breakdown (perf build only).
          const QString _perf_row_name = row.title();
          const auto _perf_row_t0 = std::chrono::steady_clock::now();
          double _perf_vr_ms = 0;
          size_t _perf_ann = 0;
          bool _perf_is_dense = false;

          // Use the maximum annotation width to decide whether the whole row
          // is truly dense; inside the normal path, tiny individual fragments
          // still fall back to a cheap color block.
          const uint64_t max_annotation = srow.data->get_max_annotation();
          const double max_ann_width = max_annotation / samples_per_pixel;

          // Frontier LOD: if the decode thread is still running and the
          // viewport overlaps the newest decoded samples of this row, fall
          // back to the dense block path even at high zoom. That region is
          // repainted at every publish (~60Hz), and per-annotation drawText
          // there is what saturated the GUI thread; colour blocks keep the
          // per-frame cost bounded until decoding completes.
          const uint64_t row_frontier = srow.data->get_max_sample();
          const uint64_t screen_samples = (uint64_t)std::max(
              1.0, (double)(right - left) * samples_per_pixel);
          const bool near_frontier =
              decoding && (row_frontier >= start_sample) &&
              (row_frontier <= end_sample + screen_samples);
          const bool use_dense = (max_ann_width < 2.0) || near_frontier;
          _perf_is_dense = use_dense;

          const RowDataSnapshot *const row_data = srow.data.get();
          int dbg_blocks = 0;
          {
            if (use_dense) {
                // Pixel-bucket pass (O(screen width) draw, single O(N) linear
                // scan over visible annotations). The old block-walk called
                // get_first_annotation_ending_after() once PER visible bit
                // annotation; at small zoom a bit row (e.g. CAN Bits) holds
                // millions of sub-pixel annotations, so that walk was
                // O(N·log N) and could cost >200ms/frame. Bucketing aggregates
                // every sub-pixel annotation into one colour block per screen
                // column (last-writer-wins, identical to the MID dense path),
                // bounding cost by viewport width regardless of annotation
                // count. Behaviour/visuals unchanged for real dense rows.
                const auto _perf_vr_t0 = std::chrono::steady_clock::now();
                auto range =
                    row_data->get_visible_range(start_sample, end_sample);
                _perf_vr_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - _perf_vr_t0).count();
                _perf_ann = range.second - range.first;
                const size_t start_idx = range.first;
                const size_t end_idx = range.second;

                if (start_idx < end_idx) {
                  const int vis_width = right - left + 1;
                  std::vector<uint8_t> col_valid(vis_width, 0);
                  std::vector<uint8_t> col_color(vis_width, 0);
                  QColor dense_colors[16];
                  for (int c = 0; c < 16; c++)
                    dense_colors[c] = getAnnColor(c);

                  row_data->for_each_index(
                      start_idx, end_idx, [&](const Annotation &a, size_t) {
                        const double x = a.start_sample() / samples_per_pixel -
                                         pixels_offset;
                        if (x < left - DrawPadding || x > right + DrawPadding)
                          return;
                        const int col = (int)x;
                        if (col >= left && col < left + vis_width) {
                          col_valid[col - left] = 1;
                          col_color[col - left] =
                              (uint8_t)((a.type() % MaxAnnType) % 16);
                        }
                      });

                  for (int i = 0; i < vis_width; i++) {
                    if (!col_valid[i])
                      continue;
                    p.fillRect(QRectF((double)(left + i),
                                      y - annotation_height * 0.5, 1.0,
                                      annotation_height),
                               dense_colors[col_color[i]]);
                    dbg_blocks++;
                  }
                }
              } else {
                const auto _perf_vr_t0 = std::chrono::steady_clock::now();
                auto range =
                    row_data->get_visible_range(start_sample, end_sample);
                _perf_vr_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - _perf_vr_t0).count();
                _perf_ann = range.second - range.first;
                const size_t start_idx = range.first;
                const size_t end_idx = range.second;

                if (start_idx < end_idx) {
                  const int vis_width = right - left + 1;

                  // When the visible slice contains far more annotations
                  // than screen pixels (e.g. ~1.5M PWM annotations zoomed
                  // out to the full capture), aggregating every sub-pixel
                  // fragment into a per-column colour bucket keeps the draw
                  // cost at O(screen width) instead of O(N) fillRect calls.
                  const size_t dense_threshold =
                      (size_t)std::max(8, vis_width) * 4;
                  const bool dense = (end_idx - start_idx) > dense_threshold;

                  std::vector<uint8_t> col_valid;
                  std::vector<uint8_t> col_color;
                  QColor dense_colors[16];
                  if (dense) {
                    col_valid.assign(vis_width, 0);
                    col_color.assign(vis_width, 0);
                    for (int c = 0; c < 16; c++)
                      dense_colors[c] = getAnnColor(c);
                  }

                  // Iterate the visible range under a single shared lock
                  // (one lock per row instead of one per annotation).
                  row_data->for_each_index(
                      start_idx, end_idx, [&](const Annotation &a, size_t) {
                        const uint64_t span_samples =
                            a.end_sample() > a.start_sample()
                                ? a.end_sample() - a.start_sample()
                                : 0;
                        const double ann_width =
                            span_samples / samples_per_pixel;
                        if (ann_width < 2.0) {
                          // Tiny individual fragment: cheap colour block
                          const double x = a.start_sample() / samples_per_pixel -
                                           pixels_offset;
                          if (x < left - DrawPadding ||
                              x > right + DrawPadding)
                            return;
                          if (dense) {
                            // Bucket by pixel column; last writer wins,
                            // matching the previous per-annotation
                            // overpaint behaviour.
                            const int col = (int)x;
                            if (col >= left && col < left + vis_width) {
                              col_valid[col - left] = 1;
                              col_color[col - left] =
                                  (uint8_t)((a.type() % MaxAnnType) % 16);
                            }
                          } else {
                            const size_t colour =
                                (a.type() % MaxAnnType) % 16;
                            const QColor fill = getAnnColor(colour);
                            p.fillRect(QRectF(x, y - annotation_height * 0.5,
                                              std::max(1.0, ann_width),
                                              annotation_height),
                                       fill);
                          }
                        } else {
                          draw_annotation(a, p, get_text_colour(),
                                          annotation_height, left, right,
                                          samples_per_pixel, pixels_offset, y,
                                          0, ann_width, fore, back);
                        }
                      });

                  // Flush dense per-column buckets: one fillRect per pixel
                  // column instead of per annotation.
                  if (dense) {
                    for (int i = 0; i < vis_width; i++) {
                      if (!col_valid[i])
                        continue;
                      p.fillRect(QRectF((double)(left + i),
                                        y - annotation_height * 0.5, 1.0,
                                        annotation_height),
                                 dense_colors[col_color[i]]);
                    }
                  }
                }
              }
            }

          if (pxv_decode_debug()) {
            // ---- Debug overlay: mark HIDDEN annotations and WHY ----
            // Brute-force every annotation that actually overlaps the visible
            // window [start_sample, end_sample]. For each we recover its GLOBAL
            // index via get_annotation_index(start_sample) and test membership
            // in the lookup range [drange). Then classify:
            //   - baseline : cyan (straddles an edge) / green (fully inside) ->
            //                this one WAS looked up (may still be clipped).
            //   - RED  : overlaps window but global idx is OUTSIDE [drange) ->
            //            "NOT IN LOOKUP RANGE" (get_visible_range /
            //            _first_end_after missed it -> the row disappears).
            //   - ORANGE: inside [drange) BUT its clamped pixel span is fully
            //            outside the viewport padding -> "CLIPPED OUT" by
            //            draw_annotation's edge test.
            // This directly answers "which line segments are hidden and why".
            auto drange = row_data->get_visible_range(start_sample, end_sample);
            int hidden_before_begin = 0;
            int hidden_after_end = 0;
            int hidden_clipped = 0;
            int total_overlap = 0;

            row_data->for_each_index(0, row_data->get_annotation_size(),
                [&](const Annotation &a, size_t idx) {
                  if (a.start_sample() > end_sample ||
                      a.end_sample() < start_sample)
                    return;  // does not overlap the window at all
                  total_overlap++;
                  const double as =
                      a.start_sample() / samples_per_pixel - pixels_offset;
                  const double ae =
                      a.end_sample() / samples_per_pixel - pixels_offset;
                  const bool straddle = (a.start_sample() < start_sample) ||
                                        (a.end_sample() > end_sample);

                  const bool in_lookup =
                      (idx >= drange.first && idx < drange.second);
                  const bool clipped =
                      (as > right + DrawPadding) || (ae < left - DrawPadding);

                  bool is_hidden = false;
                  QColor hc;
                  if (!in_lookup) {
                    is_hidden = true;
                    if (idx < drange.first) {
                      // Global index BEFORE the lookup begin: get_visible_range
                      // / _first_end_after skipped this overlapping annotation.
                      hc = QColor(255, 0, 0, 210);  // RED = before lookup begin
                      hidden_before_begin++;
                    } else {
                      // Global index AT/AFTER the lookup end: lookup upper bound
                      // excluded an overlapping annotation that should be shown.
                      hc = QColor(180, 0, 80, 210);  // DARK-RED = after lookup end
                      hidden_after_end++;
                    }
                  } else if (clipped) {
                    is_hidden = true;
                    hc = QColor(255, 140, 0, 210);  // ORANGE = clipped out
                    hidden_clipped++;
                  }

                  if (is_hidden) {
                    p.fillRect(QRectF(as, y - annotation_height * 0.45,
                                      std::max(1.0, ae - as),
                                      annotation_height * 0.9), hc);
                  } else {
                    QColor c = straddle ? QColor(0, 200, 255, 90)
                                        : QColor(0, 220, 0, 90);
                    p.fillRect(QRectF(as, y - annotation_height * 0.45,
                                      std::max(1.0, ae - as),
                                      annotation_height * 0.9), c);
                  }
                });

            const QString dbg = QString(
                "[%1] rng=%2..%3 n=%4 spp=%5 maxw=%6 B/A/O=%7/%8/%9 aH=%10")
                .arg(use_dense ? "DENSE" : "MID")
                .arg(drange.first)
                .arg(drange.second)
                .arg(row_data->get_annotation_size())
                .arg(samples_per_pixel, 0, 'f', 1)
                .arg(max_ann_width, 0, 'f', 1)
                .arg(hidden_before_begin)
                .arg(hidden_after_end)
                .arg(hidden_clipped)
                .arg(annotation_height);
            p.setPen(QColor(255, 255, 0));
            p.drawText(QRectF(left, y - annotation_height * 0.5,
                              right - left, annotation_height),
                       Qt::AlignRight | Qt::AlignVCenter, dbg);

            // File log with the hidden breakdown.
            {
              static long fcount = 0;
              fcount++;
              const uint64_t winL =
                  (uint64_t)max((left + pixels_offset) * samples_per_pixel, 0.0);
              const uint64_t winR =
                  (uint64_t)max((right + pixels_offset) * samples_per_pixel, 0.0);
              int true_hits = 0;
              row_data->for_each_index(0, row_data->get_annotation_size(),
                  [&](const Annotation &a, size_t) {
                    if (a.start_sample() <= end_sample &&
                        a.end_sample() >= start_sample)
                      true_hits++;
                  });
              const bool empty_range = (drange.first >= drange.second);
              const bool anomaly =
                  (empty_range && true_hits > 0) ||
                  (use_dense && dbg_blocks == 0 && true_hits > 0) ||
                  (hidden_before_begin + hidden_after_end + hidden_clipped) > 0;
              if (fcount % 4 == 0 || anomaly) {
                pxv_decode_log(
                    "ROW=%-16s PATH=%-5s spp=%11.4f maxw=%9.2f "
                    "winL/R=[%llu,%llu] view=[%llu,%llu] left/right=[%d,%d] "
                    "range=[%llu,%llu] n=%llu blocks=%d overlap=%d "
                    "BEFORE_BEGIN=%d AFTER_END=%d CLIPPED=%d%s\n",
                    qPrintable(row.title()), use_dense ? "DENSE" : "MID",
                    samples_per_pixel, max_ann_width,
                    (unsigned long long)winL, (unsigned long long)winR,
                    (unsigned long long)start_sample,
                    (unsigned long long)end_sample, left, right,
                    (unsigned long long)drange.first,
                    (unsigned long long)drange.second,
                    (unsigned long long)row_data->get_annotation_size(),
                    dbg_blocks, total_overlap,
                    hidden_before_begin, hidden_after_end, hidden_clipped,
                    anomaly ? "  <<< HIDDEN ANNOTATIONS PRESENT" : "");
              }
            }
          }

          const double _perf_row_ms = std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - _perf_row_t0).count();
          pv::base::perf::record_track(_perf_row_name, _perf_row_ms,
                                       _perf_vr_ms, _perf_row_ms - _perf_vr_ms,
                                       _perf_ann);
          pv::base::perf::frame_add_rows(_perf_is_dense ? 1 : 0,
                                         _perf_is_dense ? 0 : 1, _perf_ann);

          y += annotation_height;
          _cur_row_headings.push_back(row.title());
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

  // TDM/PWM analog port: draw decoder-generated waveforms.
  const auto analog_data = _decoder_stack->analog_data_copy();
  if (!analog_data.empty() && _decoder_stack->analog_visible()) {
    const int analog_ch_height = annotation_height * 2;
    y -= annotation_height / 2; // next annotation centre -> analog row top
    p.save();

    const size_t pixel_width = static_cast<size_t>(std::max(1, right - left + 1));
    const size_t render_budget = std::max<size_t>(64, pixel_width * 8);
    p.setRenderHint(QPainter::Antialiasing, true);
    std::vector<pv::data::DecoderAnalogSample> render_samples;
    render_samples.reserve(render_budget + 1);

    pv::data::DecoderAnalogTriggerConfig trigger_cfg;
    const bool trigger_visual =
        _decoder_stack->get_analog_display_trigger_config(trigger_cfg);

    for (const auto &ch_data : analog_data) {
      if (!ch_data || !ch_data->visible())
        continue;

      float min_val = -1.0f;
      float max_val = 1.0f;
      render_samples.clear();
      ch_data->copy_samples_for_render(start_sample, end_sample,
                                       render_budget, render_samples,
                                       min_val, max_val);

      const float mid_y = static_cast<float>(y) +
          ch_data->v_offset() * analog_ch_height * 0.5f;
      const float base_h = static_cast<float>(analog_ch_height) * 0.45f;
      const float v_scale = ch_data->v_scale();
      const bool is_auto = v_scale < 0.001f;
      const float value_center = (min_val + max_val) * 0.5f;
      const float value_range = max_val - min_val;
      const float scale_factor = is_auto
          ? (value_range > 0.001f ? (base_h * 1.8f / value_range) : base_h)
          : (base_h * v_scale);

      p.setPen(QPen(QColor(128, 128, 128), 1, Qt::DotLine));
      p.drawLine(left, static_cast<int>(mid_y), right, static_cast<int>(mid_y));

      // Continuous line + independent per-pixel envelope.
      std::vector<QPointF> line_points;
      std::vector<QPointF> envelope_points;
      line_points.reserve(pixel_width);
      envelope_points.reserve(pixel_width * 2);
      if (!render_samples.empty()) {
        const double inv_spp = 1.0 / samples_per_pixel;
        int cur_col = std::numeric_limits<int>::min();
        float col_min = 0.0f;
        float col_max = 0.0f;
        float col_last = 0.0f;
        bool col_has = false;

        auto value_to_y = [&](float value) -> double {
          const float display_value = is_auto ? (value - value_center) : value;
          return (double)(mid_y - display_value * scale_factor);
        };

        auto flush_col = [&](int px) {
          if (!col_has || px < left || px > right)
            return;
          line_points.emplace_back((double)px, value_to_y(col_last));
          if (col_max > col_min) {
            envelope_points.emplace_back((double)px, value_to_y(col_min));
            envelope_points.emplace_back((double)px, value_to_y(col_max));
          }
          col_has = false;
        };

        for (const auto &sample : render_samples) {
          const uint64_t sp = std::max(start_sample, sample.start_sample);
          const int px = static_cast<int>(sp * inv_spp - pixels_offset);
          if (px < left || px > right)
            continue;

          if (px != cur_col) {
            flush_col(cur_col);
            cur_col = px;
            col_min = col_max = col_last = sample.value;
            col_has = true;
          } else {
            if (!col_has)
              col_min = col_max = sample.value;
            col_min = std::min(col_min, sample.value);
            col_max = std::max(col_max, sample.value);
            col_last = sample.value;
            col_has = true;
          }
        }
        flush_col(cur_col);
      }

      const QColor wave_color = getAnalogChannelColor(ch_data->channel());
      if (envelope_points.size() >= 2) {
        p.setPen(QPen(wave_color, 1.0));
        p.drawLines(envelope_points.data(),
                    static_cast<int>(envelope_points.size() / 2));
      }
      if (line_points.size() > 1) {
        p.setPen(QPen(wave_color, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line_points.data(), static_cast<int>(line_points.size()));
      }

      // Analog display-trigger visualization.
      if (trigger_visual && trigger_cfg.enabled &&
          ch_data->channel() == trigger_cfg.channel) {
        const double eng_lo = ch_data->engineering_minimum();
        const double eng_hi = ch_data->engineering_maximum();
        const double gain = (eng_hi - eng_lo) * 0.5;
        const double offset = (eng_hi + eng_lo) * 0.5;
        double normalized_level = 0.0;
        if (std::abs(gain) > 1e-15)
          normalized_level = (trigger_cfg.level - offset) / gain;
        const double display_level = is_auto
            ? normalized_level - value_center : normalized_level;
        const double level_y = mid_y - display_level * scale_factor;
        const double row_top = y;
        const double row_bottom = y + analog_ch_height;
        const QColor trig_color(235, 80, 80);

        p.save();
        p.setPen(QPen(trig_color, 1.2, Qt::DashLine));
        if (level_y >= row_top && level_y <= row_bottom)
          p.drawLine(QPointF(left, level_y), QPointF(right, level_y));

        const double tx = left + (right - left) *
            std::clamp(trigger_cfg.display_position_percent, 0, 100) / 100.0;
        p.setPen(QPen(trig_color, 1.5));
        p.drawLine(QPointF(tx, row_top), QPointF(tx, row_bottom));
        const QPointF tri[3] = {
            QPointF(tx, row_top + 2), QPointF(tx - 5, row_top + 10),
            QPointF(tx + 5, row_top + 10)};
        p.setBrush(trig_color);
        p.drawPolygon(tri, 3);

        QString edge_text = QStringLiteral("\u2191");
        if (trigger_cfg.edge == pv::data::DecoderAnalogTriggerEdge::Falling)
          edge_text = QStringLiteral("\u2193");
        else if (trigger_cfg.edge == pv::data::DecoderAnalogTriggerEdge::Either)
          edge_text = QStringLiteral("\u2195");
        const QString mode_text =
            trigger_cfg.mode == pv::data::DecoderAnalogTriggerMode::Normal
                ? QStringLiteral("N") : QStringLiteral("A");
        const QString unit = QString::fromStdString(ch_data->engineering_unit());
        const QString label = QStringLiteral("TRIG CH%1 %2 %3 %4%5 @%6%")
            .arg(ch_data->channel()).arg(mode_text).arg(edge_text)
            .arg(QString::number(trigger_cfg.level, 'g', 7))
            .arg(unit.isEmpty() ? QString() : QStringLiteral(" ") + unit)
            .arg(std::clamp(trigger_cfg.display_position_percent, 0, 100));
        p.setPen(trig_color);
        p.setBrush(Qt::NoBrush);
        p.drawText(QPointF(left + 6, row_top + 14), label);
        p.restore();
      }

      _cur_row_headings.push_back(QString::fromStdString(ch_data->label()));
      y += analog_ch_height;
    }
    p.restore();
  }

  PXV_PERF_FRAME_END();
}

void DecodeTrace::paint_fore(QPainter &p, int left, int right, QColor fore,
                             QColor back, const PaintContext &ctx) {
  using namespace pv::data::decode;

  (void)p;
  (void)left;
  (void)right;
  (void)fore;
  (void)back;
  (void)ctx;
}

void DecodeTrace::draw_annotation(const pv::data::decode::Annotation &a,
                                  QPainter &p, QColor text_color, int h,
                                  int left, int right, double samples_per_pixel,
                                  double pixels_offset, int y,
                                  size_t base_colour, double min_annWidth,
                                  QColor fore, QColor back) {
  const double start =
      max(a.start_sample() / samples_per_pixel - pixels_offset, (double)left);
  const double end =
      min(a.end_sample() / samples_per_pixel - pixels_offset, (double)right);

  const size_t colour = ((base_colour + a.type()) % MaxAnnType) % 16;
  const QColor fill = getAnnColor(colour);
  const QColor outline = getAnnOutlineColor(colour);

  if (start > right + DrawPadding || end < left - DrawPadding) {
    if (pxv_decode_debug()) {
      // RED: annotation was skipped because it lies outside the clip region.
      p.fillRect(QRectF(left, y - 2, right - left, 4), QColor(255, 0, 0, 150));
    }
    return;
  }

  // NOTE: The previous two LOD "already-drawn" / sub-pixel-collinear skip
  // guards have been removed. They suppressed valid annotations that merely
  // overlapped a wider one (e.g. an I2C byte's R/W flag under the address
  // bubble), and — because start/end are clamped to the viewport — a wide
  // annotation straddling the window edge expanded the "covered" span across
  // the whole row, silently dropping every annotation inside the viewport
  // ("一串注解有元素在窗口外也会导致这个元素不会绘制"). Drawing all in-range
  // annotations matches upstream PulseView behaviour and guarantees no data is
  // hidden. (Out-of-clip culling above is retained.)

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
    // TDM value bubbles carry long PCM hex text. At medium zoom the
    // time-span rectangle can be narrower than the readable label, so
    // enlarge only the visual capsule around its true time centre.
    // The annotation's real start/end samples are unchanged for measurement
    // and hit-testing.
    double visual_start = start;
    double visual_end = end;
    bool tdm_audio_value = false;
    if (!_decoder_stack->stack().empty()) {
      auto &top_up = _decoder_stack->stack().front();
      auto *top_decoder = top_up.get();
      const srd_decoder *definition = top_decoder ? top_decoder->decoder() : nullptr;
      tdm_audio_value = definition && definition->id &&
                        (std::strcmp(definition->id, "tdm_audio_fast") == 0 ||
                         std::strcmp(definition->id, "tdm_audio_c") == 0);
    }
    if (tdm_audio_value && !a.annotations().empty() && end - start > 2.0) {
      const QFontMetrics fm(theme_font_decoder());
      const int text_width = fm.horizontalAdvance(a.annotations().front());
      const double wanted = std::min(160.0, std::max(48.0, text_width + 24.0));
      if (visual_end - visual_start < wanted) {
        const double center = (visual_start + visual_end) * 0.5;
        visual_start = center - wanted * 0.5;
        visual_end = center + wanted * 0.5;
      }
    }
    draw_range(a, p, fill, outline, text_color, h, visual_start, visual_end,
               y, fore, back);

    // SDA sampling-edge marker deferred to Task 26 (requires paint_mark edge_dir parameter)

    if ((a.type() / 100 == 2) && (end - start > 20)) {
  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
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

          if (_view) {
for (auto &s : _view->get_own_signals()) {
int binded_index = dec->binded_probe_index(probe);
              if ((s->get_index() == binded_index) &&
                  s->signal_type() == SR_CHANNEL_LOGIC) {
                view::LogicSignal *logicSig = (view::LogicSignal *)s.get();
                logicSig->paint_mark(p, start, mark_end_int, type / 100);
                break;
              }
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

QString DecodeTrace::best_annotation_text(
    const pv::data::decode::Annotation &a, double rect_width,
    const QFontMetrics &fm) {
  const std::vector<QString> &ann_list = a.annotations();
  if (ann_list.empty())
    return QString();

  // Try to find an annotation that will fit; pick the longest one that fits.
  QString best_annotation;
  int best_width = 0;

  for (auto &txt : ann_list) {
    const int w = fm.boundingRect(QRect(), 0, txt).width();
    if (w <= rect_width && w > best_width) {
      best_annotation = txt;
      best_width = w;
    }
  }

  if (best_annotation.isEmpty())
    best_annotation = ann_list.back();

  return best_annotation;
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

  // Pick the best-fitting annotation text for this rect width. The width
  // computation is a View-layer concern (uses QFontMetrics); the Core
  // Annotation class is now a pure data class.
  const QString best_annotation =
      best_annotation_text(a, rect.width(), p.fontMetrics());

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
  // Start tracking decode duration on first call
  if (!_decode_elapsed_timer.isValid())
    _decode_elapsed_timer.start();

  // Throttle ONLY the (relatively expensive) layout/progress rebuild, NOT the
  // viewport repaint. Repaint is decoupled and coalesced to <=60 FPS by
  // View::_delayed_view_update_timer, which is cheap because per-row drawing
  // is now O(screen width) (pixel bucket). The old code gated the WHOLE update
  // — including the repaint request — behind a 1 FPS (1000ms) discard throttle
  // for >=3 decoders; that made decode growth appear as uneven "stair-steps"
  // (a big jump every ~1-2s) and dropped in-flight updates. With repaint
  // decoupled no frame is lost and growth is smooth.
  //
  // Layout/progress refresh cadence: 100ms is plenty now that a single repaint
  // costs <5ms even with millions of annotations.
  const qint64 throttle_ms = 100;

  qint64 elapsed = _update_timer.isValid() ? _update_timer.elapsed() : 999999;
  // P0 throttle fix: gate on elapsed time ONLY, not on is_running. The old
  // `!(is_running && elapsed < throttle_ms)` short-circuited to true once
  // the stack stopped (is_running==false), so after decode finished the
  // 100ms throttle was bypassed entirely and every lingering notification
  // (incl. from the final publish window) forced a full repaint — the W12
  // window showed ~494 fps with PUBLISH=0/s. Stopped-state notifications
  // are now throttled to the same 100ms cadence as running ones.
  if (elapsed >= throttle_ms) {
    _update_timer.start();

    // 1. Update progress
    decoded_progress(_decoder_stack->get_progress());

    // 2. Trigger geometry layout updates if height changed
    const int expectedHeight = rows_size() * _view->get_signalHeight();
    if (_totalHeight != expectedHeight) {
      _view->signals_changed(nullptr);
    }

    // 3. Lightweight, coalesced viewport repaint (never dropped). Throttled
    //    together with the layout/progress pass to ~10 FPS during decode
    //    growth. Firing this on every new_decode_data (effectively up to 60
    //    FPS after the merge change) multiplied the per-frame viewport cost
    //    (SignalPixmapPass rebuild + get_visible_range work) by ~6x and made
    //    decode growth stutter badly. 10 FPS is smooth enough for a
    //    progress-driven view while leaving the main thread free for decoding.
    //    Do NOT call data_updated() — it rebuilds headers/margins/scrollbars
    //    and marks the whole pixmap cache dirty; decode changes only affect
    //    the decode trace rendering.
    if (_view && _data_source->is_stopped_status()) {
      // P2: decode-only repaint — skips the signal-pixmap rebuild (signals
      // are unchanged during decode growth; the decode layer is drawn by
      // DecodeTracePass outside the cached pixmap).
      _view->request_decode_only_update();
    }
  }
}

int DecodeTrace::get_progress() { return _decoder_stack->get_progress(); }

void DecodeTrace::on_decode_done() {
  // Reset decode duration timer so next decode session starts fresh.
  _decode_elapsed_timer.invalidate();

  // Emit the final progress (100%). The viewport repaint below goes through
  // the coalesced delayed-update timer (not a direct viewport_update), so the
  // final frame flushes within <=16ms — progress is never left stuck, and
  // bursts of simultaneous decoder completions no longer flood the main
  // thread with back-to-back repaints.
  decoded_progress(_decoder_stack->get_progress());

  // Recalculate the full signal layout only when the decoder trace height
  // actually changed.  TDM/PWM Repeat normally keeps the same channel/row
  // layout, so doing signals_changed() every frame needlessly rebuilds
  // groups, margins, scrollbars and calls data_updated() again.
  if (_view) {
    const int expectedHeight = rows_size() * _view->get_signalHeight();
    if (_totalHeight != expectedHeight)
      _view->signals_changed(nullptr);
  }

  // Coalesced final repaint (<=60 FPS): avoids completion-burst stutter.
  if (_view && _data_source->is_stopped_status()) {
    _view->request_delayed_update();
  }

  _data_source->decode_done();
}

void DecodeTrace::on_error_message_changed(const QString &msg) {
  // P0-A: When the decoder stack reports an error, trigger a viewport repaint
  // so that draw_error() is called to display the error message on the trace.
  // Empty messages clear any previously shown error.
  if (_view && _data_source->is_stopped_status()) {
    _view->viewport_update();
  }
  if (!msg.isEmpty()) {
    pxv_err("DecodeTrace: decoder error: %s", msg.toStdString().c_str());
  }
}

// TDM/PWM analog port helper methods

int DecodeTrace::analog_channel_count() const {
  if (!_decoder_stack || !_decoder_stack->analog_visible())
    return 0;
  int count = 0;
  for (const auto &ch : _decoder_stack->analog_data_copy())
    if (ch && ch->visible())
      ++count;
  return count;
}

bool DecodeTrace::hit_test_analog_channel(
    int viewportY, int vOffset, int &ch_index,
    std::shared_ptr<pv::data::DecoderAnalogData> &out_data) {
  ch_index = -1;
  out_data.reset();
  if (!_decoder_stack || !_decoder_stack->analog_visible())
    return false;

  std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> visible;
  for (const auto &ch : _decoder_stack->analog_data_copy())
    if (ch && ch->visible()) visible.push_back(ch);
  if (visible.empty()) return false;

  const int units = rows_size();
  const int base_h = units > 0 ? _totalHeight / units : _view->get_signalHeight();
  const int analog_h = base_h * 2;
  const int annotation_units = std::max(0, units - 2 * static_cast<int>(visible.size()));
  const int content_y = viewportY + vOffset;
  const int analog_top = get_y() - _totalHeight / 2 + annotation_units * base_h;

  for (size_t i = 0; i < visible.size(); ++i) {
    const int y0 = analog_top + static_cast<int>(i) * analog_h;
    if (content_y >= y0 && content_y < y0 + analog_h) {
      ch_index = visible[i]->channel();
      out_data = visible[i];
      return true;
    }
  }
  return false;
}

bool DecodeTrace::get_analog_channel_rect(int ch_index, int vOffset,
                                          QRectF &screen_row_rect) {
  if (!_decoder_stack) return false;
  std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> visible;
  for (const auto &ch : _decoder_stack->analog_data_copy())
    if (ch && ch->visible()) visible.push_back(ch);

  int row = -1;
  for (size_t i = 0; i < visible.size(); ++i)
    if (visible[i]->channel() == ch_index) { row = static_cast<int>(i); break; }
  if (row < 0) return false;

  const int units = rows_size();
  const int base_h = units > 0 ? _totalHeight / units : _view->get_signalHeight();
  const int analog_h = base_h * 2;
  const int annotation_units = std::max(0, units - 2 * static_cast<int>(visible.size()));
  const int row_top = get_y() - _totalHeight / 2 + annotation_units * base_h + row * analog_h;
  screen_row_rect = QRectF(0, row_top - vOffset, _view->get_view_width(), analog_h);
  return true;
}

bool DecodeTrace::get_analog_hover(
    int viewportX, int viewportY, int vOffset, uint64_t capture_sample,
    int &ch_index, pv::data::DecoderAnalogSample &sample,
    double &engineering_value, std::string &unit, QPointF &screen_point,
    QRectF &screen_row_rect) {
  std::shared_ptr<pv::data::DecoderAnalogData> ch_data;
  if (!hit_test_analog_channel(viewportY, vOffset, ch_index, ch_data) ||
      !ch_data || !ch_data->visible() ||
      !ch_data->get_sample_at(capture_sample, sample))
    return false;
  if (!get_analog_channel_rect(ch_index, vOffset, screen_row_rect))
    return false;

  const float analog_h = static_cast<float>(screen_row_rect.height());
  const float mid_y = static_cast<float>(screen_row_rect.top()) +
      ch_data->v_offset() * analog_h * 0.5f;
  const float base_h = analog_h * 0.45f;
  float point_y = mid_y;
  const float v_scale = ch_data->v_scale();
  if (v_scale < 0.001f) {
    const uint64_t vis_start = _view->pixel2index(0);
    const uint64_t vis_end = _view->pixel2index(_view->get_view_width());
    std::vector<pv::data::DecoderAnalogSample> tmp;
    float min_val = -1.0f, max_val = 1.0f;
    ch_data->copy_samples_for_render(std::min(vis_start, vis_end),
                                     std::max(vis_start, vis_end),
                                     1024, tmp, min_val, max_val);
    const float center = (min_val + max_val) * 0.5f;
    const float range = max_val - min_val;
    const float factor = range > 0.001f ? (base_h * 1.8f / range) : base_h;
    point_y -= (sample.value - center) * factor;
  } else {
    point_y -= sample.value * base_h * v_scale;
  }

  engineering_value = ch_data->engineering_value(sample.value);
  unit = ch_data->engineering_unit();
  screen_point = QPointF(viewportX, point_y);
  return true;
}

void DecodeTrace::set_indicator_heading_row(int row) {
  _indicator_heading_row = row;
}

void DecodeTrace::set_indicator_analog_channel(int ch_index) {
  std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> visible;
  for (const auto &ch : _decoder_stack->analog_data_copy())
    if (ch && ch->visible()) visible.push_back(ch);
  int ai = -1;
  for (size_t i = 0; i < visible.size(); ++i) {
    if (visible[i]->channel() == ch_index) { ai = static_cast<int>(i); break; }
  }
  if (ai < 0) { _indicator_heading_row = -1; return; }
  const int ann_rows = std::max(
      0, static_cast<int>(_cur_row_headings.size()) - static_cast<int>(visible.size()));
  _indicator_heading_row = ann_rows + ai;
}

bool DecodeTrace::hit_test_indicator_auto_fit(
    int viewportX, int viewportY, int vOffset, int &out_ch_index,
    std::shared_ptr<pv::data::DecoderAnalogData> &out_data) {
  out_ch_index = -1;
  out_data.reset();
  if (_indicator_heading_row < 0 || !_indicator_button_rect.isValid())
    return false;

  std::vector<std::shared_ptr<pv::data::DecoderAnalogData>> visible;
  for (const auto &ch : _decoder_stack->analog_data_copy())
    if (ch && ch->visible()) visible.push_back(ch);
  const int ann_rows = std::max(
      0, static_cast<int>(_cur_row_headings.size()) - static_cast<int>(visible.size()));
  const int ai = _indicator_heading_row - ann_rows;
  if (ai < 0 || ai >= static_cast<int>(visible.size()))
    return false;

  const QPointF pos(viewportX, viewportY + vOffset);
  if (!_indicator_button_rect.contains(pos))
    return false;
  out_data = visible[ai];
  out_ch_index = out_data ? out_data->channel() : -1;
  return out_data != nullptr;
}

void DecodeTrace::auto_fit_visible_analog(
    const std::shared_ptr<pv::data::DecoderAnalogData> &ch_data,
    uint64_t vis_start_sample, uint64_t vis_end_sample) {
  if (!ch_data) return;
  auto view = ch_data->read_samples();
  const auto &samples = view.samples();
  if (samples.empty()) return;

  size_t lo = 0, hi = samples.size();
  while (lo < hi) {
    const size_t mid = lo + (hi-lo)/2;
    if (samples[mid].end_sample < vis_start_sample) lo = mid + 1;
    else hi = mid;
  }
  float mn = 0.0f, mx = 0.0f;
  bool found = false;
  for (size_t i = lo; i < samples.size(); ++i) {
    if (samples[i].start_sample > vis_end_sample) break;
    const float v = samples[i].value;
    if (!found) { mn = mx = v; found = true; }
    else { mn = std::min(mn, v); mx = std::max(mx, v); }
  }
  if (!found) return;
  const float range = mx - mn;
  if (range < 0.0001f) {
    ch_data->set_v_scale(1.0f);
    ch_data->set_v_offset(1.0f);
    return;
  }
  float vs = 2.0f / range;
  vs = std::min(100.0f, std::max(0.05f, vs));
  ch_data->set_v_scale(vs);
  float vo = 1.0f + 0.9f * ((mn + mx) * 0.5f) * vs;
  vo = std::min(3.0f, std::max(-3.0f, vo));
  ch_data->set_v_offset(vo);
}

void DecodeTrace::sync_analog_display_options(
    const std::shared_ptr<pv::data::DecoderAnalogData> &ch_data,
    bool sync_vpos, bool sync_vzoom) {
  if (!ch_data || (!sync_vpos && !sync_vzoom)) return;
  const int channel = ch_data->channel();
  if (channel < 0) return;

  const std::string vpos_key = "ch" + std::to_string(channel) + "_vpos";
  const std::string vzoom_key = "ch" + std::to_string(channel) + "_vzoom";

  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    const srd_decoder *definition = dec ? dec->decoder() : nullptr;
    if (!definition || !definition->id) continue;
    const bool target = std::strstr(definition->id, "tdm_audio") != nullptr ||
                        std::strstr(definition->id, "pwm_waveform") != nullptr;
    if (!target) continue;

    bool has_vpos = false, has_vzoom = false;
    for (GSList *l = definition->options; l; l = l->next) {
      const auto *option = static_cast<const srd_decoder_option *>(l->data);
      if (!option || !option->id) continue;
      has_vpos = has_vpos || vpos_key == option->id;
      has_vzoom = has_vzoom || vzoom_key == option->id;
    }
    if (sync_vpos && has_vpos)
      dec->set_option(vpos_key.c_str(), g_variant_new_double(ch_data->v_offset()));
    if (sync_vzoom && has_vzoom)
      dec->set_option(vzoom_key.c_str(), g_variant_new_double(ch_data->v_scale()));
  }
}

int DecodeTrace::rows_size() {
using pv::data::decode::Decoder;
int size = 0;

// Scheme A: count rows from the immutable published snapshot (lock-free).
const auto snap = _decoder_stack->published_snapshot();
for (auto &up : _decoder_stack->stack()) {
auto dec = up.get();
if (dec->shown()) {
if (snap) {
for (const auto &i : *snap) {
const pv::data::decode::Row &_row = i.first;
const auto &srow = i.second;
if (_row.decoder() == dec->decoder() && srow.gshow &&
srow.data && !srow.data->empty())
size++;
}
}
} else {
size++;
}
}
// analog rows use two standard height units.
size += 2 * analog_channel_count();

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

void *DecodeTrace::get_key_handel() { return _decoder_stack->get_key_handel(); }

// to show decoder's property setting dialog
bool DecodeTrace::create_popup(bool isnew, QPoint anchor) {
  (void)isnew;

  int ret = false; // setting have changed flag
  bool bOpenDlg = true;

  pxv_info("DecodeTrace: enter create_popup");
  while (bOpenDlg) {
    bOpenDlg = false;
    QWidget *top = _view ? _view->window() : nullptr;
    pxv_info("DecodeTrace: GetTopWindow returned %p", top);
    dialogs::DecoderOptionsDlg dlg(top);
    dlg.set_cursor_range(_decode_cursor1, _decode_cursor2);
    dlg.load_options(this);

    // 锚点定位(与毛刺滤波浮窗相同的弹出逻辑):若调用方提供了有效锚点,
    // 在 exec() 前移动对话框,避免 QDialog 默认居中。
    if (!anchor.isNull())
      dlg.move(anchor);

    pxv_info("DecodeTrace: before dlg.exec()");
    int dlg_ret = dlg.exec();
    pxv_info("DecodeTrace: after dlg.exec(), ret=%d (Accepted=%d)", dlg_ret,
             QDialog::Accepted);

    if (QDialog::Accepted == dlg_ret) {
      dlg.apply_setting();

  for (auto &up : _decoder_stack->stack()) {
    auto dec = up.get();
    if (dec->commit() || _decoder_stack->options_changed()) {
          _decoder_stack->set_options_changed(true);
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

  pxv_info("DecodeTrace: exit create_popup, returning %d", ret);
  return ret;
}

} // namespace view
} // namespace pv
