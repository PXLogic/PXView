#include "pv/core/qt_async_dispatcher.h"

#include <QCoreApplication>

namespace pv {
namespace core {

// --- QtAsyncDispatcher::AsyncEvent ---

QEvent::Type QtAsyncDispatcher::AsyncEvent::eventType() {
    static QEvent::Type t = static_cast<QEvent::Type>(
        QEvent::registerEventType(QEvent::User + 0x100));
    return t;
}

QtAsyncDispatcher::AsyncEvent::AsyncEvent(std::function<void()> f)
    : QEvent(eventType()) {
    // Write the payload BEFORE publishing the ready flag so the consumer's
    // acquire-load (eventFilter / customEvent) synchronizes with this
    // release-store and sees a fully-constructed event.
    fn = std::move(f);
    ready.store(true, std::memory_order_release);
}

// --- QtAsyncDispatcher::EventFilter ---

class QtAsyncDispatcher::EventFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == AsyncEvent::eventType()) {
            auto *e = static_cast<AsyncEvent *>(event);
            // Acquire-load pairs with the release-store in the AsyncEvent
            // constructor: makes fn (and the QEvent payload) visible before
            // we read them. Silences the TSan false positives on the
            // worker-thread -> main-thread handoff (see AsyncEvent::ready).
            e->ready.load(std::memory_order_acquire);
            if (e->fn) {
                e->fn();
            }
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
};

// --- QtAsyncDispatcher ---

QtAsyncDispatcher::QtAsyncDispatcher()
    : _filter(std::make_unique<EventFilter>())
    , _target_thread_id(std::this_thread::get_id()) {
    qApp->installEventFilter(_filter.get());
}

QtAsyncDispatcher::~QtAsyncDispatcher() {
    // The static default dispatcher (EventBus::post_async_dispatch) is
    // destroyed via atexit AFTER main() returns, at which point the
    // QApplication/QCoreApplication stack object is already gone and
    // qApp is null.  Guard against it to avoid a null-deref SIGSEGV on exit.
    if (_filter && qApp) {
        qApp->removeEventFilter(_filter.get());
    }
}

void QtAsyncDispatcher::post(std::function<void()> fn) {
    if (!qApp) return;
    QCoreApplication::postEvent(qApp, new AsyncEvent(std::move(fn)));
}

void QtAsyncDispatcher::post_to(QObject* target, std::function<void()> fn) {
    if (!qApp || !target) return;
    QCoreApplication::postEvent(target, new AsyncEvent(std::move(fn)));
}

bool QtAsyncDispatcher::on_target_thread() const {
    return std::this_thread::get_id() == _target_thread_id;
}

} // namespace core
} // namespace pv
