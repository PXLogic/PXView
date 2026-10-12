// Perf metric registry — unit tests.
//
// Validation split (see mem/perf-instrumentation.md): COUNT metrics are
// deterministic, so they are asserted to EXACT values. TIMING metrics are
// wall-clock dependent, so only invariants are asserted (min <= avg <= max,
// no negatives, window clears) — never a value.
//
// Compiled with PXV_PERF and linked against pv/base/perf.cpp directly (the
// registry is deliberately Qt-free, so no pxview-core link is needed).

#include <QtTest>
#include <QJsonDocument>

#include <cstring>
#include <string>
#include <vector>

#include "pv/base/perf.h"

using namespace pv::base::perf;

namespace {

// NOTE: always pass a NAMED Snapshot local. Passing take_snapshot() straight in
// binds the returned pointer to a temporary that dies at the end of the full
// expression, so the pointer dangles and reads recycled heap memory. This
// actually bit us: the failing value was a reused heap address, identical for
// every metric.
const MetricSample *find_metric(const Snapshot &s, MetricId id) {
  for (const MetricSample &m : s.metrics) {
    if (m.id == id)
      return &m;
  }
  return nullptr;
}

const NamedSample *find_row(const Snapshot &s, const char *name) {
  for (const NamedSample &r : s.rows) {
    if (r.name == name)
      return &r;
  }
  return nullptr;
}

const NamedSample *find_op(const Snapshot &s, const char *name) {
  for (const NamedSample &o : s.ops) {
    if (o.name == name)
      return &o;
  }
  return nullptr;
}

class RecordingSink : public IPerfSink {
public:
  const char *name() const override { return "recording"; }
  void on_window(const Snapshot &s) override { windows.push_back(s); }
  std::vector<Snapshot> windows;
};

}  // namespace

class TestPerfRegistry : public QObject {
  Q_OBJECT

private slots:
  void init() {
    set_level(Level::Basic);
    set_flush_interval_ms(2000);
    set_heartbeat_interval_ms(100);
    set_frame_idle_ms(250);
    take_snapshot(true);  // clear any residue from the previous test
  }

  // ---- table consistency -------------------------------------------------

  void metricTableIsConsistent() {
    QCOMPARE(metric_count(), static_cast<std::size_t>(MetricId::Count_));
    for (std::size_t i = 0; i < metric_count(); ++i) {
      const MetricDef &d = metric_def(static_cast<MetricId>(i));
      QVERIFY(d.name && *d.name);
      QVERIFY(d.unit && *d.unit);
      QVERIFY(d.desc && *d.desc);
      // name lookup round-trips
      QCOMPARE(metric_by_name(d.name), static_cast<MetricId>(i));
      // names are unique (they are the JSON ids)
      for (std::size_t j = i + 1; j < metric_count(); ++j) {
        QVERIFY(std::strcmp(d.name, metric_def(static_cast<MetricId>(j)).name) != 0);
      }
    }
    QCOMPARE(metric_by_name("no.such.metric"), MetricId::Count_);
  }

  // A frame RATE must report the LATEST sample, not a window average: the
  // gauge is sampled on every heartbeat (~10ms) while frames arrive in
  // bursts, so an average is dominated by idle samples and reads ~0 even
  // while frames are flowing (measured: avg=0, max=82 — the trend chart was
  // a flat line). Locking the aggregation here so it cannot regress.
  void frameRateMetricsUseLatestSample() {
    QVERIFY(metric_def(MetricId::FrameFps).agg == Agg::Last);
    QVERIFY(metric_def(MetricId::FrameViewportFps).agg == Agg::Last);
  }

  // ---- count metrics: exact assertions -----------------------------------

  void counterAccumulatesExactCounts() {
    add(MetricId::AnnInRange, 5);
    add(MetricId::AnnInRange, 5);
    add(MetricId::AnnInRange, 5);

    const Snapshot s = take_snapshot(true);
    const MetricSample *m = find_metric(s, MetricId::AnnInRange);
    QVERIFY(m != nullptr);
    QCOMPARE(static_cast<quint64>(m->calls), quint64(3));
    QCOMPARE(m->total, 15.0);
    QCOMPARE(m->max, 5.0);
    QCOMPARE(m->min, 5.0);
    // frame.ann_in_range is declared Agg::Avg — the meaningful number is the
    // per-frame average (it replaces the old PER_FRAME_AVG line), not the sum.
    QCOMPARE(m->value, 5.0);
    QCOMPARE(m->total, 15.0);  // the sum is still available alongside
  }

  void gaugeTracksWindowMaxAndMin() {
    set_gauge(MetricId::EventLagMax, 0.0);
    set_gauge(MetricId::EventLagMax, 10.5);
    set_gauge(MetricId::EventLagMax, 3.0);

    const Snapshot s = take_snapshot(true);
    const MetricSample *m = find_metric(s, MetricId::EventLagMax);
    QVERIFY(m != nullptr);
    QCOMPARE(static_cast<quint64>(m->calls), quint64(3));
    QCOMPARE(m->max, 10.5);
    QCOMPARE(m->min, 0.0);
    QCOMPARE(m->last, 3.0);
    QCOMPARE(m->value, 10.5);  // Agg::Max
  }

