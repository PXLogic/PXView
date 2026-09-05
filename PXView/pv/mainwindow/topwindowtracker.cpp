/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/mainwindow/topwindowtracker.h"

#include <QWidget>

TopWindowTracker::TopWindowTracker()
{
    _topWindow = nullptr;
}

TopWindowTracker::~TopWindowTracker()
{
}

TopWindowTracker::TopWindowTracker(TopWindowTracker &o)
{
    (void)o;
}

TopWindowTracker* TopWindowTracker::Instance()
{
    static TopWindowTracker *ins = nullptr;
    if (ins == nullptr){
        ins = new TopWindowTracker();
    }
    return ins;
}

void TopWindowTracker::Destroy()
{
    // The tracker does not own the window it points at, and the singleton is
    // intentionally kept alive for the process lifetime (same contract as
    // AppControl::Instance()). Nothing to release here.
}

bool TopWindowTracker::TopWindowIsMaximized()
{
    if (_topWindow != nullptr){
        return _topWindow->isMaximized();
    }
    return false;
}
