/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Boston, MA  02110-1301 USA
 */

#ifndef PXVIEW_PV_TABCONTEXT_H
#define PXVIEW_PV_TABCONTEXT_H

#include <QString>
#include <QDateTime>
#include <cstddef>
#include <cstdint>

#include "pv/base/pxvdef.h"   // ds_device_handle / NULL_HANDLE

namespace pv {

namespace view {
class View;
}

namespace data {
class SessionDocument;
}

namespace core {
class DocumentRegistry;
}

class SigSession;

class TabContext
{
public:
    enum State {
        LIVE,
        HISTORICAL
    };

    // modernize-core-layer-radical phase 2: TabContext now holds a WEAK
    // reference to the document (doc) plus its owning index and registry.
    // The document is owned by DocumentRegistry; TabContext::~TabContext
    // calls registry->release_document(doc_index) instead of delete.
    TabContext(view::View *view, SigSession *session, data::SessionDocument *doc,
               size_t doc_index, core::DocumentRegistry *registry);
    ~TabContext();

    inline view::View* view() { return _view; }
    inline void set_view(view::View *v) { _view = v; }
    inline data::SessionDocument* document() { return _document; }
    inline SigSession* session() { return _session; }
    inline QString title() const { return _title; }
    inline QString file_path() const { return _file_path; }
    inline State state() const { return _state; }
    inline bool is_live() const { return _state == LIVE; }
    bool has_data();
    inline QDateTime timestamp() const { return _timestamp; }

    inline void set_title(const QString &title) { _title = title; }
    inline void set_file_path(const QString &path) { _file_path = path; }

    // The device this tab's data came from. NULL_HANDLE for tabs that have
    // never been bound to a device (e.g. a fresh empty tab before any capture).
    //
    // File tabs (.pxl / imported VCD/CSV/...) MUST remember their virtual
    // device: the global DeviceAgent can only hold ONE active device, so
    // tabbing away switches it. Without this handle the tab can never get its
    // device back — its sr_channels (channel names / enabled / types) are gone
    // and the tab silently degrades to whatever device happens to be active
    // (typically Demo).
    inline ds_device_handle device_handle() const { return _device_handle; }
    inline void set_device_handle(ds_device_handle h) { _device_handle = h; }

    void make_live();
    void activate();
    void deactivate();

    static int _next_session_id;

private:
    view::View              *_view;
    SigSession              *_session;
    data::SessionDocument   *_document;   // weak reference (owned by DocumentRegistry)
    size_t                  _doc_index;   // owning index in DocumentRegistry
    core::DocumentRegistry  *_doc_registry; // owner of the document
    QString                 _title;
    QString                 _file_path;
    State                   _state;
    QDateTime               _timestamp;
    ds_device_handle        _device_handle = NULL_HANDLE;
};

} // namespace pv

#endif
