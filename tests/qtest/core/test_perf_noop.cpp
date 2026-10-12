// Compiled WITHOUT PXV_PERF on purpose.
//
// This target exists to lock one property that the old perflog.h could not
// offer: with the compile gate OFF, every PXV_PERF_* macro expands to a no-op
// that
//   * needs no #ifdef/#else at the call site,
//   * leaves no dangling local (so no (void) dance and no
//     -Wunused-variable), and
//   * references no perf symbol at all.
//
// If someone re-introduces a macro that requires the gate to be ON, this file
// stops compiling — which is the whole point.

#include <QtTest>

// The gate must be OFF for this TU. flags.cmake uses a directory-scoped
// add_compile_definitions(), which the tests/ subdirectory inherits, so the
// define has to be dropped explicitly here rather than by "not adding it".
#ifdef PXV_PERF
#undef PXV_PERF
#endif

#include "pv/base/perf.h"

class TestPerfNoop : public QObject {
  Q_OBJECT

private slots:
  void macrosCompileAndAreInert() {
    // These locals exist ONLY to feed the macros below. In the old design each
    // of them needed a hand-written `#else (void)x;` branch.
    int count_local = 7;
    double ms_local = 1.5;
    const char *name_local = "row";

    PXV_PERF_ADD(::pv::base::perf::MetricId::AnnInRange, count_local);
    PXV_PERF_GAUGE(::pv::base::perf::MetricId::CpuUtilMax, ms_local);
    PXV_PERF_OBSERVE(::pv::base::perf::MetricId::FrameViewport, ms_local);
    PXV_PERF_OP_MAX(name_local, ms_local);

    {
      PXV_PERF_SCOPE(::pv::base::perf::MetricId::FramePaintMid);
      PXV_PERF_TRACK_ROW(name_local);
      PXV_PERF_TRACK_AUX_NS(1000);
      PXV_PERF_TRACK_COUNT(3);
      PXV_PERF_ADD(::pv::base::perf::MetricId::PaintCalls, 1);
    }

    PXV_PERF_ADD(::pv::base::perf::MetricId::PaintCalls, 1);

    // The scope macros MUST have a disabled branch too. They were originally
    // defined only under PXV_PERF, which broke every non-instrumented build of
    // decodetrace.cpp / viewport_painter.cpp ("PXV_PERF_SCOPE_VIEWPORT was not
    // declared in this scope"). That was a real CI failure — this block is the
    // guard against repeating it.
    {
      PXV_PERF_SCOPE_VIEWPORT();
      PXV_PERF_PAINTMID_START();
      PXV_PERF_PAINTMID_END();
    }

    // Every remaining macro, so none can silently lose its disabled branch.
    PXV_PERF_HEARTBEAT();
    PXV_PERF_ZOOM_FRAME();
    PXV_PERF_COPY_STAGING(4096);
    PXV_PERF_COPY_EXPORT(4096);
    PXV_PERF_TRACK_AUX_MS(ms_local);
    PXV_PERF_TRACK_ROW_VALUES(name_local, ms_local, ms_local, 3);
    PXV_PERF_TRACK_COUNT_ONCE(name_local, 3);

    // Nothing may have been mutated: the macros are pure no-ops.
    QCOMPARE(count_local, 7);
    QCOMPARE(ms_local, 1.5);
    QCOMPARE(std::string(name_local), std::string("row"));
  }
};

QTEST_APPLESS_MAIN(TestPerfNoop)
#include "test_perf_noop.moc"