  // ---- timing metrics: invariants only -----------------------------------

  void timerTracksSumBoundsAndAverage() {
    observe(MetricId::FrameViewport, 2.0);
    observe(MetricId::FrameViewport, 4.0);
    observe(MetricId::FrameViewport, 6.0);

    const Snapshot s = take_snapshot(true);
    const MetricSample *m = find_metric(s, MetricId::FrameViewport);
    QVERIFY(m != nullptr);
    QCOMPARE(static_cast<quint64>(m->calls), quint64(3));
    QVERIFY(qAbs(m->total - 12.0) < 1e-6);
    QCOMPARE(m->max, 6.0);
    QCOMPARE(m->min, 2.0);
    QVERIFY(qAbs(m->value - 4.0) < 1e-6);  // Agg::Avg
  }

  void timingInvariantsHold() {
    observe(MetricId::FramePaintMid, 1.0);
    observe(MetricId::FramePaintMid, 9.0);
    observe(MetricId::FramePaintMid, 5.0);

    const Snapshot s = take_snapshot(true);
    const MetricSample *m = find_metric(s, MetricId::FramePaintMid);
    QVERIFY(m != nullptr);
    const double avg = m->total / static_cast<double>(m->calls);
    QVERIFY(m->min >= 0.0);
    QVERIFY(m->max >= 0.0);
    QVERIFY(m->min <= avg);
    QVERIFY(avg <= m->max);
  }

  // ---- named metrics -----------------------------------------------------

  void namedRowAggregates() {
    track_row("I2C: Bits", 2.0, 0.5, 10);
    track_row("I2C: Bits", 4.0, 1.0, 20);

    const Snapshot s = take_snapshot(true);
    const NamedSample *r = find_row(s, "I2C: Bits");
    QVERIFY(r != nullptr);
    QCOMPARE(static_cast<quint64>(r->calls), quint64(2));
    QVERIFY(qAbs(r->total - 6.0) < 1e-6);
    QCOMPARE(r->max, 4.0);
    QVERIFY(qAbs(r->aux - 1.5) < 1e-6);
    QCOMPARE(static_cast<quint64>(r->count), quint64(30));
  }

  void namedRowsAreSortedHottestFirst() {
    track_row("cheap", 1.0, 0.0, 1);
    track_row("expensive", 50.0, 0.0, 1);
    track_row("mid", 10.0, 0.0, 1);

    const Snapshot s = take_snapshot(true);
    QCOMPARE(s.rows.size(), std::size_t(3));
    QCOMPARE(s.rows[0].name, std::string("expensive"));
    QCOMPARE(s.rows[1].name, std::string("mid"));
    QCOMPARE(s.rows[2].name, std::string("cheap"));
  }

  void opMaxKeepsMaximum() {
    op_max("signals_changed", 12.4);
    op_max("signals_changed", 2.0);

    const Snapshot s = take_snapshot(true);
    const NamedSample *o = find_op(s, "signals_changed");
    QVERIFY(o != nullptr);
    QCOMPARE(o->max, 12.4);
    QCOMPARE(static_cast<quint64>(o->calls), quint64(2));
  }

  // ---- window semantics --------------------------------------------------

  void snapshotResetClearsWindow() {
    add(MetricId::PublishCount, 1);
    const Snapshot first = take_snapshot(true);
    QVERIFY(find_metric(first, MetricId::PublishCount) != nullptr);
    // second window must be empty — the roll really clears
    const Snapshot second = take_snapshot(true);
    QVERIFY(find_metric(second, MetricId::PublishCount) == nullptr);
  }

  void peekDoesNotDisturbWindow() {
    add(MetricId::BatchFlushes, 1);
    add(MetricId::BatchFlushes, 1);
    // reset=false is the peek used by the HTTP endpoint: must not steal data
    const Snapshot peeked = take_snapshot(false);
    const MetricSample *peek = find_metric(peeked, MetricId::BatchFlushes);
    QVERIFY(peek != nullptr);
    QCOMPARE(static_cast<quint64>(peek->calls), quint64(2));
    const Snapshot after = take_snapshot(true);
    const MetricSample *still = find_metric(after, MetricId::BatchFlushes);
    QVERIFY(still != nullptr);
    QCOMPARE(static_cast<quint64>(still->calls), quint64(2));  // still there
  }

  // ---- gating ------------------------------------------------------------

