#include "pv/core/eventbus.h"

#include "pv/core/qt_async_dispatcher.h"

#include <algorithm>

namespace pv {
namespace core {

thread_local int EventBus::_broadcast_depth = 0;
std::thread::id EventBus::_main_thread_id;

EventBus::EventBus(std::unique_ptr<IAsyncDispatcher> dispatcher)
    : _alive_shared(std::make_shared<std::atomic<bool>>(true))
    , _dispatcher(dispatcher ? std::shared_ptr<IAsyncDispatcher>(std::move(dispatcher))
                             : std::make_shared<QtAsyncDispatcher>()) {
    _main_thread_id = std::this_thread::get_id();
}

EventBus::~EventBus() {
    if (_alive_shared)
        _alive_shared->store(false, std::memory_order_release);
    // _dispatcher destructor cleans up the Qt event filter (if QtAsyncDispatcher).
}

void EventBus::dispatch_async(std::function<void()> fn) {
    // Shared-ownership convenience: keep the dispatcher alive via a local
    // copy so the posted functor stays safe even if the EventBus is destroyed.
    std::shared_ptr<IAsyncDispatcher> d = _dispatcher;
    d->post(std::move(fn));
}

void EventBus::dispatch_async(std::shared_ptr<IAsyncDispatcher> disp,
                              std::function<void()> fn) {
    disp->post(std::move(fn));
}

// Static convenience — uses a global default QtAsyncDispatcher singleton.
// External callers (SessionService, WsTransport, McpTransport,
// DeviceOptionsDock) call this to post functors to the main thread without
// holding an EventBus instance.
void EventBus::post_async_dispatch(std::function<void()> fn) {
    // Function-local static — created on first call, destroyed at program exit.
    // This is thread-safe in C++11+ (magic statics).
    static auto s_default_dispatcher = std::make_unique<QtAsyncDispatcher>();
    s_default_dispatcher->post(std::move(fn));
}

} // namespace core
} // namespace pv
