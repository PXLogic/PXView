
/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef _I_CALLBACKS_
#define _I_CALLBACKS_

#include <QString>
#include <cstdint>
#include <string>

// Common base for all session callback sub-interfaces.
// Provides a virtual destructor so that dynamic_cast can be used to query
// at runtime which sub-interfaces a registered callback implements.
//
// All sub-interfaces below inherit *virtually* from ISessionCallbackBase so
// that a single concrete class (e.g. MainWindow) can implement multiple
// sub-interfaces without creating an ambiguous diamond inheritance.
class ISessionCallbackBase
{
public:
    virtual ~ISessionCallbackBase() = default;
};

// Data-feed callbacks: invoked when captured sample data or samplerate
// metadata changes.
class IDataCallback : public virtual ISessionCallbackBase
{
public:
    virtual void data_updated() = 0;
    virtual void receive_data_len(quint64 len) = 0;
    virtual void receive_header() = 0;
    virtual void cur_snap_samplerate_changed() = 0;
};

// Capture lifecycle callbacks: invoked on frame boundaries and capture
// state changes (including region selection and repeat-hold progress).
class ICaptureCallback : public virtual ISessionCallbackBase
{
public:
    virtual void frame_began() = 0;
    virtual void frame_ended() = 0;
    virtual void update_capture() = 0;
    virtual void show_region(uint64_t start, uint64_t end, bool keep) = 0;
    virtual void repeat_hold(int percent) = 0;
    virtual void cur_samplelimits_changed() {}
};

// Trigger callbacks: invoked when a trigger fires or when a trigger-related
// message should be propagated to listeners.
class ITriggerCallback : public virtual ISessionCallbackBase
{
public:
    virtual void receive_trigger(quint64 trigger_pos) = 0;
    virtual void show_wait_trigger() = 0;
};

// Session state callbacks: invoked on session-wide events such as errors,
// save requests, signal list changes, decode completion and deferred
// UI messages.
class ISessionStateCallback : public virtual ISessionCallbackBase
{
public:
    virtual void session_error() = 0;
    virtual void session_save() = 0;
    virtual void signals_changed() = 0;
    virtual void decode_done() = 0;
    virtual void delay_prop_msg(QString strMsg) = 0;
};

class ISessionDataGetter
{
public:
    virtual bool genSessionData(std::string &str) = 0;
};


class IDlgCallback
{
public:
    virtual void OnDlgResult(bool bYes)=0;
};

class IMainForm{
public:
    virtual void switchLanguage(int language)=0;
};


class IDecoderPannel
{
public:
    virtual void update_deocder_item_name(void *trace_handel, const char *name)=0;
    virtual void rebuild_layers()=0;
};

enum ParentNativeEvent
{
    PARENT_EVENT_DISPLAY_CHANGED = 0,
};

class IParentNativeEventCallback
{
public:
    virtual void OnParentNativeEvent(ParentNativeEvent msg)=0;
};

#endif
