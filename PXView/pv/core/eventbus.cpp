#include "pv/core/eventbus.h"

#include <algorithm>

namespace pv {
namespace core {

thread_local int EventBus::_broadcast_depth = 0;
std::thread::id EventBus::_main_thread_id;

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

class EventBus::AsyncEventFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() == AsyncDispatchEvent::eventType()) {
            auto *e = static_cast<AsyncDispatchEvent *>(event);
            if (e->fn) {
                e->fn();
            }
            return true;
        }
        return QObject::eventFilter(obj, event);
    }
};

EventBus::EventBus()
    : _alive_shared(std::make_shared<std::atomic<bool>>(true)) {
    _main_thread_id = std::this_thread::get_id();
    _async_filter = std::make_unique<AsyncEventFilter>();
    qApp->installEventFilter(_async_filter.get());
}

EventBus::~EventBus() {
    if (_alive_shared)
        _alive_shared->store(false, std::memory_order_release);
    if (_async_filter) {
        qApp->removeEventFilter(_async_filter.get());
        _async_filter.reset();
    }
}

void EventBus::post_async_dispatch(std::function<void()> fn) {
    QCoreApplication::postEvent(qApp, new AsyncDispatchEvent(std::move(fn)));
}

} // namespace core
} // namespace pv

