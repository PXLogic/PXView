#include "sessionmanager.h"
#include "tabcontext.h"
#include "view/view.h"
#include "data/sessiondocument.h"
#include "sigsession.h"

namespace pv {

SessionManager* SessionManager::_instance = nullptr;

SessionManager::SessionManager()
    : _active_context(nullptr)
{
}

SessionManager* SessionManager::instance()
{
    if (!_instance)
        _instance = new SessionManager();
    return _instance;
}

TabContext* SessionManager::create_context(view::View *view, SigSession *session, data::SessionDocument *doc)
{
    TabContext *ctx = new TabContext(view, session, doc);
    _contexts.push_back(ctx);
    if (!_active_context)
        _active_context = ctx;
    return ctx;
}

void SessionManager::destroy_context(TabContext *ctx)
{
    if (!ctx)
        return;

    auto it = std::find(_contexts.begin(), _contexts.end(), ctx);
    if (it != _contexts.end())
        _contexts.erase(it);

    auto dit = std::find(_detached_contexts.begin(), _detached_contexts.end(), ctx);
    if (dit != _detached_contexts.end())
        _detached_contexts.erase(dit);

    if (_active_context == ctx)
        _active_context = nullptr;

    delete ctx;
}

void SessionManager::detach_context(TabContext *ctx)
{
    if (!ctx)
        return;

    auto it = std::find(_contexts.begin(), _contexts.end(), ctx);
    if (it != _contexts.end()) {
        _contexts.erase(it);
        _detached_contexts.push_back(ctx);
    }

    if (_active_context == ctx) {
        _active_context = _contexts.empty() ? nullptr : _contexts.front();
    }
}

void SessionManager::attach_context(TabContext *ctx)
{
    if (!ctx)
        return;

    auto it = std::find(_detached_contexts.begin(), _detached_contexts.end(), ctx);
    if (it != _detached_contexts.end()) {
        _detached_contexts.erase(it);
        _contexts.push_back(ctx);
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
        return _contexts[index];
    return nullptr;
}

void SessionManager::remove_from_main_list(TabContext *ctx)
{
    auto it = std::find(_contexts.begin(), _contexts.end(), ctx);
    if (it != _contexts.end())
        _contexts.erase(it);
}

void SessionManager::move_context(int from, int to)
{
    if (from < 0 || from >= (int)_contexts.size() || to < 0 || to >= (int)_contexts.size())
        return;
    if (from == to)
        return;

    TabContext* ctx = _contexts[from];
    _contexts.erase(_contexts.begin() + from);
    _contexts.insert(_contexts.begin() + to, ctx);
}

} // namespace pv
