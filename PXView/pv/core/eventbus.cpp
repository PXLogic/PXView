#include "pv/core/eventbus.h"

#include <algorithm>
#include <cassert>

namespace pv {
namespace core {

thread_local int EventBus::_broadcast_depth = 0;
// _main_thread_id is default-initialized (std::thread::id{}) and set in
// the EventBus constructor to ensure it captures the main thread ID.
std::thread::id EventBus::_main_thread_id;

// ---------------------------------------------------------------------------
// Custom QEvent subclass for async dispatch.
//
// QCoreApplication::postEvent only accesses the *receiver's* QThreadData,
// NOT the calling thread's QThreadData. This is the critical difference
// from QMetaObject::invokeMethod, which calls QThread::currentThread()
// internally and creates a QThreadData for the calling thread.
//
// On Windows, when a worker thread (e.g. libsigrok session thread, decoder
// thread) that has a QThreadData exits, the DLL_THREAD_DETACH handler in
// Qt6Core.dll destroys the QThreadData. This destruction can crash
// (SIGSEGV) due to accumulated heap state or event dispatcher issues.
//
// By using postEvent with this custom event type, we ensure that worker
// threads never create a QThreadData, and thus the thread-exit cleanup
// is safe.
// ---------------------------------------------------------------------------

class AsyncDispatchEvent : public QEvent {
public:
    static QEvent::Type eventType() {
        static QEvent::Type t = static_cast<QEvent::Type>(
            QEvent::registerEventType(QEvent::User + 0x100));
        return t;
    }

    std::function<void()> fn;

    explicit AsyncDispatchEvent(std::function<void()> f)
        : QEvent(eventType()), fn(std::move(f)) {}
};

// ---------------------------------------------------------------------------
// Event filter installed on qApp to process AsyncDispatchEvent events.
//
// This class overrides QObject::eventFilter without Q_OBJECT. This is safe
// because eventFilter is a virtual function override — Q_OBJECT is only
// needed for signals, slots, and properties, none of which are used here.
// ---------------------------------------------------------------------------

class EventBus::AsyncEventFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == AsyncDispatchEvent::eventType()) {
            auto *e = static_cast<AsyncDispatchEvent *>(event);
            if (e->fn) {
                e->fn();
            }
            return true; // event handled
        }
        return QObject::eventFilter(obj, event);
    }
};

EventBus::EventBus()
    // TS-1 fix: allocate the shared alive flag so posted lambdas can
    // safely check it without accessing 'this' after destruction.
    : _alive_shared(std::make_shared<std::atomic<bool>>(true)) {
    // Track A7: Set _main_thread_id in the constructor (NOT static init)
    // to guarantee it captures the main thread's ID.
    _main_thread_id = std::this_thread::get_id();
    _async_filter = std::make_unique<AsyncEventFilter>();
    qApp->installEventFilter(_async_filter.get());
}

EventBus::~EventBus() {
    // TS-1 fix: set the shared alive flag to false with release ordering so
    // that any acquire-load in a posted lambda sees false after this point.
    // The shared_ptr keeps the atomic flag alive even after the EventBus is
    // destroyed, so the lambda's load is always well-defined.
    if (_alive_shared)
        _alive_shared->store(false, std::memory_order_release);
    if (_async_filter) {
        qApp->removeEventFilter(_async_filter.get());
        _async_filter.reset();
    }
}

void EventBus::post_async_dispatch(std::function<void()> fn) {
    // Post a custom event to qApp. QCoreApplication::postEvent accesses
    // only the receiver's (qApp's) QThreadData, NOT the calling thread's.
    // This is the key fix: no QThreadData is created on worker threads.
    QCoreApplication::postEvent(qApp, new AsyncDispatchEvent(std::move(fn)));
}

// Spec v2 Task 7: add_callback/remove_callback removed (ISessionCallback abolished)

void EventBus::add_event_listener(interface::IEventListener *l) {
    if (!l)
        return;
    std::unique_lock<std::shared_mutex> lk(_listeners_mutex);
    if (std::find(_event_listeners.begin(), _event_listeners.end(), l) !=
        _event_listeners.end())
        return;
    _event_listeners.push_back(l);
}

void EventBus::remove_event_listener(interface::IEventListener *l) {
    std::unique_lock<std::shared_mutex> lk(_listeners_mutex);
    auto it = std::find(_event_listeners.begin(), _event_listeners.end(), l);
    if (it != _event_listeners.end())
        _event_listeners.erase(it);
}

} // namespace core
} // namespace pv
