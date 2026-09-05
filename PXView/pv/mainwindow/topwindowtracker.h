/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QRect>

class QWidget;

// View-layer owner of the top-level window pointer and the screen geometry.
//
// These two members used to live on AppControl. Because they are QWidget/QScreen
// concerns, their presence forced AppControl -- and with it the entire headless
// build -- to link Qt Widgets. Moving them here lets AppControl sit in the Core
// layer (see CMake/core_sources.cmake), which is what makes the console
// pxviewd binary possible without pulling Widgets into a GUI-less process.
//
// This class is the View side of that split: it is only ever touched by GUI
// code (MainFrame, MsgBox, TitleBar, WinNativeWidget).
class TopWindowTracker
{
private:
    explicit TopWindowTracker();
    ~TopWindowTracker();
    TopWindowTracker(TopWindowTracker &o);

public:
    static TopWindowTracker* Instance();

    void Destroy();

    inline void SetTopWindow(QWidget *w){
        _topWindow = w;
    }

    inline QWidget* GetTopWindow(){
        return _topWindow;
    }

    bool TopWindowIsMaximized();

public:
    /// Available geometry of the screen the top window lives on.
    /// Written by WinNativeWidget, read by TitleBar.
    QRect screenRect;

private:
    QWidget *_topWindow;
};
