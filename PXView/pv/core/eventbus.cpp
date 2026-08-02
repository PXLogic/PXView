#include "eventbus.h"

#include <algorithm>
#include <cassert>

namespace pv {
namespace core {

thread_local int EventBus::_broadcast_depth = 0;
std::thread::id EventBus::_main_thread_id = std::this_thread::get_id();

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

EventBus::EventBus() {
    _async_filter = new AsyncEventFilter();
    qApp->installEventFilter(_async_filter);
}

EventBus::~EventBus() {
    if (_async_filter) {
        qApp->removeEventFilter(_async_filter);
        delete _async_filter;
        _async_filter = nullptr;
    }
}

void EventBus::post_async_dispatch(std::function<void()> fn) {
    // Post a custom event to qApp. QCoreApplication::postEvent accesses
    // only the receiver's (qApp's) QThreadData, NOT the calling thread's.
    // This is the key fix: no QThreadData is created on worker threads.
    QCoreApplication::postEvent(qApp, new AsyncDispatchEvent(std::move(fn)));
}

void EventBus::add_callback(ISessionCallbackBase *cb) {
    if (cb)
        _callbacks.push_back(cb);
}

void EventBus::remove_callback(ISessionCallbackBase *cb) {
    auto it = std::find(_callbacks.begin(), _callbacks.end(), cb);
    if (it != _callbacks.end())
        _callbacks.erase(it);
}

void EventBus::add_event_listener(interface::IEventListener *l) {
    if (!l)
        return;
    if (std::find(_event_listeners.begin(), _event_listeners.end(), l) !=
        _event_listeners.end())
        return;
    _event_listeners.push_back(l);
}

void EventBus::remove_event_listener(interface::IEventListener *l) {
    auto it = std::find(_event_listeners.begin(), _event_listeners.end(), l);
    if (it != _event_listeners.end())
        _event_listeners.erase(it);
}

} // namespace core
} // namespace pv
