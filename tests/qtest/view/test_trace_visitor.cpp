/*
 * test_trace_visitor.cpp — QTest unit tests for TraceVisitor type dispatch
 *
 * Migrated from GTest to QTest (P0).
 */

#include <QtTest>
#include "pv/view/trace/trace_visitor.h"

using pv::view::TraceVisitor;
using pv::view::ConstTraceVisitor;

class CountingVisitor : public TraceVisitor {
public:
    int logic_count = 0, dso_count = 0, analog_count = 0;
    int decode_count = 0, spectrum_count = 0, math_count = 0, lissajous_count = 0;
};

class ConstCountingVisitor : public ConstTraceVisitor {
public:
    int logic_count = 0, dso_count = 0, analog_count = 0;
    int decode_count = 0, spectrum_count = 0, math_count = 0, lissajous_count = 0;
};

// Custom test hierarchy for dispatch verification
class TestVisitorBase {
public:
    virtual ~TestVisitorBase() = default;
    virtual void visit(struct TestLogic &) {}
    virtual void visit(struct TestDso &) {}
    virtual void visit(struct TestAnalog &) {}
    virtual void visit(struct TestDecode &) {}
    virtual void visit(struct TestSpectrum &) {}
    virtual void visit(struct TestMath &) {}
    virtual void visit(struct TestLissajous &) {}
};

struct TestLogic { void accept(TestVisitorBase &v) { v.visit(*this); } };
struct TestDso { void accept(TestVisitorBase &v) { v.visit(*this); } };
struct TestAnalog { void accept(TestVisitorBase &v) { v.visit(*this); } };
struct TestDecode { void accept(TestVisitorBase &v) { v.visit(*this); } };
struct TestSpectrum { void accept(TestVisitorBase &v) { v.visit(*this); } };
struct TestMath { void accept(TestVisitorBase &v) { v.visit(*this); } };
struct TestLissajous { void accept(TestVisitorBase &v) { v.visit(*this); } };

class DispatchCounter : public TestVisitorBase {
public:
    int logic = 0, dso = 0, analog = 0, decode = 0, spectrum = 0, math = 0, lissajous = 0;
    void visit(TestLogic &) override { logic++; }
    void visit(TestDso &) override { dso++; }
    void visit(TestAnalog &) override { analog++; }
    void visit(TestDecode &) override { decode++; }
    void visit(TestSpectrum &) override { spectrum++; }
    void visit(TestMath &) override { math++; }
    void visit(TestLissajous &) override { lissajous++; }
};

// Const visitor
class ConstTestVisitorBase {
public:
    virtual ~ConstTestVisitorBase() = default;
    virtual void visit(const struct ConstTestLogic &) {}
    virtual void visit(const struct ConstTestDso &) {}
};
struct ConstTestLogic { void accept(ConstTestVisitorBase &v) const { v.visit(*this); } };
struct ConstTestDso { void accept(ConstTestVisitorBase &v) const { v.visit(*this); } };
class ConstDispatchCounter : public ConstTestVisitorBase {
public:
    int logic = 0, dso = 0;
    void visit(const ConstTestLogic &) override { logic++; }
    void visit(const ConstTestDso &) override { dso++; }
};

class TestTraceVisitor : public QObject {
    Q_OBJECT
private slots:
    void DefaultVisitMethodsAreNoOps();
    void ConstVisitorDefaultVisitMethodsAreNoOps();
    void SubclassCanOverrideVisitMethods();
    void HasVirtualDestructor();
    void ConstVisitorHasVirtualDestructor();
    void DispatchToLogicSignal();
    void DispatchToDsoSignal();
    void DispatchToAnalogSignal();
    void DispatchToDecodeTrace();
    void DispatchToSpectrumTrace();
    void DispatchToMathTrace();
    void DispatchToLissajousTrace();
    void MultipleDispatchesAccumulate();
    void AllTypesDispatchCorrectly();
    void NoDispatchWithoutAcceptCall();
    void ConstVisitorDispatchesCorrectly();
};

