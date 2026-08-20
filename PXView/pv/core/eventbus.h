#ifndef PXVIEW_CORE_EVENTBUS_H
#define PXVIEW_CORE_EVENTBUS_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "pv/core/iasync_dispatcher.h"
#include "pv/base/log.h"
#include "pv/utility/atomic_shared_ptr.h"

namespace pv {
namespace core {

/**
 * Subscription — RAII handle for a type-erased callback subscription.
 *
 * Returned by EventBus::subscribe<T>(). When destroyed, the callback is
 * automatically unsubscribed. Move-only.
 *
 * Replaces IEventListener's 45 virtual functions with fine-grained
 * per-event-type lambda registration.
 */
class Subscription {
public:
    Subscription() = default;
    Subscription(std::function<void()> unsub)
        : _unsub(std::make_shared<std::function<void()>>(std::move(unsub))) {}
    ~Subscription() {
        if (_unsub && *_unsub) (*_unsub)();
    }
    Subscription(Subscription &&) = default;
    Subscription &operator=(Subscription &&) = default;
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;
private:
    std::shared_ptr<std::function<void()>> _unsub;
};

/**
 * EventBus — central dispatch hub for Core→View/Service notifications.
 *
 * Two dispatch paths:
 *   * broadcast<T>() — synchronous. Dispatches to all subscribe<T>()
 *     callbacks for the event type. A thread_local _broadcast_depth guard
 *     defers re-entrant calls to prevent event loss.
 *   * broadcast_async<T>() — asynchronous. Queues a typed event onto
 *     the target thread via the injected IAsyncDispatcher (default:
 *     QtAsyncDispatcher which uses QCoreApplication::postEvent).
 *
 * IEventListener has been removed. All event consumers use subscribe<T>()
 * with RAII Subscription management.
 *
 * Thread safety (modernize-thread-model Task 4 — COW subscription table):
 *   The subscription table is copy-on-write. subscribe<T>() / the
 *   Subscription destructor (the only writers) mutate the authoritative
 *   `_callbacks` map under `_callbacks_mutex`, then publish a fresh immutable
 *   snapshot (`_snapshot`, pv::atomic_shared_ptr). Readers
 *   (dispatch_to_callbacks / has_subscribers) load the snapshot and iterate
 *   it — no per-event vector copy. A snapshot is only rebuilt when a
 *   subscription actually changes, so the broadcast hot path (e.g. DataUpdated)
 *   does a single snapshot load + iterate instead of a shared_mutex read lock
 *   + vector copy + allocation on every event.
 *
 *   Snapshot shared_ptrs keep every callback's std::function storage alive
 *   while any reader still holds the snapshot, so an unsubscribed callback
 *   that was already published in an in-flight snapshot can run at most once
 *   more but never touches freed storage (callback context keep-alive).
 */
class EventBus {
public:
    // Construct with an optional custom async dispatcher.  If nullptr,
    // a QtAsyncDispatcher (QCoreApplication::postEvent) is used by default.
    explicit EventBus(std::unique_ptr<IAsyncDispatcher> dispatcher = nullptr);
    ~EventBus();

    // ---- Type-erased callback subscription ----
    // Register a lambda for a specific event type. Returns a Subscription
    // that auto-unsubscribes on destruction (RAII).
    template <typename EventType>
    Subscription subscribe(std::function<void(const EventType &)> cb) {
        auto type_id = std::type_index(typeid(EventType));
        auto cb_ptr = std::make_shared<
            std::function<void(const EventType &)>>(std::move(cb));
        auto erased = std::make_shared<std::function<void(const void *)>>(
            [cb_ptr](const void *ptr) {
                (*cb_ptr)(*static_cast<const EventType *>(ptr));
            });
        std::lock_guard<std::mutex> lk(_callbacks_mutex);
        auto &vec = _callbacks[type_id];
        // Reuse a previously vacated (unsubscribed) slot when available, so the
        // vector size tracks the historical peak subscription count rather than
        // the cumulative number of sub/unsub cycles. Each Subscription captures
        // its own idx, so we must not swap/pop (which would invalidate indices).
        size_t idx = vec.size();
        for (size_t i = 0; i < vec.size(); ++i) {
            if (!vec[i]) {
                idx = i;
                break;
            }
        }
        if (idx == vec.size())
            vec.push_back(erased);
        else
            vec[idx] = erased;
        rebuild_snapshot(); // publish a fresh COW snapshot (version++)
        return Subscription([this, type_id, idx]() {
            std::lock_guard<std::mutex> lk2(_callbacks_mutex);
            auto it = _callbacks.find(type_id);
            if (it != _callbacks.end() && idx < it->second.size()) {
                it->second[idx].reset();
                rebuild_snapshot(); // unsubscribed → publish a fresh snapshot
            }
        });
    }

