#ifndef PXVIEW_PV_SESSIONMANAGER_H
#define PXVIEW_PV_SESSIONMANAGER_H

#include <cstddef>
#include <vector>

namespace pv {

namespace view { class View; }
namespace data { class SessionDocument; }
namespace core { class DocumentRegistry; }

class SigSession;
class TabContext;

class SessionManager
{
public:
    static SessionManager* instance();

    // phase 2: doc_index + registry threaded through so TabContext can release
    // its document slot on destruction (ownership is in DocumentRegistry).
    TabContext* create_context(view::View *view, SigSession *session,
                               data::SessionDocument *doc, size_t doc_index,
                               core::DocumentRegistry *registry);
    void destroy_context(TabContext *ctx);

    void detach_context(TabContext *ctx);
    void attach_context(TabContext *ctx);

    void set_active_context(TabContext *ctx);
    TabContext* get_active_context();

    int context_count();
    TabContext* context_at(int index);

    void remove_from_main_list(TabContext *ctx);
    void move_context(int from, int to);

private:
    SessionManager();
    static SessionManager *_instance;

    std::vector<TabContext*> _contexts;
    std::vector<TabContext*> _detached_contexts;
    TabContext *_active_context;
};

} // namespace pv

#endif
