/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/core/ui_hooks.h"

#include "pv/base/log.h"
#include "pv/core/eventbus.h"

#include <condition_variable>
#include <mutex>

namespace pv {

namespace {
AskSaveFileFn g_ask_save_file = nullptr;
NotifyUserFn g_notify_user = nullptr;
DumpUiTreeFn g_dump_ui_tree = nullptr;
}

void set_ask_save_file_hook(AskSaveFileFn fn)
{
    g_ask_save_file = fn;
}

QString ask_save_file(const QString &caption, const QString &dir,
                      const QString &filter, QString *selected_filter)
{
    if (g_ask_save_file != nullptr) {
        return g_ask_save_file(caption, dir, filter, selected_filter);
    }
    // No UI bound (headless process): behave exactly like a cancelled dialog.
    return QString();
}

void set_notify_user_hook(NotifyUserFn fn)
{
    g_notify_user = fn;
}

void notify_user(const QString &message)
{
    if (g_notify_user != nullptr) {
        g_notify_user(message);
        return;
    }
    // No UI bound (headless process): the log is the user-facing surface.
    pxv_err("%s", message.toUtf8().constData());
}

void set_dump_ui_tree_hook(DumpUiTreeFn fn)
{
    g_dump_ui_tree = fn;
}

QString dump_ui_tree()
{
    if (g_dump_ui_tree == nullptr) {
        return QStringLiteral("{\"headless\":true,\"windows\":[]}");
    }
    if (pv::core::EventBus::on_main_thread()) {
        return g_dump_ui_tree();
    }
    QString result;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    pv::core::EventBus::post_async_dispatch([&]() {
        result = g_dump_ui_tree();
        {
            std::lock_guard<std::mutex> lock(m);
            done = true;
            cv.notify_one();
        }
    });
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&done]() { return done; });
    return result;
}

} // namespace pv
