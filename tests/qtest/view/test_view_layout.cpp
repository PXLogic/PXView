/*
 * test_view_layout.cpp — QTest unit tests for MockViewLayout / IViewLayout
 *
 * Migrated from GTest to QTest (P0).
 */

#include <QtTest>
#include "pv/view/iview_delegates.h"

using pv::view::IViewLayout;
using pv::view::MockViewLayout;

class TestViewLayout : public QObject {
    Q_OBJECT
private slots:
    void DefaultScale();
    void DefaultOffset();
    void DefaultMaxScale();
    void DefaultMinScale();
    void DefaultDsoZoomFactor();
    void DefaultSignalHeightScale();
    void DefaultSpanY();
    void DefaultSignalHeight();
    void SetScale();
    void SetOffset();
    void SetMaxScale();
    void SetMinScale();
    void SetDsoZoomFactor();
    void SetSignalHeight();
    void SetSignalHeightScale();
    void SetSpanY();
    void SetScaleOffsetUpdatesBoth();
    void SetScaleOffsetOverwritesPrevious();
    void SetScaleOffsetNegativeOffset();
    void SetMaxOffset();
    void SetMinOffset();
    void DefaultMaxOffset();
    void DefaultMinOffset();
    void GetScrollLayoutReturnsCurrentOffset();
    void SetScrollLength();
    void GetScrollLayoutDefaultLength();
    void UsableThroughIViewLayoutPointer();
    void InstancesAreIndependent();
};

void TestViewLayout::DefaultScale() { MockViewLayout l; QCOMPARE(l.scale(), 10.0); }
void TestViewLayout::DefaultOffset() { MockViewLayout l; QCOMPARE(l.offset(), 0); }
void TestViewLayout::DefaultMaxScale() { MockViewLayout l; QCOMPARE(l.maxscale(), 1e9); }
void TestViewLayout::DefaultMinScale() { MockViewLayout l; QCOMPARE(l.minscale(), 1e-15); }
void TestViewLayout::DefaultDsoZoomFactor() { MockViewLayout l; QCOMPARE(l.dso_zoom_factor(), 1.0); }
void TestViewLayout::DefaultSignalHeightScale() { MockViewLayout l; QCOMPARE(l.signalHeightScale(), 24); }
void TestViewLayout::DefaultSpanY() { MockViewLayout l; QCOMPARE(l.spanY(), 0); }
void TestViewLayout::DefaultSignalHeight() { MockViewLayout l; QCOMPARE(l.signalHeight(), 0); }

void TestViewLayout::SetScale() { MockViewLayout l; l.set_scale(0.001); QCOMPARE(l.scale(), 0.001); }
void TestViewLayout::SetOffset() { MockViewLayout l; l.set_offset(12345); QCOMPARE(l.offset(), 12345); }
void TestViewLayout::SetMaxScale() { MockViewLayout l; l.set_maxscale(100.0); QCOMPARE(l.maxscale(), 100.0); }
void TestViewLayout::SetMinScale() { MockViewLayout l; l.set_minscale(0.0001); QCOMPARE(l.minscale(), 0.0001); }
void TestViewLayout::SetDsoZoomFactor() { MockViewLayout l; l.set_dso_zoom_factor(2.5); QCOMPARE(l.dso_zoom_factor(), 2.5); }
void TestViewLayout::SetSignalHeight() { MockViewLayout l; l.set_signalHeight(48); QCOMPARE(l.signalHeight(), 48); }
void TestViewLayout::SetSignalHeightScale() { MockViewLayout l; l.set_signalHeightScale(30); QCOMPARE(l.signalHeightScale(), 30); }
void TestViewLayout::SetSpanY() { MockViewLayout l; l.set_spanY(60); QCOMPARE(l.spanY(), 60); }

void TestViewLayout::SetScaleOffsetUpdatesBoth() {
    MockViewLayout l; l.set_scale_offset(0.005, 999);
    QCOMPARE(l.scale(), 0.005); QCOMPARE(l.offset(), 999);
}
void TestViewLayout::SetScaleOffsetOverwritesPrevious() {
    MockViewLayout l; l.set_scale(1.0); l.set_offset(0);
    l.set_scale_offset(0.002, 500);
    QCOMPARE(l.scale(), 0.002); QCOMPARE(l.offset(), 500);
}
void TestViewLayout::SetScaleOffsetNegativeOffset() {
    MockViewLayout l; l.set_scale_offset(1.0, -100); QCOMPARE(l.offset(), -100);
}
void TestViewLayout::SetMaxOffset() { MockViewLayout l; l.set_max_offset(10000); QCOMPARE(l.get_max_offset(), 10000); }
void TestViewLayout::SetMinOffset() { MockViewLayout l; l.set_min_offset(-100); QCOMPARE(l.get_min_offset(), -100); }
void TestViewLayout::DefaultMaxOffset() { MockViewLayout l; QCOMPARE(l.get_max_offset(), 0); }
void TestViewLayout::DefaultMinOffset() { MockViewLayout l; QCOMPARE(l.get_min_offset(), 0); }

void TestViewLayout::GetScrollLayoutReturnsCurrentOffset() {
    MockViewLayout l; l.set_offset(42);
    int64_t len = -1, off = -1;
    l.get_scroll_layout(len, off);
    QCOMPARE(off, 42);
}
void TestViewLayout::SetScrollLength() {
    MockViewLayout l; l.set_scroll_length(5000);
    int64_t len = 0, off = 0;
    l.get_scroll_layout(len, off);
    QCOMPARE(len, 5000);
}
void TestViewLayout::GetScrollLayoutDefaultLength() {
    MockViewLayout l;
    int64_t len = -1, off = -1;
    l.get_scroll_layout(len, off);
    QCOMPARE(len, 0); QCOMPARE(off, 0);
}
void TestViewLayout::UsableThroughIViewLayoutPointer() {
    MockViewLayout mock;
    IViewLayout *iface = &mock;
    iface->set_scale_offset(0.01, 200);
    QCOMPARE(iface->scale(), 0.01);
    QCOMPARE(iface->offset(), 200);
    QCOMPARE(iface->maxscale(), 1e9);
    QCOMPARE(iface->minscale(), 1e-15);
    QCOMPARE(iface->spanY(), 0);
    QCOMPARE(iface->signalHeight(), 0);
    QCOMPARE(iface->signalHeightScale(), 24);
    QCOMPARE(iface->dso_zoom_factor(), 1.0);
    QCOMPARE(iface->get_max_offset(), 0);
    QCOMPARE(iface->get_min_offset(), 0);
    int64_t len = 0, off = 0;
    iface->get_scroll_layout(len, off);
    QCOMPARE(off, 200);
}
void TestViewLayout::InstancesAreIndependent() {
    MockViewLayout a, b;
    a.set_scale(0.1); b.set_scale(50.0);
    QCOMPARE(a.scale(), 0.1);
    QCOMPARE(b.scale(), 50.0);
}

QTEST_MAIN(TestViewLayout)
#include "test_view_layout.moc"
