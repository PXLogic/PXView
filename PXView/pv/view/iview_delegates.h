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

#ifndef PXVIEW_PV_VIEW_IVIEW_DELEGATES_H
#define PXVIEW_PV_VIEW_IVIEW_DELEGATES_H

#include <cstdint>
#include <vector>

class QColor;
class QString;

namespace pv {
namespace view {

/**
 * @brief Read-only interface to ViewLayout state.
 *
 * Phase 8 (Testability): extracted so that ViewCursors, ViewSignalSync,
 * ViewDerivedTraces, ViewDataSync, ViewportPainter, and RenderPasses can
 * depend on this abstract interface instead of the concrete ViewLayout /
 * View classes. In unit tests, a mock implementation (MockViewLayout)
 * can be substituted — eliminating the need for a real QWidget-based View.
 *
 * ViewLayout already implements all these methods; the interface simply
 * formalizes the contract. Delegates that only need layout state should
 * accept IViewLayout* instead of View*.
 */
class IViewLayout {
public:
  virtual ~IViewLayout() = default;

  // -- Scale / offset state (read) --
  virtual double scale() const = 0;
  virtual int64_t offset() const = 0;
  virtual double maxscale() const = 0;
  virtual double minscale() const = 0;

  // -- Signal height state (read) --
  virtual int spanY() const = 0;
  virtual int signalHeight() const = 0;
  virtual int signalHeightScale() const = 0;

  // -- DSO zoom state (read) --
  virtual double dso_zoom_factor() const = 0;

  // -- Scale / offset mutation --
  virtual void set_scale_offset(double scale, int64_t offset) = 0;

  // -- Offset bounds (read) --
  virtual int64_t get_max_offset() = 0;
  virtual int64_t get_min_offset() = 0;

  // -- Scroll layout (read) --
  virtual void get_scroll_layout(int64_t &length, int64_t &offset) = 0;
};

/**
 * @brief Read-only interface to ViewCursors state.
 *
 * Phase 8 (Testability): allows other delegates and rendering code
 * to query cursor state without depending on the concrete ViewCursors
 * class.
 */
class IViewCursors {
public:
  virtual ~IViewCursors() = default;

  virtual bool cursors_shown() const = 0;
  virtual bool trig_cursor_shown() const = 0;
  virtual bool search_cursor_shown() const = 0;
  virtual bool xcursors_shown() const = 0;
};

/**
 * @brief Read-only interface to ViewSignalSync state.
 *
 * Phase 8 (Testability): allows rendering code and tests to query
 * the signal list without depending on the concrete ViewSignalSync
 * class.
 */
class IViewSignalStore {
public:
  virtual ~IViewSignalStore() = default;

  virtual size_t signal_count() const = 0;
  virtual bool rebuild_in_progress() const = 0;
};

/**
 * @brief Mock implementation of IViewLayout for unit testing.
 *
 * Phase 8 (Testability): provides a simple in-memory implementation
 * that can be used in unit tests for delegates (ViewportPainter,
 * RenderPasses, etc.) without creating a real View widget.
 *
 * Usage:
 *   MockViewLayout layout;
 *   layout.set_scale(10.0);
 *   layout.set_offset(0);
 *   // pass &layout to delegate under test
 */
class MockViewLayout : public IViewLayout {
public:
  MockViewLayout() = default;

  // -- Test setters --
  void set_scale(double s) { _scale = s; }
  void set_offset(int64_t o) { _offset = o; }
  void set_maxscale(double s) { _maxscale = s; }
  void set_minscale(double s) { _minscale = s; }
  void set_spanY(int s) { _spanY = s; }
  void set_signalHeight(int h) { _signalHeight = h; }
  void set_signalHeightScale(int s) { _signalHeightScale = s; }
  void set_dso_zoom_factor(double f) { _dso_zoom_factor = f; }
  void set_max_offset(int64_t v) { _max_offset = v; }
  void set_min_offset(int64_t v) { _min_offset = v; }

  // -- IViewLayout overrides --
  double scale() const override { return _scale; }
  int64_t offset() const override { return _offset; }
  double maxscale() const override { return _maxscale; }
  double minscale() const override { return _minscale; }
  int spanY() const override { return _spanY; }
  int signalHeight() const override { return _signalHeight; }
  int signalHeightScale() const override { return _signalHeightScale; }
  double dso_zoom_factor() const override { return _dso_zoom_factor; }
  void set_scale_offset(double scale, int64_t offset) override {
    _scale = scale;
    _offset = offset;
  }
  int64_t get_max_offset() override { return _max_offset; }
  int64_t get_min_offset() override { return _min_offset; }
  void get_scroll_layout(int64_t &length, int64_t &offset) override {
    length = _scroll_length;
    offset = _offset;
  }

  void set_scroll_length(int64_t l) { _scroll_length = l; }

private:
  double _scale = 10.0;
  int64_t _offset = 0;
  double _maxscale = 1e9;
  double _minscale = 1e-15;
  int _spanY = 0;
  int _signalHeight = 0;
  int _signalHeightScale = 24;
  double _dso_zoom_factor = 1.0;
  int64_t _max_offset = 0;
  int64_t _min_offset = 0;
  int64_t _scroll_length = 0;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_IVIEW_DELEGATES_H
