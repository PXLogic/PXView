/*
 * test_view_layout.cpp — Unit tests for MockViewLayout / IViewLayout
 *
 * Verifies that MockViewLayout correctly implements the IViewLayout interface
 * contract: default values, getter/setter round-trips, set_scale_offset
 * atomicity, scroll layout, and offset bounds.
 *
 * MockViewLayout is a header-only class in pv/view/iview_delegates.h.
 * No Qt linking required.
 */

#include <gtest/gtest.h>

#include "pv/view/iview_delegates.h"

using pv::view::IViewLayout;
using pv::view::MockViewLayout;

// ---- Default values ----

TEST(MockViewLayout, DefaultScale) {
    MockViewLayout layout;
    EXPECT_DOUBLE_EQ(layout.scale(), 10.0);
}

TEST(MockViewLayout, DefaultOffset) {
    MockViewLayout layout;
    EXPECT_EQ(layout.offset(), 0);
}

TEST(MockViewLayout, DefaultMaxScale) {
    MockViewLayout layout;
    EXPECT_DOUBLE_EQ(layout.maxscale(), 1e9);
}

TEST(MockViewLayout, DefaultMinScale) {
    MockViewLayout layout;
    EXPECT_DOUBLE_EQ(layout.minscale(), 1e-15);
}

TEST(MockViewLayout, DefaultDsoZoomFactor) {
    MockViewLayout layout;
    EXPECT_DOUBLE_EQ(layout.dso_zoom_factor(), 1.0);
}

TEST(MockViewLayout, DefaultSignalHeightScale) {
    MockViewLayout layout;
    EXPECT_EQ(layout.signalHeightScale(), 24);
}

TEST(MockViewLayout, DefaultSpanY) {
    MockViewLayout layout;
    EXPECT_EQ(layout.spanY(), 0);
}

TEST(MockViewLayout, DefaultSignalHeight) {
    MockViewLayout layout;
    EXPECT_EQ(layout.signalHeight(), 0);
}

// ---- Setter / Getter round-trips ----

TEST(MockViewLayout, SetScale) {
    MockViewLayout layout;
    layout.set_scale(0.001);
    EXPECT_DOUBLE_EQ(layout.scale(), 0.001);
}

TEST(MockViewLayout, SetOffset) {
    MockViewLayout layout;
    layout.set_offset(12345);
    EXPECT_EQ(layout.offset(), 12345);
}

TEST(MockViewLayout, SetMaxScale) {
    MockViewLayout layout;
    layout.set_maxscale(100.0);
    EXPECT_DOUBLE_EQ(layout.maxscale(), 100.0);
}

TEST(MockViewLayout, SetMinScale) {
    MockViewLayout layout;
    layout.set_minscale(0.0001);
    EXPECT_DOUBLE_EQ(layout.minscale(), 0.0001);
}

TEST(MockViewLayout, SetDsoZoomFactor) {
    MockViewLayout layout;
    layout.set_dso_zoom_factor(2.5);
    EXPECT_DOUBLE_EQ(layout.dso_zoom_factor(), 2.5);
}

TEST(MockViewLayout, SetSignalHeight) {
    MockViewLayout layout;
    layout.set_signalHeight(48);
    EXPECT_EQ(layout.signalHeight(), 48);
}

TEST(MockViewLayout, SetSignalHeightScale) {
    MockViewLayout layout;
    layout.set_signalHeightScale(30);
    EXPECT_EQ(layout.signalHeightScale(), 30);
}

TEST(MockViewLayout, SetSpanY) {
    MockViewLayout layout;
    layout.set_spanY(60);
    EXPECT_EQ(layout.spanY(), 60);
}

// ---- set_scale_offset atomicity ----

TEST(MockViewLayout, SetScaleOffsetUpdatesBoth) {
    MockViewLayout layout;
    layout.set_scale_offset(0.005, 999);
    EXPECT_DOUBLE_EQ(layout.scale(), 0.005);
    EXPECT_EQ(layout.offset(), 999);
}

