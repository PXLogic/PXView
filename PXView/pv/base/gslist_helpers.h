#pragma once

#include <glib.h>
#include <libsigrok/libsigrok.h>

#include <functional>
#include <vector>

namespace pv {

// Thin, allocation-free adapters over libsigrok's GSList so call sites stop
// hand-writing `for (GSList *l = list; l; l = l->next)` pointer arithmetic.
// libsigrok owns the list; these never free it. The element is cast to T* —
// callers that need null-safety keep their own `if (!x) return;` (or the
// original `if (!x) continue;`) inside the body, since some lists legitimately
// contain null nodes.

template <typename T, typename Fn>
inline void for_each_gslist(const GSList *list, Fn &&fn) {
    for (const GSList *l = list; l; l = l->next)
        fn(static_cast<T *>(l->data));
}

// Count nodes (no null filtering) — replaces `for (GSList *l = list; l; ...) n++`.
inline int count_gslist(const GSList *list) {
    int n = 0;
    for (const GSList *l = list; l; l = l->next)
        ++n;
    return n;
}

// Materialise a GSList of T* into a vector. Only for call sites that walk the
// same list several times and want a cache-friendly container (e.g. decoder
// channel enumeration). Allocates; keep off the hot path.
template <typename T>
inline std::vector<T *> gslist_to_vector(const GSList *list) {
    std::vector<T *> out;
    if (list)
        out.reserve(static_cast<size_t>(count_gslist(list)));
    for_each_gslist<T>(list, [&](T *p) { out.push_back(p); });
    return out;
}

}  // namespace pv
