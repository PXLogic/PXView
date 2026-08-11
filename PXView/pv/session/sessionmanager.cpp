#include "pv/session/sessionmanager.h"
#include "pv/session/tabcontext.h"
#include "pv/view/view.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/session/sigsession.h"

namespace pv {

std::unique_ptr<SessionManager> SessionManager::_instance;

SessionManager::SessionManager()
    : _active_context(nullptr)
{
}

SessionManager* SessionManager::instance()
{
    if (!_instance)
        _instance = std::unique_ptr<SessionManager>(new SessionManager());
    return _instance.get();
}

TabContext* SessionManager::create_context(view::View *view, SigSession *session,
                                           data::SessionDocument *doc, size_t doc_index,
                                           core::DocumentRegistry *registry)
{
    auto ctx = std::make_unique<TabContext>(view, session, doc, doc_index, registry);
    TabContext *raw = ctx.get();
    _contexts.push_back(std::move(ctx));
    if (!_active_context)
        _active_context = raw;
    return raw;
}

void SessionManager::destroy_context(TabContext *ctx)
{
    if (!ctx)
        return;

    auto it = std::find_if(_contexts.begin(), _contexts.end(),
        [ctx](const auto& up) { return up.get() == ctx; });
    if (it != _contexts.end())
        _contexts.erase(it);

    auto dit = std::find_if(_detached_contexts.begin(), _detached_contexts.end(),
        [ctx](const auto& up) { return up.get() == ctx; });
    if (dit != _detached_contexts.end())
        _detached_contexts.erase(dit);

    if (_active_context == ctx)
        _active_context = nullptr;

    // unique_ptr in vector deletes ctx on erase; no manual delete needed.
}

void SessionManager::detach_context(TabContext *ctx)
{
    if (!ctx)
        return;

    auto it = std::find_if(_contexts.begin(), _contexts.end(),
        [ctx](const auto& up) { return up.get() == ctx; });
    if (it != _contexts.end()) {
        _detached_contexts.push_back(std::move(*it));
        _contexts.erase(it);
    }

    if (_active_context == ctx) {
        _active_context = _contexts.empty() ? nullptr : _contexts.front().get();
    }
}

void SessionManager::attach_context(TabContext *ctx)
{
    if (!ctx)
        return;

    auto it = std::find_if(_detached_contexts.begin(), _detached_contexts.end(),
        [ctx](const auto& up) { return up.get() == ctx; });
    if (it != _detached_contexts.end()) {
        _contexts.push_back(std::move(*it));
        _detached_contexts.erase(it);
    }

    if (!_active_context)
        _active_context = ctx;
}

void SessionManager::set_active_context(TabContext *ctx)
{
    _active_context = ctx;
}

TabContext* SessionManager::get_active_context()
{
    return _active_context;
}

int SessionManager::context_count()
{
    return (int)_contexts.size();
}

TabContext* SessionManager::context_at(int index)
{
    if (index >= 0 && index < (int)_contexts.size())
        return _contexts[index].get();
    return nullptr;
}

void SessionManager::remove_from_main_list(TabContext *ctx)
{
    auto it = std::find_if(_contexts.begin(), _contexts.end(),
        [ctx](const auto& up) { return up.get() == ctx; });
    if (it != _contexts.end())
        _contexts.erase(it);
}

void SessionManager::move_context(int from, int to)
{
    if (from < 0 || from >= (int)_contexts.size() || to < 0 || to >= (int)_contexts.size())
        return;
    if (from == to)
        return;

    auto ctx = std::move(_contexts[from]);
    _contexts.erase(_contexts.begin() + from);
    _contexts.insert(_contexts.begin() + to, std::move(ctx));
}

} // namespace pv