  void disabledLevelSamplesNothing() {
    RecordingSink sink;
    add_sink(&sink);

    set_level(Level::Off);
    add(MetricId::AnnInRange, 100);
    observe(MetricId::FrameViewport, 5.0);
    set_gauge(MetricId::CpuUtilMax, 3.0);
    track_row("X", 1.0, 0.0, 1);
    op_max("Y", 1.0);
    flush_window();
    QCOMPARE(sink.windows.size(), std::size_t(0));

    set_level(Level::Basic);
    const Snapshot s = take_snapshot(true);
    QVERIFY(find_metric(s, MetricId::AnnInRange) == nullptr);
    QVERIFY(find_metric(s, MetricId::FrameViewport) == nullptr);
    QVERIFY(find_metric(s, MetricId::CpuUtilMax) == nullptr);
    QVERIFY(find_row(s, "X") == nullptr);
    QVERIFY(find_op(s, "Y") == nullptr);

    remove_sink(&sink);
  }

  void sinkReceivesWindowOnFlush() {
    RecordingSink sink;
    add_sink(&sink);

    add(MetricId::BatchFlushes, 1);
    add(MetricId::BatchFlushes, 1);
    flush_window();

    QCOMPARE(sink.windows.size(), std::size_t(1));
    const MetricSample *m =
        find_metric(sink.windows.back(), MetricId::BatchFlushes);
    QVERIFY(m != nullptr);
    QCOMPARE(static_cast<quint64>(m->calls), quint64(2));

    // flush rolls the window: a second flush with no activity emits nothing
    flush_window();
    QCOMPARE(sink.windows.size(), std::size_t(1));

    remove_sink(&sink);
  }

  void removingASinkStopsDelivery() {
    RecordingSink sink;
    add_sink(&sink);
    add(MetricId::BatchFlushes, 1);
    flush_window();
    QCOMPARE(sink.windows.size(), std::size_t(1));

    remove_sink(&sink);
    add(MetricId::BatchFlushes, 1);
    flush_window();
    QCOMPARE(sink.windows.size(), std::size_t(1));  // unchanged
  }

  // ---- frame / heartbeat drivers ----------------------------------------

  void frameBoundaryRecordsIntervalAndFrameCount() {
    frame_boundary();     // (re)prime the frame clock
    take_snapshot(true);  // drop whatever that recorded

    frame_boundary();  // now a small, continuous gap is measured
    const Snapshot s = take_snapshot(true);

    const MetricSample *iv = find_metric(s, MetricId::FrameInterval);
    QVERIFY(iv != nullptr);
    QCOMPARE(static_cast<quint64>(iv->calls), quint64(1));
    QVERIFY(iv->value >= 0.0);
    QVERIFY(iv->value < 250.0);  // must not be mistaken for an idle gap

    const MetricSample *pc = find_metric(s, MetricId::PaintCalls);
    QVERIFY(pc != nullptr);
    QCOMPARE(static_cast<quint64>(pc->calls), quint64(1));
  }

  void idleGapIsNotRecordedAsFrameInterval() {
    set_frame_idle_ms(1);
    frame_boundary();
    take_snapshot(true);

    QTest::qWait(30);  // longer than the idle threshold
    frame_boundary();

    const Snapshot s = take_snapshot(true);
    QVERIFY2(find_metric(s, MetricId::FrameInterval) == nullptr,
             "an idle gap must not be recorded as a dropped frame");

    set_frame_idle_ms(250);
  }

  void heartbeatRecordsNonNegativeLag() {
    heartbeat();
    take_snapshot(true);
    heartbeat();

    const Snapshot s = take_snapshot(true);
    const MetricSample *m = find_metric(s, MetricId::EventLagMax);
    QVERIFY(m != nullptr);
    QVERIFY(m->max >= 0.0);
    QVERIFY(m->min >= 0.0);
  }

  // ---- serialisation -----------------------------------------------------

  void jsonIsParseableAndEscaped() {
    add(MetricId::AnnInRange, 7);
    track_row("row \"quoted\" \\ backslash", 1.0, 0.0, 1);
    op_max("op\twith\ttabs", 3.0);

    const std::string json = snapshot_to_json(take_snapshot(true));
    QVERIFY(!json.empty());
    QCOMPARE(json.front(), '{');
    QCOMPARE(json.back(), '}');

    // The real check: a strict parser must accept it. Hand-rolled escaping
    // bugs (unescaped quotes / control chars in decoder row titles) show up
    // here and nowhere else.
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(json));
    QVERIFY2(!doc.isNull(), json.c_str());
    QVERIFY(doc.isObject());
    QVERIFY(doc.object().contains("metrics"));
    QVERIFY(doc.object().contains("rows"));
    QVERIFY(doc.object().contains("window_s"));
  }

  void textOutputMentionsEveryReportedMetric() {
    add(MetricId::AnnInRange, 3);
    track_row("some row", 1.0, 0.0, 1);
    op_max("some op", 2.0);

    const std::string text = snapshot_to_text(take_snapshot(true));
    QVERIFY(text.find("WINDOW") != std::string::npos);
    QVERIFY(text.find(metric_def(MetricId::AnnInRange).name) != std::string::npos);
    QVERIFY(text.find("some row") != std::string::npos);
    QVERIFY(text.find("some op") != std::string::npos);
  }
};

QTEST_MAIN(TestPerfRegistry)
#include "test_perf_registry.moc"
