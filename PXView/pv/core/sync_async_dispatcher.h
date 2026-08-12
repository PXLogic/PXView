#ifndef PXVIEW_CORE_SYNC_ASYNC_DISPATCHER_H
#define PXVIEW_CORE_SYNC_ASYNC_DISPATCHER_H

#include "pv/core/iasync_dispatcher.h"

namespace pv {
namespace core {

/**
 * SyncAsyncDispatcher — synchronous dispatcher for unit tests.
 *
 * Calls the functor inline on the calling thread instead of posting it
 * to an event loop.  Allows EventBus unit tests to run without a
 * QCoreApplication instance.
 */
class SyncAsyncDispatcher : public IAsyncDispatcher {
public:
    void post(std::function<void()> fn) override {
        if (fn) fn();
    }
    bool on_target_thread() const override {
        return true;
    }
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_SYNC_ASYNC_DISPATCHER_H
