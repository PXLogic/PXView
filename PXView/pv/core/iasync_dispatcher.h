#ifndef PXVIEW_CORE_IASYNC_DISPATCHER_H
#define PXVIEW_CORE_IASYNC_DISPATCHER_H

#include <functional>

namespace pv {
namespace core {

/**
 * IAsyncDispatcher — abstract async dispatch strategy.
 *
 * EventBus::broadcast_async delegates to this interface to post a functor
 * to the target thread (usually the main/GUI thread).  The default
 * implementation is QtAsyncDispatcher (uses QCoreApplication::postEvent),
 * but it can be replaced with:
 *   - SyncAsyncDispatcher   (unit tests — calls functor inline)
 *   - Python GIL-safe dispatcher (pybind11 embedding)
 *   - Custom event-loop dispatcher (pure C / WASM)
 *
 * Thread-safety: post() may be called from any thread.
 */
class IAsyncDispatcher {
public:
    virtual ~IAsyncDispatcher() = default;

    /// Post fn to the target thread for asynchronous execution.
    /// Thread-safe: may be called from any thread.
    virtual void post(std::function<void()> fn) = 0;

    /// Check whether the calling thread is the target thread.
    /// If true, the caller may choose to execute synchronously instead.
    virtual bool on_target_thread() const = 0;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_IASYNC_DISPATCHER_H
