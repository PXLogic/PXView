
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

// Spec v2 Task 7: ISessionCallbackBase / IDataCallback / ICaptureCallback /
// ITriggerCallback / ISessionStateCallback have been REMOVED. All session
// notifications now go through the typed EventBus (broadcast_async<T> /
// broadcast<T> / broadcast_sync<T>) with IEventListener. The remaining
// interfaces below are NOT notification callbacks — they are functional
// interfaces (data getter, dialog result, main form, decoder panel,
// native event) and are kept.

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
