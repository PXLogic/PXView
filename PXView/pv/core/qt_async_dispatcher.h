#ifndef PXVIEW_CORE_QT_ASYNC_DISPATCHER_H
#define PXVIEW_CORE_QT_ASYNC_DISPATCHER_H

#include "pv/core/iasync_dispatcher.h"

#include <QEvent>
#include <QObject>
#include <memory>
#include <thread>

namespace pv {
namespace core {

/**
 * QtAsyncDispatcher — default IAsyncDispatcher implementation.
 *
 * Wraps the functor in a custom QEvent and posts it to qApp via
 * QCoreApplication::postEvent.  An event filter installed on qApp
 * intercepts the custom event and invokes the functor on the main thread.
 *
 * Behaviour is identical to the former EventBus::post_async_dispatch
 * static method (same QEvent::User + 0x100 event type, same filter
 * mechanism).  This is a mechanical extraction — no behavioural change.
 */
class QtAsyncDispatcher : public IAsyncDispatcher {
public:
    QtAsyncDispatcher();
    ~QtAsyncDispatcher() override;

    void post(std::function<void()> fn) override;
    bool on_target_thread() const override;

    // Public so that arbitrary QObject targets can handle AsyncEvent in
    // their customEvent() override when posted via post_to().
    class AsyncEvent : public QEvent {
    public:
        static QEvent::Type eventType();

        std::function<void()> fn;

        explicit AsyncEvent(std::function<void()> f);
    };

    // Post a functor to a specific QObject's thread. Uses
    // QCoreApplication::postEvent(target, ...) which only accesses the
    // receiver's QThreadData — safe to call from any thread (no
    // QThread::currentThread() / QMetaObject::invokeMethod SIGSEGV risk).
    // The target must handle AsyncEvent in its customEvent() override.
    static void post_to(QObject* target, std::function<void()> fn);

private:
    class EventFilter;
    std::unique_ptr<EventFilter> _filter;
    std::thread::id _target_thread_id;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_CORE_QT_ASYNC_DISPATCHER_H