TEST(MockViewLayout, SetScaleOffsetOverwritesPrevious) {
    MockViewLayout layout;
    layout.set_scale(1.0);
    layout.set_offset(0);
    layout.set_scale_offset(0.002, 500);
    EXPECT_DOUBLE_EQ(layout.scale(), 0.002);
    EXPECT_EQ(layout.offset(), 500);
}

TEST(MockViewLayout, SetScaleOffsetNegativeOffset) {
    MockViewLayout layout;
    layout.set_scale_offset(1.0, -100);
    EXPECT_EQ(layout.offset(), -100);
}

// ---- Offset bounds ----

TEST(MockViewLayout, SetMaxOffset) {
    MockViewLayout layout;
    layout.set_max_offset(10000);
    EXPECT_EQ(layout.get_max_offset(), 10000);
}

TEST(MockViewLayout, SetMinOffset) {
    MockViewLayout layout;
    layout.set_min_offset(-100);
    EXPECT_EQ(layout.get_min_offset(), -100);
}

TEST(MockViewLayout, DefaultMaxOffset) {
    MockViewLayout layout;
    EXPECT_EQ(layout.get_max_offset(), 0);
}

TEST(MockViewLayout, DefaultMinOffset) {
    MockViewLayout layout;
    EXPECT_EQ(layout.get_min_offset(), 0);
}

// ---- Scroll layout ----

TEST(MockViewLayout, GetScrollLayoutReturnsCurrentOffset) {
    MockViewLayout layout;
    layout.set_offset(42);
    int64_t length = -1;
    int64_t offset = -1;
    layout.get_scroll_layout(length, offset);
    EXPECT_EQ(offset, 42);
}

TEST(MockViewLayout, SetScrollLength) {
    MockViewLayout layout;
    layout.set_scroll_length(5000);
    int64_t length = 0;
    int64_t offset = 0;
    layout.get_scroll_layout(length, offset);
    EXPECT_EQ(length, 5000);
}

TEST(MockViewLayout, GetScrollLayoutDefaultLength) {
    MockViewLayout layout;
    int64_t length = -1;
    int64_t offset = -1;
    layout.get_scroll_layout(length, offset);
    EXPECT_EQ(length, 0);
    EXPECT_EQ(offset, 0);
}

// ---- Polymorphic usage (IViewLayout* pointer) ----

TEST(MockViewLayout, UsableThroughIViewLayoutPointer) {
    MockViewLayout mock;
    IViewLayout *iface = &mock;

    iface->set_scale_offset(0.01, 200);
    EXPECT_DOUBLE_EQ(iface->scale(), 0.01);
    EXPECT_EQ(iface->offset(), 200);
    EXPECT_DOUBLE_EQ(iface->maxscale(), 1e9);
    EXPECT_DOUBLE_EQ(iface->minscale(), 1e-15);
    EXPECT_EQ(iface->spanY(), 0);
    EXPECT_EQ(iface->signalHeight(), 0);
    EXPECT_EQ(iface->signalHeightScale(), 24);
    EXPECT_DOUBLE_EQ(iface->dso_zoom_factor(), 1.0);
    EXPECT_EQ(iface->get_max_offset(), 0);
    EXPECT_EQ(iface->get_min_offset(), 0);

    int64_t length = 0, offset = 0;
    iface->get_scroll_layout(length, offset);
    EXPECT_EQ(offset, 200);
}

// ---- Independence of instances ----

TEST(MockViewLayout, InstancesAreIndependent) {
    MockViewLayout a;
    MockViewLayout b;

    a.set_scale(0.1);
    b.set_scale(50.0);

    EXPECT_DOUBLE_EQ(a.scale(), 0.1);
    EXPECT_DOUBLE_EQ(b.scale(), 50.0);
}