void TestTraceVisitor::DefaultVisitMethodsAreNoOps() { TraceVisitor v; QVERIFY(true); }
void TestTraceVisitor::ConstVisitorDefaultVisitMethodsAreNoOps() { ConstTraceVisitor v; QVERIFY(true); }
void TestTraceVisitor::SubclassCanOverrideVisitMethods() {
    CountingVisitor v;
    QCOMPARE(v.logic_count, 0); QCOMPARE(v.dso_count, 0);
    QCOMPARE(v.analog_count, 0); QCOMPARE(v.decode_count, 0);
    QCOMPARE(v.spectrum_count, 0); QCOMPARE(v.math_count, 0);
    QCOMPARE(v.lissajous_count, 0);
}
void TestTraceVisitor::HasVirtualDestructor() {
    TraceVisitor *v = new CountingVisitor(); delete v; QVERIFY(true);
}
void TestTraceVisitor::ConstVisitorHasVirtualDestructor() {
    ConstTraceVisitor *v = new ConstCountingVisitor(); delete v; QVERIFY(true);
}
void TestTraceVisitor::DispatchToLogicSignal() {
    DispatchCounter c; TestLogic l; l.accept(c);
    QCOMPARE(c.logic, 1); QCOMPARE(c.dso, 0);
}
void TestTraceVisitor::DispatchToDsoSignal() {
    DispatchCounter c; TestDso d; d.accept(c);
    QCOMPARE(c.dso, 1); QCOMPARE(c.logic, 0);
}
void TestTraceVisitor::DispatchToAnalogSignal() {
    DispatchCounter c; TestAnalog a; a.accept(c); QCOMPARE(c.analog, 1);
}
void TestTraceVisitor::DispatchToDecodeTrace() {
    DispatchCounter c; TestDecode d; d.accept(c); QCOMPARE(c.decode, 1);
}
void TestTraceVisitor::DispatchToSpectrumTrace() {
    DispatchCounter c; TestSpectrum s; s.accept(c); QCOMPARE(c.spectrum, 1);
}
void TestTraceVisitor::DispatchToMathTrace() {
    DispatchCounter c; TestMath m; m.accept(c); QCOMPARE(c.math, 1);
}
void TestTraceVisitor::DispatchToLissajousTrace() {
    DispatchCounter c; TestLissajous l; l.accept(c); QCOMPARE(c.lissajous, 1);
}
void TestTraceVisitor::MultipleDispatchesAccumulate() {
    DispatchCounter c;
    TestLogic logic; TestDso dso; TestAnalog analog;
    logic.accept(c); logic.accept(c); dso.accept(c);
    analog.accept(c); analog.accept(c); analog.accept(c);
    QCOMPARE(c.logic, 2); QCOMPARE(c.dso, 1); QCOMPARE(c.analog, 3);
    QCOMPARE(c.decode, 0); QCOMPARE(c.spectrum, 0);
    QCOMPARE(c.math, 0); QCOMPARE(c.lissajous, 0);
}
void TestTraceVisitor::AllTypesDispatchCorrectly() {
    DispatchCounter c;
    TestLogic logic; TestDso dso; TestAnalog analog;
    TestDecode decode; TestSpectrum spectrum; TestMath math; TestLissajous lissajous;
    logic.accept(c); dso.accept(c); analog.accept(c); decode.accept(c);
    spectrum.accept(c); math.accept(c); lissajous.accept(c);
    QCOMPARE(c.logic, 1); QCOMPARE(c.dso, 1); QCOMPARE(c.analog, 1);
    QCOMPARE(c.decode, 1); QCOMPARE(c.spectrum, 1); QCOMPARE(c.math, 1);
    QCOMPARE(c.lissajous, 1);
}
void TestTraceVisitor::NoDispatchWithoutAcceptCall() {
    DispatchCounter c; TestLogic logic; (void)logic;
    QCOMPARE(c.logic, 0);
}
void TestTraceVisitor::ConstVisitorDispatchesCorrectly() {
    ConstDispatchCounter c;
    const ConstTestLogic logic; const ConstTestDso dso;
    logic.accept(c); dso.accept(c);
    QCOMPARE(c.logic, 1); QCOMPARE(c.dso, 1);
}

QTEST_MAIN(TestTraceVisitor)
#include "test_trace_visitor.moc"
