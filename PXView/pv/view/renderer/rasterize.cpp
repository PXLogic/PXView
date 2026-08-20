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

#include "pv/view/renderer/rasterize.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <QLine>
#include <QPen>
#include <QPointF>
#include <QRectF>
#include <QVector>

#include "pv/base/log.h"
#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/snapshot/logicsnapshot.h"

using namespace std;

namespace pv {
namespace view {

// Mirror of LogicSignal::TogMaxScale (logic edge-density compression factor).
// Kept local so this pure function has no dependency on the Signal class.
static constexpr uint16_t kRasterizeTogMaxScale = 10;

void rasterize_logic_channel(
    QPainter &p, data::LogicSnapshot *snapshot, int channel_index,
    int left, int right, int y, int total_height, const QColor &colour,
    double scale, int64_t offset, uint64_t end_align_sample,
    const PaintContext &ctx, const std::vector<GlitchRange> *preview_ranges) {
  // Verbatim extraction of LogicSignal::paint_mid_align (no arithmetic
  // changes — pixel parity required). Members replaced by parameters;
  // _cur_pulses/_cur_edges replaced by local buffers.
  if (!snapshot)
    return;
  assert(right >= left);

  if (scale <= 0)
    return;

  const int high_offset = y - total_height;
  const int low_offset = y;

  double samplerate = snapshot ? snapshot->samplerate() : 0;
  if (!snapshot || snapshot->empty() || samplerate == 0) {
    pxv_warn(
        "rasterize_logic_channel: no data or samplerate==0, skipping paint");
    return;
  }

  if (!snapshot->has_data(channel_index)) {
    pxv_warn("rasterize_logic_channel: has_data false for index=%d, skipping "
             "paint",
             channel_index);
    return;
  }

  uint64_t ring_cnt = snapshot->get_ring_sample_count();
  if (ring_cnt == 0) {
    pxv_warn("rasterize_logic_channel: ring_sample_count==0, skipping paint");
    return;
  }
  if (end_align_sample >= ring_cnt)
    end_align_sample = ring_cnt - 1;

  const int64_t last_sample = end_align_sample;
  const double samples_per_pixel = samplerate * scale;

  uint16_t width = right - left;
  const double start = offset * samples_per_pixel;
  const double end = (offset + width + 1) * samples_per_pixel;
  const uint64_t end_index =
      min(max((int64_t)floor(end), (int64_t)0), last_sample);
  const uint64_t start_index = max((uint64_t)floor(start), (uint64_t)0);

  if (start_index > end_index)
    return;

  width =
      min(width, (uint16_t)ceil((end_index + 1) / samples_per_pixel - offset));
  const uint16_t max_togs = width / kRasterizeTogMaxScale;

  // Local buffers (were LogicSignal::_cur_pulses/_cur_edges members).
  std::vector<std::pair<bool, bool>> cur_pulses;
  std::vector<std::pair<uint16_t, bool>> cur_edges;
  const bool first_sample = snapshot->get_display_edges(
      cur_pulses, cur_edges, start_index, end_index, width, max_togs, offset,
      samples_per_pixel, channel_index);
  assert(cur_pulses.size() >= width);

  int preX = 0;
  int preY = first_sample ? high_offset : low_offset;
  int x = preX;
  std::vector<QLine> wave_lines;

  if (cur_edges.size() < max_togs) {
    std::vector<std::pair<uint16_t, bool>>::const_iterator i;
    for (i = cur_edges.begin() + 1; i != cur_edges.end() - 1; i++) {
      x = (*i).first;
      wave_lines.push_back(QLine(preX, preY, x, preY));
      wave_lines.push_back(QLine(x, high_offset, x, low_offset));
      preX = x;
      preY = (*i).second ? high_offset : low_offset;
    }
    x = (*i).first;
    wave_lines.push_back(QLine(preX, preY, x, preY));
  } else if (cur_pulses.size() > 0) {
    std::vector<std::pair<bool, bool>>::const_iterator i = cur_pulses.begin();
    while (i != cur_pulses.end() - 1) {
      if ((*i).first) {
        wave_lines.push_back(QLine(preX, preY, x, preY));
        wave_lines.push_back(QLine(x, high_offset, x, low_offset));
        preX = x;
        preY = (*i).second ? high_offset : low_offset;
      }
      x++;
      i++;
    }
    wave_lines.push_back(QLine(preX, preY, x, preY));
  }

  // Original: p.setPen(_colour.isValid() ? _colour : fore). The caller passes
  // the FINAL colour (already computed as _colour.isValid() ? _colour : fore).
  p.setPen(colour);
  p.drawLines(wave_lines.data(), wave_lines.size());

  // === Glitch filter overlay (extracted from paint_mid_align Task 8) ===
  // Coordinate mapping: pixel x = sample_index / samples_per_pixel - offset
  // (offset is the pixel offset of the left edge; matches the edge/pulse
  // pixel coordinates produced by get_display_edges above).
  const int sig_idx = channel_index;

  // (1) Already-filtered ranges (red overlay). Drawn when the snapshot has
  //     been glitch-filtered AND the user has enabled the overlay. Read
  //     directly from the snapshot (thread-safe); takes precedence over the
  //     live preview.
  if (snapshot && snapshot->is_glitch_filtered() &&
      ctx.show_glitch_overlay) {
    const auto &ranges = snapshot->get_filtered_ranges(sig_idx);
    if (!ranges.empty()) {
      p.setBrush(QColor(255, 82, 82, 90));
      p.setPen(Qt::NoPen);
      for (const auto &r : ranges) {
        if (r.end < start_index || r.start > end_index)
          continue; // off-screen cull
        int x1 = (int)(r.start / samples_per_pixel - offset);
        int x2 = (int)(r.end / samples_per_pixel - offset);
        if (x2 <= x1)
          x2 = x1 + 1;
        p.drawRect(x1, high_offset, x2 - x1, low_offset - high_offset);
      }
    }
  }
  // (2) Live preview ranges (orange overlay). Only shown while the popup is
  //     open and the snapshot is not yet actually filtered. Ranges are passed
  //     in (previously read from View::get_preview_ranges on the GUI thread).
  else if (preview_ranges && !preview_ranges->empty()) {
    p.setBrush(QColor(255, 183, 77, 70));
    p.setPen(Qt::NoPen);
    for (const auto &pulse : *preview_ranges) {
      if (pulse.end < start_index || pulse.start > end_index)
        continue;
      int x1 = (int)(pulse.start / samples_per_pixel - offset);
      int x2 = (int)(pulse.end / samples_per_pixel - offset);
      if (x2 <= x1)
        x2 = x1 + 1;
      p.drawRect(x1, high_offset, x2 - x1, low_offset - high_offset);
    }
  }
}

// Mirror of View::ForeAlpha (kept local so this pure function has no
// dependency on the View class).
static constexpr int kRasterizeForeAlpha = 200;

void rasterize_dso_channel(
    QPainter &p, data::DsoSnapshot *snapshot, int zeroY, int left, int right,
    int64_t start, int64_t end, int hw_offset, double samples_per_pixel,
    int channel_index, float top, float bottom, float scale,
    const QColor &colour) {
  // Verbatim extraction of DsoSignal::paint_per_pixel (no arithmetic
  // changes — pixel parity required). Members replaced by parameters; the
  // debug s_dso_timing writes (never read) are dropped; static thread_local
  // scratch buffers are retained (per-thread, no cross-thread sharing).
  const int width = right - left;
  if (width <= 0 || end <= start)
    return;

  const uint8_t *const samples_buffer =
      snapshot->get_samples(start, end, channel_index);
  if (!samples_buffer)
    return;

  QColor trace_colour = colour;
  trace_colour.setAlpha(kRasterizeForeAlpha);
  p.setPen(QPen(Qt::NoPen));
  p.setBrush(trace_colour);

  static thread_local QVector<QRectF> rects;
  if (rects.size() < width)
    rects.resize(width);
  QRectF *r = rects.data();

  const double spp = samples_per_pixel;
  const double base_sample = start;

  if (spp < 1.0) {
    // ---- Polyline mode (zoomed in: spp < 1.0) ----
    static thread_local QVector<QPointF> pts;
    if (pts.size() < width)
      pts.resize(width);

    p.setPen(QPen(colour));
    p.setBrush(Qt::NoBrush);

    int pt_count = 0;
    for (int x = 0; x < width; x++) {
      double sample_pos = base_sample + x * spp;
      int64_t s0 = (int64_t)floor(sample_pos);
      double frac = sample_pos - s0;

      if (s0 < start) {
        s0 = start;
        frac = 0;
      }
      if (s0 >= end) {
        if (pt_count > 0)
          break;
        continue;
      }

      int64_t s1 = s0 + 1;
      if (s1 > end)
        s1 = end;
      uint8_t v0 = samples_buffer[s0 - start];
      uint8_t v1 =
          (s1 <= end && s1 > start) ? samples_buffer[s1 - start] : v0;
      float v = v0 + (float)(v1 - v0) * frac;
      float y = min(max(top, zeroY + (v - hw_offset) * scale), bottom);
      pts[pt_count++] = QPointF((float)(left + x), y);
    }
    p.drawPolyline(pts.data(), pt_count);
  } else {
    // ---- Min/max mode (zoomed out: spp >= 1.0) ----
    static thread_local QVector<uint8_t> min_buf, max_buf;
    if (min_buf.size() < width) {
      min_buf.resize(width);
      max_buf.resize(width);
    }

    for (int x = 0; x < width; x++) {
      int64_t s_start = (int64_t)floor(base_sample + x * spp);
      int64_t s_end = (int64_t)floor(base_sample + (x + 1) * spp);

      if (s_start < start)
        s_start = start;
      if (s_end > end)
        s_end = end;
      if (s_end <= s_start)
        s_end = s_start + 1;
      if (s_end > end)
        s_end = end;
      if (s_start >= s_end) {
        if (x > 0) {
          min_buf[x] = min_buf[x - 1];
          max_buf[x] = max_buf[x - 1];
        } else {
          min_buf[x] = 128;
          max_buf[x] = 128;
        }
        continue;
      }

      const uint8_t *psrc = samples_buffer + (s_start - start);
      const int64_t span = s_end - s_start;
      uint8_t min_v = *psrc;
      uint8_t max_v = *psrc;
      for (int64_t i = 1; i < span; i++) {
        const uint8_t v = psrc[i];
        if (v < min_v)
          min_v = v;
        if (v > max_v)
          max_v = v;
      }
      min_buf[x] = min_v;
      max_buf[x] = max_v;
    }

    for (int x = 0; x < width; x++) {
      uint8_t draw_max = max_buf[x];
      uint8_t draw_min = min_buf[x];
      if (x + 1 < width) {
        draw_max = max(draw_max, min_buf[x + 1]);
        draw_min = min(draw_min, max_buf[x + 1]);
      }

      float y_top =
          min(max(top, zeroY + (draw_min - hw_offset) * scale), bottom);
      float y_bot =
          min(max(top, zeroY + (draw_max - hw_offset) * scale), bottom);

      float h = y_bot - y_top;
      if (h >= 0.0f && h < 1.0f)
        h = 1.0f;
      else if (h <= 0.0f && h > -1.0f)
        h = -1.0f;

      r[x] = QRectF((float)(left + x), y_top, 1.0f, h);
    }
    p.drawRects(r, width);
  }
}

void rasterize_analog_channel(
    QPainter &p, data::AnalogSnapshot *snapshot, int zeroY, int left,
    int right, uint64_t start_index, int64_t sample_count,
    double samples_per_pixel, int order, float top, float bottom,
    int hw_offset, float scale, float float_scale, const QColor &colour) {
  // Verbatim extraction of AnalogSignal::paint_per_pixel (no arithmetic
  // changes — pixel parity required). Members replaced by parameters; the
  // member _rects scratch buffer is replaced by a static thread_local one.
  const int64_t channel_num = (int64_t)snapshot->get_channel_num();
  const uint8_t unit_bytes = snapshot->get_unit_bytes();
  const uint8_t *const samples = snapshot->get_samples(0);
  if (!samples || sample_count <= 0)
    return;

  const bool is_float = snapshot->is_float();
  const uint64_t sample_cnt = snapshot->get_sample_count();
  const uint64_t ring_end = snapshot->get_ring_end();
  const uint64_t data_size = sample_cnt * channel_num * unit_bytes;
  const double spp = samples_per_pixel;
  const int pixel_width = right - left;

  if (pixel_width <= 0)
    return;

  QColor trace_colour = colour;
  trace_colour.setAlpha(kRasterizeForeAlpha);
  p.setPen(QPen(Qt::NoPen));
  p.setBrush(trace_colour);

  static thread_local QVector<QRectF> rects;
  if (rects.size() < pixel_width)
    rects.resize(pixel_width + 10);
  QRectF *r = rects.data();

  // Helper: read a single sample value at ring index and map to screen Y.
  auto read_sample_y = [&](uint64_t ring_idx) -> float {
    uint64_t idx = (ring_idx * channel_num + order) * unit_bytes;
    if (idx + unit_bytes > data_size)
      return zeroY;
    float yvalue;
    if (is_float && unit_bytes == sizeof(float)) {
      yvalue = *reinterpret_cast<const float *>(samples + idx);
      yvalue = zeroY - yvalue * float_scale;
    } else {
      yvalue = samples[idx];
      for (uint8_t i = 1; i < unit_bytes; i++)
        yvalue += (samples[idx + i] << (i * 8));
      yvalue = zeroY + (yvalue - hw_offset) * scale;
    }
    return min(max(yvalue, top), bottom);
  };

  if (spp < 1.0) {
    // ---- Polyline mode (zoomed in: spp < 1.0) ----
    // Opaque pen — bypasses alpha blending entirely.
    p.setPen(QPen(colour));
    p.setBrush(Qt::NoBrush);

    static thread_local QVector<QPointF> pts;
    if (pts.size() < pixel_width)
      pts.resize(pixel_width);

    int pt_count = 0;
    for (int x = 0; x < pixel_width; x++) {
      double sample_pos = (double)x * spp;
      uint64_t s0_offset = (uint64_t)floor(sample_pos);
      double frac = sample_pos - s0_offset;

      if (s0_offset >= (uint64_t)sample_count) {
        if (pt_count > 0)
          break;
        continue;
      }

      uint64_t s0 = (start_index + s0_offset) % sample_cnt;
      uint64_t s1 = (s0 + 1) % sample_cnt;
      float y0 = read_sample_y(s0);
      float y1 = read_sample_y(s1);
      float v = y0 + (float)(y1 - y0) * frac;
      pts[pt_count++] = QPointF((float)(left + x), v);

      if (s0 == ring_end)
        break;
    }
    p.drawPolyline(pts.data(), pt_count);
  } else {
    // ---- Min/max mode (zoomed out: spp >= 1.0) ----
    static thread_local QVector<float> min_buf, max_buf;
    if (min_buf.size() < pixel_width) {
      min_buf.resize(pixel_width);
      max_buf.resize(pixel_width);
    }

    // Pass 1: compute min/max Y for each pixel column.
    for (int x = 0; x < pixel_width; x++) {
      uint64_t s_start_off = (uint64_t)floor((double)x * spp);
      uint64_t s_end_off = (uint64_t)floor((double)(x + 1) * spp);
      if (s_end_off <= s_start_off)
        s_end_off = s_start_off + 1;
      if (s_end_off > (uint64_t)sample_count)
        s_end_off = (uint64_t)sample_count;
      if (s_start_off >= (uint64_t)sample_count) {
        if (x > 0) {
          min_buf[x] = min_buf[x - 1];
          max_buf[x] = max_buf[x - 1];
        } else {
          min_buf[x] = zeroY;
          max_buf[x] = zeroY;
        }
        continue;
      }

      uint64_t s0 = (start_index + s_start_off) % sample_cnt;
      float y_min = read_sample_y(s0);
      float y_max = y_min;

      for (uint64_t i = 1; i < s_end_off - s_start_off; i++) {
        uint64_t si = (start_index + s_start_off + i) % sample_cnt;
        if (si == ring_end)
          break;
        float yv = read_sample_y(si);
        if (yv < y_min)
          y_min = yv;
        if (yv > y_max)
          y_max = yv;
      }
      min_buf[x] = y_min;
      max_buf[x] = y_max;
    }

    // Pass 2: draw rectangles with vertical overlap to adjacent pixels.
    for (int x = 0; x < pixel_width; x++) {
      float draw_max = max_buf[x];
      float draw_min = min_buf[x];
      if (x + 1 < pixel_width) {
        draw_max = max(draw_max, min_buf[x + 1]);
        draw_min = min(draw_min, max_buf[x + 1]);
      }

      float h = draw_max - draw_min;
      if (h >= 0.0f && h < 1.0f)
        h = 1.0f;
      else if (h <= 0.0f && h > -1.0f)
        h = -1.0f;

      r[x] = QRectF((float)(left + x), draw_min, 1.0f, h);
    }
    p.drawRects(r, pixel_width);
  }
}

} // namespace view
} // namespace pv