    // ---- Query ----
    // Returns true iff at least one LIVE (subscribed, not-yet-unsubscribed)
    // callback exists across all event types. Unsubscribed slots (reset to
    // null) are not counted. Reads the current COW snapshot without touching
    // `_callbacks`.
    bool has_subscribers() const {
        auto snap = _snapshot.load(std::memory_order_acquire);
        if (!snap)
            return false;
        for (const auto &kv : *snap)
            for (const auto &cb : kv.second)
                if (cb)
                    return true;
        return false;
    }

    // Monotonic snapshot version — incremented on every COW publish
    // (i.e. every actual subscription-table change). Lock-free read; used by
    // tests/debugging to assert "only rebuild on change" semantics.
    uint64_t version() const { return _version.load(std::memory_order_relaxed); }

    // ---- Sync typed event broadcast ----
    template <typename EventType> void broadcast(const EventType &ev) {
        // R4: broadcast() and _deferred_broadcasts are main-thread-only.
        // Redirect off-main-thread misuse to the async path instead of
        // racing on the non-thread-safe _deferred_broadcasts queue.
        if (!on_main_thread()) {
            pxv_err("EventBus::broadcast invoked off the main thread; "
                    "redirecting to async dispatch (broadcast_async).");
            broadcast_async(ev);
            return;
        }
        ++_broadcast_depth;
        if (_broadcast_depth > 1) {
            pxv_warn("Event broadcast re-entrancy detected (depth=%d), "
                     "deferring event for later delivery",
                     _broadcast_depth);
            _deferred_broadcasts.push([this, ev]() {
                dispatch_to_callbacks<EventType>(ev);
            });
            --_broadcast_depth;
            return;
        }
        dispatch_to_callbacks(ev);
        --_broadcast_depth;
        drain_deferred();
    }

    // ---- Async typed event broadcast (worker-thread → main-thread) ----
    //
    // For empty (parameterless) event types — detected at compile time via
    // std::is_empty_v — e.g. DataUpdated, CollectEnd, EndCollectWork,
    // DeviceOptionsUpdated, SessionStopped — same-type async events are
    // coalesced: if an event of the same type is already pending in the qApp
    // event queue, subsequent calls are silently dropped.  The pending flag
    // is cleared just before dispatch, so a genuinely new event posted during
    // dispatch will still be queued.
    //
    // For non-empty event types (TriggerReceived{pos}, CaptureStateChanged{...},
    // SignalsChanged{rebuild_kind,...}, ...) every call is dispatched
    // individually — coalescing would lose per-instance data.
    template <typename EventType> void broadcast_async(const EventType &ev) {
        auto alive = _alive_shared;
        // Safety check BEFORE touching any members (incl. _pending_async_types):
        // closes the race window where the EventBus is destroyed concurrently
        // with this call, so we don't touch a freed member after the destructor.
        if (!alive->load(std::memory_order_acquire))
            return; // EventBus already destroyed — drop the event
        // Shared ownership: keep the dispatcher alive for callbacks already
        // queued/in-flight even if this EventBus is destroyed meanwhile, so the
        // posted lambda dispatches via this copy rather than `this->_dispatcher`.
        std::shared_ptr<IAsyncDispatcher> disp = _dispatcher;

        if constexpr (std::is_empty_v<EventType>) {
            // Coalescable: skip if same type is already pending
            {
                std::lock_guard<std::mutex> lk(_pending_async_mutex);
                auto ti = std::type_index(typeid(EventType));
                if (_pending_async_types.count(ti) > 0)
                    return; // already pending — coalesce
                _pending_async_types.insert(ti);
            }
            dispatch_async(disp, [this, alive]() {
                // Check liveness FIRST so a post replayed after ~EventBus never
                // touches freed members (incl. _pending_async_mutex/types).
                if (!alive->load(std::memory_order_acquire))
                    return;
                // Clear pending flag BEFORE dispatching so that events posted
                // during dispatch are not lost.
                {
                    std::lock_guard<std::mutex> lk(_pending_async_mutex);
                    _pending_async_types.erase(
                        std::type_index(typeid(EventType)));
                }
                // Empty event — default-construct (no per-instance data)
                broadcast(EventType{});
            });
        } else {
            // Non-coalescable: always dispatch
            dispatch_async(disp, [this, ev, alive]() {
                if (!alive->load(std::memory_order_acquire))
                    return;
                broadcast(ev);
            });
        }
    }

    // ---- Internal: post a functor to the target thread via the injected
    //      IAsyncDispatcher. Used by broadcast_async<T>(). Accepts the
    //      dispatcher by shared_ptr so queued callbacks keep it alive.
    // Single-arg overload keeps the dispatcher alive through a local shared
    // copy even if the EventBus is destroyed; used by external callers
    // (e.g. SigSession hotplug forwarding).
    void dispatch_async(std::function<void()> fn);
    void dispatch_async(std::shared_ptr<IAsyncDispatcher> disp,
                        std::function<void()> fn);

