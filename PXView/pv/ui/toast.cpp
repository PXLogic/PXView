/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "toast.h"
#include <QLabel>
#include <QHBoxLayout>
#include <QTimer>
#include <QPropertyAnimation>
#include <QApplication>
#include <QPainter>
#include <QScreen>
#include <QIcon>
#include <QGraphicsOpacityEffect>
#include "../config/appconfig.h"
#include "iconcache.h"

namespace pv {
namespace ui {

Toast::Toast(QWidget *parent, const QString &text, Level level)
    : QWidget(parent)
{
    // Make it a plain child widget so it doesn't get any OS-level shadows
    // and doesn't exceed the application borders.
    // We raise() it to ensure it stays on top of other child widgets.
    setAttribute(Qt::WA_DeleteOnClose);

    // Make sure labels don't inherit the opaque background-color from global QWidget stylesheet
    setStyleSheet("QLabel { background-color: transparent; }");

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(20, 15, 20, 15);
    layout->setSpacing(10);

    // Icon
    _iconLabel = new QLabel(this);
    
    _textLabel = new QLabel(text, this);
    QColor textColor = AppConfig::Instance().GetThemeColor("@fg-base");
    if (!textColor.isValid()) textColor = Qt::white;
    _textLabel->setStyleSheet(QString("color: %1; font-size: 14px;").arg(textColor.name()));

    layout->addWidget(_iconLabel);
    layout->addWidget(_textLabel);
    
    _timer = new QTimer(this);
    _timer->setSingleShot(true);
    connect(_timer, &QTimer::timeout, this, &Toast::closeAnimation);

    updateContent(text, level);
}

void Toast::show(QWidget *parent, const QString &text, Level level)
{
    QWidget *w = parent;
    if (!w) {
        w = QApplication::activeWindow();
    }
    
    // Find top-level window to attach to as a child
    QWidget *topLevel = w ? w->window() : nullptr;
    if (!topLevel && w) topLevel = w;
    
    // Check if there is already a Toast showing
    if (topLevel) {
        QList<Toast*> existingToasts = topLevel->findChildren<Toast*>();
        if (!existingToasts.isEmpty()) {
            Toast *existing = existingToasts.first();
            existing->updateContent(text, level);
            existing->raise();
            return;
        }
    }
    
    Toast *toast = new Toast(topLevel, text, level);
    toast->adjustSize();
    
    // Position at bottom-right of the parent window
    if (topLevel) {
        QPoint p(topLevel->width() - toast->width() - 30, 
                 topLevel->height() - toast->height() - 30);
        toast->move(p);
    }

    toast->raise(); // Ensure it is drawn above other child widgets
    toast->showAnimation();
}

void Toast::updateContent(const QString &text, Level level)
{
    _textLabel->setText(text);
    
    if (level == Warning) {
        QIcon warnIcon = IconCache::Instance().icon(":/icons/status-warning.svg");
        if (!warnIcon.isNull()) {
            _iconLabel->setPixmap(warnIcon.pixmap(24, 24));
        } else {
            _iconLabel->setText("⚠️");
        }
    } else if (level == Error) {
        _iconLabel->clear();
        _iconLabel->setText("❌");
    } else {
        _iconLabel->clear();
        _iconLabel->setText("ℹ️");
    }
    
    adjustSize();
    
    // Check if close animation is running and stop it
    QGraphicsOpacityEffect *eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (eff) {
        // If it's already fading out, stop the animation and set opacity back to 1.0
        QList<QPropertyAnimation*> anims = eff->findChildren<QPropertyAnimation*>();
        for (QPropertyAnimation *anim : anims) {
            anim->stop();
        }
        eff->setOpacity(1.0);
    }
    
    // Restart the timer to refresh its existence time
    _timer->start(3500);
}

void Toast::showAnimation()
{
    QGraphicsOpacityEffect *eff = new QGraphicsOpacityEffect(this);
    this->setGraphicsEffect(eff);
    
    QWidget::show();
    
    QPropertyAnimation *anim = new QPropertyAnimation(eff, "opacity");
    anim->setDuration(300);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->start(QAbstractAnimation::DeleteWhenStopped);

    _timer->start(3500); // Wait 3.5 seconds before starting fade out
}

void Toast::closeAnimation()
{
    QGraphicsOpacityEffect *eff = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
    if (!eff) {
        close();
        return;
    }
    
    QPropertyAnimation *anim = new QPropertyAnimation(eff, "opacity");
    anim->setDuration(300);
    anim->setStartValue(1.0);
    anim->setEndValue(0.0);
    connect(anim, &QPropertyAnimation::finished, this, &QWidget::close);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void Toast::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    
    // Get theme-aware color
    QColor bgColor = AppConfig::Instance().GetThemeColor("@panel-bg");
    if (!bgColor.isValid()) {
        if (AppConfig::Instance().IsDarkStyle()) {
            bgColor = QColor(45, 45, 45, 240);
        } else {
            bgColor = QColor(30, 30, 30, 230); // Use dark bg even on light theme for contrast
        }
    }

    p.setBrush(bgColor);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(rect(), 8, 8);
}

} // namespace ui
} // namespace pv
