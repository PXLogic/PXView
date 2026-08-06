/*
 * This file is part of the PXView project.
 *
 * Safe narrow-cast interface for the Trace hierarchy.
 *
 * Instead of dynamic_cast/static_cast to downcast from Trace* to a
 * concrete subclass, call the appropriate as_<type>() method. Each
 * subclass overrides its own method to `return this`; all others
 * inherit the base `return nullptr`. This eliminates all RTTI lookups
 * and makes the type-narrowing intent explicit at the call site.
 *
 * Usage:
 *   if (auto *dso = t->as_dso()) { dso->set_scale(...); }
 *   if (auto *logic = t->as_logic()) { logic->set_trig(...); }
 *
 * For multi-type dispatch (e.g. switch over all trace types), see
 * TraceVisitor below.
 */

#ifndef PXVIEW_PV_VIEW_TRACE_VISITOR_H
#define PXVIEW_PV_VIEW_TRACE_VISITOR_H

namespace pv {
namespace view {

// Forward declarations
class Trace;
class LogicSignal;
class DsoSignal;
class AnalogSignal;
class DecodeTrace;
class SpectrumTrace;
class MathTrace;
class LissajousTrace;

/**
 * Visitor interface for multi-type dispatch over the Trace hierarchy.
 *
 * Use when you need to handle multiple trace types in a single pass
 * (e.g. layout calculation, group counting). For simple "is this a
 * DsoSignal?" checks, prefer the as_<type>() methods on Trace.
 *
 * Usage:
 *   class MyVisitor : public TraceVisitor {
 *       void visit(DsoSignal& s) override { ... }
 *       void visit(LogicSignal& s) override { ... }
 *   };
 *   MyVisitor v;
 *   trace->accept(v);
 */
class TraceVisitor {
public:
    virtual ~TraceVisitor() = default;
    virtual void visit(LogicSignal&) {}
    virtual void visit(DsoSignal&) {}
    virtual void visit(AnalogSignal&) {}
    virtual void visit(DecodeTrace&) {}
    virtual void visit(SpectrumTrace&) {}
    virtual void visit(MathTrace&) {}
    virtual void visit(LissajousTrace&) {}
};

/**
 * Const variant of TraceVisitor for read-only traversal.
 */
class ConstTraceVisitor {
public:
    virtual ~ConstTraceVisitor() = default;
    virtual void visit(const LogicSignal&) {}
    virtual void visit(const DsoSignal&) {}
    virtual void visit(const AnalogSignal&) {}
    virtual void visit(const DecodeTrace&) {}
    virtual void visit(const SpectrumTrace&) {}
    virtual void visit(const MathTrace&) {}
    virtual void visit(const LissajousTrace&) {}
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_TRACE_VISITOR_H