    // ---- Static convenience: post a functor to the main thread via a
    //      global default QtAsyncDispatcher.  Used by external callers
    //      (SessionService, WsTransport, McpTransport, DeviceOptionsDock)
    //      that need "post to main thread" without holding an EventBus
    //      instance.  When QCoreApplication is available this is equivalent
    //      to QCoreApplication::postEvent with a custom event type.
    static void post_async_dispatch(std::function<void()> fn);

    // Check if the current thread is the main (GUI) thread.
    static bool on_main_thread() {
        if (_main_thread_id == std::thread::id{})
            return false;
        return std::this_thread::get_id() == _main_thread_id;
    }

private:
    template <typename EventType>
    void dispatch_to_callbacks(const EventType &ev) {
        // modernize-thread-model Task 4: read the current COW snapshot
        // (pv::atomic_shared_ptr load) and iterate it directly — no
        // shared_mutex, no per-event vector copy, no allocation. The snapshot
        // is immutable: concurrent subscribe/unsubscribe swap in a NEW
        // snapshot and never mutate this one, so iterating here is safe.
        // A callback that is unsubscribed after this snapshot was published
        // but before its invocation may run at most once more (the snapshot
        // shared_ptr keeps its storage alive — no dangling callback).
        auto snap = _snapshot.load(std::memory_order_acquire);
        if (!snap)
            return;
        auto it = snap->find(std::type_index(typeid(EventType)));
        if (it == snap->end())
            return;
        for (const auto &cb : it->second) {
            if (cb) (*cb)(static_cast<const void *>(&ev));
        }
    }

    // ---- Type-erased callback storage (writer-only, COW) ----
    using CallbackMap = std::unordered_map<
        std::type_index,
        std::vector<std::shared_ptr<std::function<void(const void *)>>>>;
    // Authoritative table. Mutated only by subscribe<T>() / the Subscription
    // destructor under _callbacks_mutex; readers never touch it.
    CallbackMap _callbacks;
    std::mutex _callbacks_mutex;
    // Immutable COW snapshot. Writers rebuild it (deep structural copy —
    // callback bodies are shared via shared_ptr, not duplicated) and publish
    // it only when a subscription actually changes; readers load it and
    // iterate the copy. pv::atomic_shared_ptr (pv/utility/atomic_shared_ptr.h)
    // is the real std::atomic<std::shared_ptr> on libstdc++/MSVC and
    // mutex-backed on libc++, which never implemented the C++20
    // specialization (P0718R2).
    atomic_shared_ptr<const CallbackMap> _snapshot{};
    // Monotonic counter incremented on every snapshot publish ("版本化").
    std::atomic<uint64_t> _version{0};

    // Caller MUST hold _callbacks_mutex. Deep-copies _callbacks into a fresh
    // immutable snapshot and publishes it atomically.
    void rebuild_snapshot() {
        if (_callbacks.empty()) {
            _snapshot.store(nullptr, std::memory_order_release);
            _version.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        _snapshot.store(std::make_shared<const CallbackMap>(_callbacks),
                        std::memory_order_release);
        _version.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- Re-entrancy guard ----
    // CRITICAL: broadcast() and the _deferred_broadcasts queue are NOT
    // thread-safe. They must only be called from the main (GUI) thread.
    // The _broadcast_depth counter is thread_local, so re-entrant calls
    // from the same thread are correctly deferred, but concurrent calls
    // from different threads would race on _deferred_broadcasts.
    // In practice this is safe because broadcast() is only invoked from
    // broadcast_async()'s main-thread dispatch. Direct broadcast() calls
    // from worker threads are prohibited by contract.
    static thread_local int _broadcast_depth;
    std::queue<std::function<void()>> _deferred_broadcasts; // main-thread-only

    void drain_deferred() {
        while (!_deferred_broadcasts.empty()) {
            auto fn = std::move(_deferred_broadcasts.front());
            _deferred_broadcasts.pop();
            fn();
        }
    }

    // ---- Async event coalescing ----
    // Tracks which empty event types have a pending async dispatch.
    // Protected by _pending_async_mutex.
    std::set<std::type_index> _pending_async_types;
    std::mutex _pending_async_mutex;

    // ---- Lifetime management ----
    std::shared_ptr<std::atomic<bool>> _alive_shared;
    static std::thread::id _main_thread_id;

    // ---- Async dispatch strategy (injected, defaults to QtAsyncDispatcher) ----
    // Shared ownership: queued/in-flight async callbacks hold a copy so the
    // dispatcher stays alive even after the EventBus itself is destroyed.
    std::shared_ptr<IAsyncDispatcher> _dispatcher;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_EVENTBUS_H
