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

#ifndef TOAST_H
#define TOAST_H

#include <QWidget>
#include <QString>

class QTimer;
class QLabel;

namespace pv {
namespace ui {

class Toast : public QWidget
{
    Q_OBJECT
public:
    enum Level { Info, Warning, Error };
    static void show(QWidget *parent, const QString &text, Level level = Info);
    void updateContent(const QString &text, Level level);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Toast(QWidget *parent, const QString &text, Level level);
    void showAnimation();
    void closeAnimation();

private:
    QLabel *_iconLabel;
    QLabel *_textLabel;
    QTimer *_timer;
};

} // namespace ui
} // namespace pv

#endif // TOAST_H
