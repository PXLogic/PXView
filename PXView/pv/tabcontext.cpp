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

#include "tabcontext.h"
#include "sigsession.h"
#include "view/view.h"
#include "view/signal.h"
#include "data/sessiondocument.h"
#include "deviceagent.h"
#include "log.h"
#include <QDebug>

namespace pv {

int TabContext::_next_session_id = 1;

TabContext::TabContext(view::View *view, SigSession *session, data::SessionDocument *doc) :
    _view(view),
    _session(session),
    _document(doc),
    _title(QString("Session %1").arg(_next_session_id)),
    _file_path(""),
    _state(LIVE),
    _timestamp(QDateTime::currentDateTime())
{
    _next_session_id++;
}

TabContext::~TabContext()
{
    if (_document)
        delete _document;
}

void TabContext::make_live()
{
    _state = LIVE;
}

bool TabContext::has_data()
{
    return _document && _document->has_data();
}

void TabContext::activate()
{
    fprintf(stderr, "DBG TabContext::activate() doc=%p has_config=%d has_data=%d is_working=%d\n",
        _document,
        _document ? _document->has_signal_config() : 0,
        _document ? _document->has_data() : 0,
        _session->is_working());
    fflush(stderr);

    _session->set_active_document(_document);
    _state = LIVE;
    if (_document && _document->has_signal_config()) {
        if (!_session->is_working()) {
            pxv_info("TabContext::activate() applying signal config, work_mode=%d ch_count=%d",
                _document->get_signal_config().work_mode,
                (int)_document->get_signal_config().channels.size());
            _document->apply_signal_config(_session->get_device());
            _session->reload();
        } else {
            pxv_info("TabContext::activate() session working, saving pending config");
            _document->_pending_device_config = _document->_signal_config;
        }
        _view->rebuild_signals_from_config(_document->get_signal_config());
        pxv_info("TabContext::activate() rebuild_signals_from_config done, own_signals=%d",
            (int)_view->get_own_signals().size());
    }
    if (_document && _document->has_data()) {
        _view->set_data_document(_document);
        auto &sigs = _view->get_own_signals();
        for (auto sig : sigs) {
            auto s = dynamic_cast<view::Signal*>(sig);
            if (s && s->probe()) {
                const_cast<sr_channel*>(s->probe())->enabled = s->enabled();
            }
        }
    } else if (_session->have_view_data() &&
               (_session->is_working() || _session->is_copy_in_progress()) &&
               _session->get_capture_owner_document() == _document) {
        // Document has no data yet, but session has data.
        // Bind signals to session data instead of clearing them,
        // so waveforms remain visible during active capture or background copy.
        _view->set_signal_data_from_source(_session);
    } else {
        pxv_info("TabContext::activate() no data, clearing signal data bindings");
        _view->clear_signal_data();
    }
    _view->update_scale_offset();
    _view->signals_changed(nullptr);
    pxv_info("TabContext::activate() completed");
}

void TabContext::deactivate()
{
    pxv_info("TabContext::deactivate() doc=%p", _document);
    if (_document) {
        _document->save_signal_config(_session->get_device());
    }
    _state = HISTORICAL;
}

} // namespace pv
