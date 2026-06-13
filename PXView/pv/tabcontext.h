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
#include <stdint.h>

namespace pv {

namespace view {
class View;
}

namespace data {
class SessionDocument;
}

class SigSession;

class TabContext
{
public:
    enum State {
        LIVE,
        HISTORICAL
    };

    TabContext(view::View *view, SigSession *session, data::SessionDocument *doc);
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

    void make_live();
    void activate();
    void deactivate();

    static int _next_session_id;

private:
    view::View              *_view;
    SigSession              *_session;
    data::SessionDocument   *_document;
    QString                 _title;
    QString                 _file_path;
    State                   _state;
    QDateTime               _timestamp;
};

} // namespace pv

#endif
