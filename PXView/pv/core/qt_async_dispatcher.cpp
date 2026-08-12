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
    : QEvent(eventType()), fn(std::move(f)) {}

// --- QtAsyncDispatcher::EventFilter ---

class QtAsyncDispatcher::EventFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == AsyncEvent::eventType()) {
            auto *e = static_cast<AsyncEvent *>(event);
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
    if (_filter) {
        qApp->removeEventFilter(_filter.get());
    }
}

void QtAsyncDispatcher::post(std::function<void()> fn) {
    QCoreApplication::postEvent(qApp, new AsyncEvent(std::move(fn)));
}

void QtAsyncDispatcher::post_to(QObject* target, std::function<void()> fn) {
    QCoreApplication::postEvent(target, new AsyncEvent(std::move(fn)));
}

bool QtAsyncDispatcher::on_target_thread() const {
    return std::this_thread::get_id() == _target_thread_id;
}

} // namespace core
} // namespace pv
