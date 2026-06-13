/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#include "application.h"

#include <QMessageBox>
#ifndef NDEBUG
#include <QElapsedTimer>
#endif
#include <QEvent>
#include <QMetaObject>
#include <QPointer>
#ifndef NDEBUG
#include "pv/log.h"
#endif

Application::Application(int &argc, char **argv):
    QApplication(argc, argv)
{
}

bool Application::notify(QObject *receiver_, QEvent *event_)
{
    if (!receiver_ || !event_) {
        return QApplication::notify(receiver_, event_);
    }

#ifndef NDEBUG
    int type = event_->type();
    bool is_profile_event = (type == QEvent::Paint || type == QEvent::Resize || 
                             type == QEvent::LayoutRequest || type == QEvent::MouseMove || 
                             type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease);
    
    QElapsedTimer timer;
    if (is_profile_event) {
        timer.start();
    }
#endif

    try {
        QPointer<QObject> receiverGuard(receiver_);
        bool result = QApplication::notify(receiver_, event_);
        
#ifndef NDEBUG
        if (is_profile_event) {
            qint64 elapsed = timer.elapsed();
            if (elapsed > 0 && receiverGuard) {
                pxv_warn("[PROFILER] Receiver: %s (%s), EventType: %d, took %lld ms",
                         receiver_->objectName().isEmpty() ? "unnamed" : receiver_->objectName().toUtf8().constData(),
                         receiver_->metaObject()->className(),
                         type,
                         elapsed);
            }
        }
#endif
        return result;
    } catch ( std::exception& e ) {
        QMessageBox msg(NULL);
        msg.setText(tr("Application Error"));
        msg.setInformativeText(e.what());
        msg.setStandardButtons(QMessageBox::Ok);
        msg.setIcon(QMessageBox::Warning);
        msg.exec();
    } catch (...) {
        QMessageBox msg(NULL);
        msg.setText(tr("Application Error"));
        msg.setInformativeText(tr("An unexpected error occurred"));
        msg.setStandardButtons(QMessageBox::Ok);
        msg.setIcon(QMessageBox::Warning);
        msg.exec();
    }
    return false;
}
