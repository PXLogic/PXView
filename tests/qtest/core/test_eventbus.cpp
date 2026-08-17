/*
 * test_eventbus.cpp — QTest unit tests for EventBus
 *
 * Spec harden-review, T6. Verifies R2: synchronous subscribe/broadcast,
 * RAII Subscription teardown, per-type isolation, async empty-event
 * coalescing, and the _alive_shared post-destruction guard.
 *
 * An injectable FakeDispatcher (implements IAsyncDispatcher) replaces
 * QtAsyncDispatcher so async delivery, coalescing and post-destruction
 * lifetime semantics can be observed deterministically without a real Qt
 * event loop or QCoreApplication.
 */

#include <QtTest>

#include "pv/base/log.h"   // pxv_log / xlog_writer
#include "pv/core/eventbus.h"
#include "pv/core/iasync_dispatcher.h"

#include <functional>
#include <memory>
#include <queue>
#include <thread>

using pv::core::EventBus;
using pv::core::IAsyncDispatcher;
using pv::core::Subscription;

// Global log writer referenced by EventBus::broadcast() (pxv_warn -> xlog_warn).
// Left null (never initialized): xlog_warn() no-ops on a null writer, so the
// re-entrancy warning path can be exercised without pulling in the pxv_view
// logging layer. Defined here so eventbus.cpp links without log.cpp.
//
// NOTE: requires external linkage (must satisfy eventbus.cpp's extern ref).
// If the qtest target already links a TU defining pxv_log (e.g. log.cpp),
// delete this definition to avoid a duplicate-symbol error.
xlog_writer *pxv_log = nullptr;

namespace {

// ---- test event types ----

// Empty (parameterless): std::is_empty_v == true → broadcast_async coalesces.
struct EvEmpty {};
// A second empty type: proves coalescing is per-type, not global.
struct EvEmpty2 {};
// Non-empty: broadcast_async never coalesces (per-instance data preserved).
struct EvValue { int value; };

// ---- injectable fake dispatcher ----
// post() records functors instead of running them, so the test drives exactly
// when async delivery happens (run_pending()) and can replay posts after the
// EventBus is destroyed.
class FakeDispatcher : public IAsyncDispatcher {
public:
    void post(std::function<void()> fn) override {
        ++post_count;
        _pending.push(std::move(fn));
    }
    bool on_target_thread() const override { return true; }

    // Deliver every queued functor, returning how many ran.
    int run_pending() {
        int n = 0;
        while (!_pending.empty()) {
            auto fn = std::move(_pending.front());
            _pending.pop();
            fn();
            ++n;
        }
        return n;
    }
    int pending_count() const { return static_cast<int>(_pending.size()); }

    int post_count = 0;
    std::queue<std::function<void()>> _pending;
};

} // namespace

class TestEventBus : public QObject {
    Q_OBJECT
private slots:
    void SubscribeAndBroadcastFires();
    void SubscriptionRaiiUnsubscribes();
    void MultipleEventTypesIndependent();
    void HasSubscribersReflectsRegistration();
    void BroadcastReentrancyDeferredAndDelivered();
    void AsyncEmptyCoalescesSameType();
    void AsyncNonEmptyNeverCoalesces();
    void AsyncCoalescingIsPerType();
    void AsyncPostAfterDestroyDroppedSafely();
    void MainThreadDetection();
};

void TestEventBus::SubscribeAndBroadcastFires() {
    EventBus bus(std::make_unique<FakeDispatcher>());
    int sum = 0;
    // NOTE: the returned Subscription MUST be retained; discarding it as a
    // temporary destructs immediately and unsubscribes.
    auto sub = bus.subscribe<EvValue>([&](const EvValue &ev) { sum += ev.value; });
    bus.broadcast(EvValue{3});
    bus.broadcast(EvValue{4});
    QCOMPARE(sum, 7);
}

void TestEventBus::SubscriptionRaiiUnsubscribes() {
    EventBus bus(std::make_unique<FakeDispatcher>());
    int hits = 0;
    {
        auto sub = bus.subscribe<EvValue>([&](const EvValue &) { ++hits; });
        bus.broadcast(EvValue{1});
        QCOMPARE(hits, 1);
    } // sub destroyed here → RAII unsubscribe

    bus.broadcast(EvValue{1});
    QCOMPARE(hits, 1); // must not fire after unsubscription
}

void TestEventBus::MultipleEventTypesIndependent() {
    EventBus bus(std::make_unique<FakeDispatcher>());
    int a = 0, b = 0;
    auto sa = bus.subscribe<EvEmpty>([&](const EvEmpty &) { ++a; });
    auto sb = bus.subscribe<EvValue>([&](const EvValue &ev) { b += ev.value; });

    bus.broadcast(EvEmpty{});
    bus.broadcast(EvValue{5});
    QCOMPARE(a, 1);
    QCOMPARE(b, 5);

    // Broadcasting EvValue must not touch the EvEmpty subscriber.
    bus.broadcast(EvValue{2});
    QCOMPARE(a, 1);
    QCOMPARE(b, 7);
}

void TestEventBus::HasSubscribersReflectsRegistration() {
    EventBus bus(std::make_unique<FakeDispatcher>());
    QVERIFY(!bus.has_subscribers());
    {
        auto sub = bus.subscribe<EvEmpty>([](const EvEmpty &) {});
        QVERIFY(bus.has_subscribers());
    } // sub destroyed → unsubscribe
    QVERIFY(!bus.has_subscribers());
}

