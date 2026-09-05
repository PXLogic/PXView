/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QString>

namespace pv {

/// Optional UI services the Core layer may need, injected by the GUI binary at
/// startup (see main.cpp). Headless builds (pxviewd) never install these, so
/// every hook returns its documented "no UI" result and callers must handle
/// that the same way they handle a user cancelling the dialog.
///
/// This is the seam that keeps Core free of QtWidgets includes while still
/// letting Core-initiated flows (e.g. StoreSession::MakeSessionFile) ask the
/// user for a file name in GUI mode.

/// Ask the user for a save-file name. The GUI binds
/// QFileDialog::getSaveFileName(parent=nullptr, ...).
///
/// @return chosen file name, or an empty string when no UI is bound or the
///         user cancelled.
using AskSaveFileFn = QString (*)(const QString &caption,
                                  const QString &dir,
                                  const QString &filter,
                                  QString *selected_filter);

void set_ask_save_file_hook(AskSaveFileFn fn);

QString ask_save_file(const QString &caption, const QString &dir,
                      const QString &filter, QString *selected_filter);

/// Report a non-fatal problem to the user. The GUI binds MsgBox::Show (a
/// modal message box); headless builds leave it unbound and the message is
/// written to the log at error level instead.
using NotifyUserFn = void (*)(const QString &message);

void set_notify_user_hook(NotifyUserFn fn);

void notify_user(const QString &message);

} // namespace pv
