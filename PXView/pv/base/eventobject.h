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

// EventObject (14 Qt signals) has been REMOVED — it was a legacy notification
// system superseded by EventBus (pv/core/eventbus.h). All its signal
// connections in signal_connector.cpp were dead connections (never emitted).
//
// DeviceEventObject is retained because its device_updated() signal is still
// actively used by SigSession and event_dispatcher.cpp.

#ifndef _EVENT_OBJECT_H
#define _EVENT_OBJECT_H

#include <QObject>

class DeviceEventObject : public QObject
{
    Q_OBJECT

public:
    DeviceEventObject(); 


signals: 
    void device_updated();
};


#endif