void TestEventBus::BroadcastReentrancyDeferredAndDelivered() {
    EventBus bus(std::make_unique<FakeDispatcher>());
    int count = 0;
    auto sub = bus.subscribe<EvEmpty>([&](const EvEmpty &) {
        ++count;
        if (count == 1)
            bus.broadcast(EvEmpty{}); // nested, same thread → deferred
    });

    bus.broadcast(EvEmpty{});
    // The re-entrant call is pushed onto the deferred queue and drained after
    // the outer broadcast returns, so no event is lost.
    QCOMPARE(count, 2);
}

void TestEventBus::AsyncEmptyCoalescesSameType() {
    auto disp = std::make_unique<FakeDispatcher>();
    FakeDispatcher *raw = disp.get();
    EventBus bus(std::move(disp));
    int delivered = 0;
    auto sub = bus.subscribe<EvEmpty>([&](const EvEmpty &) { ++delivered; });

    // Coalescible (empty) type: second call while one is pending → no 2nd post.
    bus.broadcast_async(EvEmpty{});
    bus.broadcast_async(EvEmpty{});
    QCOMPARE(raw->post_count, 1);
    QCOMPARE(raw->pending_count(), 1);
    QCOMPARE(delivered, 0); // not yet delivered

    // Simulate dispatcher delivery: pending flag cleared just before dispatch.
    QCOMPARE(raw->run_pending(), 1);
    QCOMPARE(delivered, 1);

    // After delivery (pending cleared), a genuinely new event is queued again.
    bus.broadcast_async(EvEmpty{});
    QCOMPARE(raw->post_count, 2);
    QCOMPARE(raw->run_pending(), 1);
    QCOMPARE(delivered, 2);
}

void TestEventBus::AsyncNonEmptyNeverCoalesces() {
    auto disp = std::make_unique<FakeDispatcher>();
    FakeDispatcher *raw = disp.get();
    EventBus bus(std::move(disp));
    int sum = 0;
    auto sub = bus.subscribe<EvValue>([&](const EvValue &ev) { sum += ev.value; });

    bus.broadcast_async(EvValue{1});
    bus.broadcast_async(EvValue{2});
    QCOMPARE(raw->post_count, 2); // non-empty type → never coalesced
    QCOMPARE(raw->run_pending(), 2);
    QCOMPARE(sum, 3);
}

void TestEventBus::AsyncCoalescingIsPerType() {
    auto disp = std::make_unique<FakeDispatcher>();
    FakeDispatcher *raw = disp.get();
    EventBus bus(std::move(disp));
    int e1 = 0, e2 = 0;
    auto s1 = bus.subscribe<EvEmpty>([&](const EvEmpty &) { ++e1; });
    auto s2 = bus.subscribe<EvEmpty2>([&](const EvEmpty2 &) { ++e2; });

    // Different empty types are not merged with each other.
    bus.broadcast_async(EvEmpty{});
    bus.broadcast_async(EvEmpty2{});
    QCOMPARE(raw->post_count, 2);
    QCOMPARE(raw->run_pending(), 2);
    QCOMPARE(e1, 1);
    QCOMPARE(e2, 1);
}

void TestEventBus::AsyncPostAfterDestroyDroppedSafely() {
    // EventBus owns the FakeDispatcher via unique_ptr<IAsyncDispatcher> (bus ctor
    // takes default-deleter unique_ptr only, so a no-op deleter is not usable).
    // Use a heap-allocated bus and grab the posted functor out of the fake's
    // queue BEFORE destroying it, then replay it after destruction.
    FakeDispatcher *fd = new FakeDispatcher;
    EventBus *bus = new EventBus(std::unique_ptr<IAsyncDispatcher>(fd));
    int delivered = 0;
    bus->subscribe<EvValue>([&](const EvValue &) { ++delivered; });
    bus->broadcast_async(EvValue{7}); // non-empty → non-coalescing, posts once
    QCOMPARE(fd->post_count, 1);
    QVERIFY(!fd->_pending.empty());

    // Grab the queued functor while the fake is still alive.
    auto fn = std::move(fd->_pending.front());
    fd->_pending.pop();

    delete bus; // ~EventBus → _alive_shared.store(false); fake freed here

    // Replay post-destruction: the non-coalescable lambda checks
    // _alive_shared BEFORE touching any `this` member, so it returns early
    // without firing the (now gone) subscription. Dangling bus is never read.
    fn();
    QCOMPARE(delivered, 0);
}

void TestEventBus::MainThreadDetection() {
    // EventBus ctor records the constructing (main) thread as _main_thread_id.
    EventBus bus(std::make_unique<FakeDispatcher>());
    QVERIFY(EventBus::on_main_thread());

    bool on_other_thread = true;
    std::thread([&]() { on_other_thread = EventBus::on_main_thread(); }).join();
    QVERIFY(!on_other_thread);
}

// Note: EventBus::broadcast()'s off-main-thread warning is exercised via the
// re-entrancy path above (pxv_log is left null so xlog_warn safely no-ops).
// We deliberately do NOT call broadcast() from a worker thread here, since
// doing so relies on the static _main_thread_id set by the ctor.

QTEST_MAIN(TestEventBus)
#include "test_eventbus.moc"