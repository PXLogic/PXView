#ifndef PXVIEW_CORE_EVENTBUS_H
#define PXVIEW_CORE_EVENTBUS_H

#include <QObject>
#include <QCoreApplication>
#include <QEvent>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <functional>
#include <vector>
#include <concepts>

#include "../interface/icallbacks.h"
#include "../interface/events.h"
#include "../log.h"

namespace pv {
namespace core {

/**
 * EventBus — central dispatch hub for Core→View/Service notifications.
 *
 * Three dispatch paths, all typed:
 *   * broadcast<T>() — synchronous. Invoked from within the async-dispatched
 *     handler (or from the main thread directly), so it already runs after the
 *     caller's stack frame has unwound. A thread_local _broadcast_depth guard
 *     prevents re-entrant typed event dispatch.
 *   * broadcast_sync<T>() — synchronous direct dispatch for the few pre/post
 *     ordering codes that MUST run synchronously BEFORE the state mutation
 *     (e.g. CurrentDeviceChangePrev / StartCollectWorkPrev / StoreConfPrev).
 *     Callers must guarantee they are on the main thread. Shares the same
 *     _broadcast_depth re-entrancy guard as broadcast<T>().
 *   * broadcast_async<T>() — asynchronous. Queues a typed event onto the qApp
 *     event loop via Qt::QueuedConnection, so worker threads (e.g. libsigrok
 *     data-feed callbacks) can emit typed events without touching QWidget from
 *     a non-GUI thread. The event is captured BY VALUE (copy) so it survives
 *     the caller's stack frame.
 *
 * SigSession is NOT a QObject, so broadcast_async queues on qApp which always
 * has a running event loop in both GUI and headless modes.
 */
class EventBus {
public:
    EventBus();
    ~EventBus();

    // ---- Listener registration ----
    // Spec v2 Task 7: add_callback/remove_callback removed (ISessionCallback abolished)
    void add_event_listener(interface::IEventListener *l);
    void remove_event_listener(interface::IEventListener *l);

    // ---- Listener queries ----
    bool has_listeners() const {
        std::shared_lock<std::shared_mutex> lk(_listeners_mutex);
        return !_event_listeners.empty();
    }

    // ---- Sync typed event broadcast ----
    // Synchronous dispatch to all registered IEventListener consumers. Called
    // from within the async-dispatched handler (or directly from the main
    // thread), so it stays sync and can't re-enter the caller.
    template <typename EventType> void broadcast(const EventType &ev) {
        std::shared_lock<std::shared_mutex> lk(_listeners_mutex);
        ++_broadcast_depth;
        if (_broadcast_depth > 1) {
            pxv_err("Event broadcast loop detected (depth=%d), suppressing",
                    _broadcast_depth);
            // 不触发 assert: EventBus 作为崩溃防线,自身不能成为模态弹窗的来源。
            // Windows 模态断言弹窗会接管 qApp 消息循环,把排队的 async 事件强行派发,
            // 造成 EventBus 自身被打穿。改为 early-return 即可阻断连锁。
            --_broadcast_depth;
            return;
        }
        for (auto *l : _event_listeners) {
            l->on_event(ev);
        }
        --_broadcast_depth;
    }

    // ---- Sync direct typed event broadcast (pre-broadcast ordering) ----
    // modernize-core-layer-radical Task 9: explicit synchronous dispatch for
    // the few pre/post ordering codes that MUST run synchronously BEFORE the
    // state mutation (e.g. CurrentDeviceChangePrev / StartCollectWorkPrev
    // / StoreConfPrev). Unlike broadcast(), this is intended to be called
    // directly from the mutating thread (not from within an async-dispatched
    // handler), so callers must guarantee they are on the main thread.
    // Shares the same thread_local _broadcast_depth re-entrancy guard as
    // broadcast().
    template <typename EventType> void broadcast_sync(const EventType &ev) {
        std::shared_lock<std::shared_mutex> lk(_listeners_mutex);
        ++_broadcast_depth;
        if (_broadcast_depth > 1) {
            pxv_err("Event broadcast_sync loop detected (depth=%d), suppressing",
                    _broadcast_depth);
            // 不触发 assert: EventBus 作为崩溃防线,自身不能成为模态弹窗的来源。
            // Windows 模态断言弹窗会接管 qApp 消息循环,把排队的 async 事件强行派发,
            // 造成 EventBus 自身被打穿。改为 early-return 即可阻断连锁。
            --_broadcast_depth;
            return;
        }
        for (auto *l : _event_listeners) {
            l->on_event(ev);
        }
        --_broadcast_depth;
    }

    // ---- Async typed event broadcast (worker-thread → main-thread) ----
    // CRITICAL FIX: Previously used QMetaObject::invokeMethod(qApp, lambda,
    // Qt::QueuedConnection) which internally calls QThread::currentThread(),
    // creating a QThreadData for the calling (worker) thread. When that thread
    // exits, Qt's DLL_THREAD_DETACH cleanup (LdrShutdownThread on Windows)
    // destroys the QThreadData, which can crash (SIGSEGV in Qt6Core.dll) due
    // to heap state accumulated during the capture session.
    //
    // The fix uses QCoreApplication::postEvent with a custom QEvent subclass
    // instead. postEvent() only accesses the *receiver's* QThreadData (qApp's,
    // which is the main thread), NOT the calling thread's QThreadData. This
    // avoids creating QThreadData on worker threads entirely.
    template <typename EventType> void broadcast_async(const EventType &ev) {
        // Capture event by value to avoid dangling references.
        // `this` is safe — EventBus outlives all worker threads (owned by
        // SigSession unique_ptr, destroyed after device threads join).
        post_async_dispatch([this, ev]() {
            broadcast(ev);
        });
    }

    // ---- Internal: post a functor to the main thread via QCoreApplication::postEvent ----
    // This avoids creating QThreadData on the calling thread (unlike
    // QMetaObject::invokeMethod), preventing Windows thread-exit crashes.
    // STATIC: can be called from anywhere without an EventBus instance —
    // transport/API layer uses this directly.
    static void post_async_dispatch(std::function<void()> fn);

    // Check if the current thread is the main (GUI) thread.
    // Uses std::this_thread::get_id() — NOT QThread::currentThread() —
    // to avoid creating a QThreadData on worker threads.
    static bool on_main_thread() {
        // If _main_thread_id hasn't been initialized yet (default-constructed),
        // return false as a safe fallback. This can happen if on_main_thread()
        // is called before EventBus is constructed.
        if (_main_thread_id == std::thread::id{})
            return false;
        return std::this_thread::get_id() == _main_thread_id;
    }

private:
    // Spec v2 Task 7: _callbacks and _callbacks_mutex removed (ISessionCallback abolished)
    std::vector<interface::IEventListener *> _event_listeners;
    mutable std::shared_mutex _listeners_mutex;
    static thread_local int _broadcast_depth;
    // Cached main thread ID — initialized in the EventBus constructor (NOT
    // via static initialization) to guarantee it's set on the main thread.
    static std::thread::id _main_thread_id;

    // Event filter installed on qApp to process custom async-dispatch events.
    // Owned via unique_ptr for automatic cleanup (Track B4).
    class AsyncEventFilter;
    std::unique_ptr<AsyncEventFilter> _async_filter;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_EVENTBUS_H
