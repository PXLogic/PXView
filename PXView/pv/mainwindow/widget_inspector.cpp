/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/mainwindow/widget_inspector.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QScreen>
#include <QTabBar>
#include <QWidget>
#include <climits>
#include <vector>

#include "pv/view/viewport/viewport.h"
#include "pv/view/view.h"
#include "pv/view/trace/trace.h"
#include "pv/view/cursor/cursor.h"
#include "pv/session/sigsession.h"
#include "pv/config/appconfig.h"

namespace pv {
namespace mainwindow {

namespace {

QJsonObject serialize_widget(QWidget *w)
{
    if (!w) {
        return QJsonObject();
    }

    QJsonObject obj;
    obj["class"] = QString::fromLatin1(w->metaObject()->className());
    obj["name"] = w->objectName();
    obj["visible"] = w->isVisible();
    obj["enabled"] = w->isEnabled();
    obj["is_window"] = w->isWindow();

    QPoint global_pos(w->x(), w->y());
    if (w->isVisible()) {
        global_pos = w->mapToGlobal(QPoint(0, 0));
    } else if (w->parentWidget()) {
        global_pos = w->parentWidget()->mapToGlobal(QPoint(w->x(), w->y()));
    }

    QJsonObject rect;
    rect["x"] = w->x();
    rect["y"] = w->y();
    rect["w"] = w->width();
    rect["h"] = w->height();
    rect["gx"] = global_pos.x();
    rect["gy"] = global_pos.y();
    obj["rect"] = rect;

    if (!w->windowTitle().isEmpty()) {
        obj["title"] = w->windowTitle();
    }
    if (!w->toolTip().isEmpty()) {
        obj["tooltip"] = w->toolTip();
    }

    if (auto *btn = qobject_cast<const QAbstractButton *>(w)) {
        if (!btn->text().isEmpty()) {
            obj["text"] = btn->text();
        }
    } else if (auto *lbl = qobject_cast<const QLabel *>(w)) {
        if (!lbl->text().isEmpty()) {
            obj["text"] = lbl->text();
        }
    } else if (auto *edit = qobject_cast<const QLineEdit *>(w)) {
        if (!edit->text().isEmpty()) {
            obj["text"] = edit->text();
        } else if (!edit->placeholderText().isEmpty()) {
            obj["placeholder"] = edit->placeholderText();
        }
    } else if (auto *tab = qobject_cast<const QTabBar *>(w)) {
        if (tab->currentIndex() >= 0) {
            obj["text"] = tab->tabText(tab->currentIndex());
        }
    }

    if (w->layout()) {
        obj["layout"] = QString::fromLatin1(w->layout()->metaObject()->className());
    }

    QJsonArray children;
    for (QObject *child : w->children()) {
        if (auto *cw = qobject_cast<QWidget *>(child)) {
            children.append(serialize_widget(cw));
        }
    }

    // Inspect internal scene elements if w is pv::view::Viewport
    if (auto *vp = qobject_cast<pv::view::Viewport *>(w)) {
        pv::view::View &view = vp->view();
        const int vOffset = view.get_vOffset();
        const int vp_w = vp->width();
        const int vp_h = vp->height();

        obj["scale"] = view.scale();
        obj["offset"] = view.offset();
        obj["v_offset"] = vOffset;

        // 1. Virtual Traces (Logic, Analog, DSO, DecodeTrace, MathTrace, etc.)
        std::vector<pv::view::Trace *> traces;
        view.get_traces(vp->type(), traces);
        for (pv::view::Trace *t : traces) {
            if (!t) {
                continue;
            }
            QJsonObject tobj;
            QColor trace_color = t->get_colour();
            if (!trace_color.isValid()) {
                std::list<int> idx_list = t->get_index_list();
                int ch_idx = idx_list.empty() ? (t->get_index() % 8) : (*idx_list.begin() % 8);
                if (ch_idx < 0) ch_idx = 0;
                ch_idx = ch_idx % 8;
                QString token = QString("@logic-channel-%1").arg(ch_idx);
                trace_color = ::AppConfig::Instance().GetThemeColor(token);
                if (!trace_color.isValid()) {
                    trace_color = vp->probe_color(ch_idx);
                }
            }

            tobj["class"] = QString::fromLatin1(t->metaObject()->className());
            tobj["name"] = t->get_name();
            tobj["is_virtual"] = true;
            tobj["virtual_type"] = QStringLiteral("trace");
            tobj["color"] = trace_color.name();
            tobj["enabled"] = t->enabled();
            tobj["channel_index"] = t->get_index();

            const int y = t->get_y();
            const int total_h = t->get_totalHeight();
            const int local_y = (y != INT_MAX) ? (y - total_h / 2 - vOffset) : 0;
            const bool is_vis = t->visible() && (y != INT_MAX) &&
                                (local_y + total_h >= 0) && (local_y < vp_h);
            tobj["visible"] = is_vis;
            tobj["is_window"] = false;

            QJsonObject r;
            r["x"] = 0;
            r["y"] = local_y;
            r["w"] = vp_w;
            r["h"] = total_h;
            r["gx"] = global_pos.x();
            r["gy"] = global_pos.y() + local_y;
            tobj["rect"] = r;

            tobj["text"] = t->get_name();
            tobj["tooltip"] = QStringLiteral("通道: %1 (高度: %2, 布局Y: %3)")
                                 .arg(t->get_name())
                                 .arg(total_h)
                                 .arg(t->get_v_offset());

            children.append(tobj);
        }

        // 2. Virtual Cursors (Regular)
        const uint64_t samplerate = view.session().cur_samplerate();
        if (view.cursors_shown()) {
            int c_idx = 1;
            for (const auto &cursor : view.get_cursorList()) {
                if (!cursor) {
                    continue;
                }
                const int64_t cursorX = static_cast<int64_t>(view.index2pixel(cursor->index()));
                const bool is_vis = (cursorX >= 0 && cursorX <= vp_w);
                const double time_sec = (samplerate > 0) ? (static_cast<double>(cursor->index()) / samplerate) : 0.0;
                const QString time_formatted = view.format_real_time(cursor->index(), samplerate);

                QJsonObject cobj;
                cobj["class"] = QStringLiteral("pv::view::Cursor");
                cobj["name"] = QStringLiteral("Cursor %1").arg(c_idx);
                cobj["is_virtual"] = true;
                cobj["virtual_type"] = QStringLiteral("cursor");
                cobj["color"] = cursor->get_color().name();
                cobj["visible"] = is_vis;
                cobj["enabled"] = true;
                cobj["is_window"] = false;
                cobj["time"] = time_sec;
                cobj["time_formatted"] = time_formatted;
                cobj["index"] = static_cast<double>(cursor->index());

                QJsonObject r;
                r["x"] = static_cast<int>(cursorX);
                r["y"] = 0;
                r["w"] = 3;
                r["h"] = vp_h;
                r["gx"] = global_pos.x() + static_cast<int>(cursorX);
                r["gy"] = global_pos.y();
                cobj["rect"] = r;

                cobj["text"] = QStringLiteral("C%1: %2").arg(c_idx).arg(time_formatted);
                children.append(cobj);
                c_idx++;
            }
        }

        // 3. Virtual Trigger Cursor
        if (view.trig_cursor_shown() && view.get_trig_cursor()) {
            auto *tc = view.get_trig_cursor();
            const int64_t trigX = static_cast<int64_t>(view.index2pixel(tc->index()));
            const bool is_vis = (trigX >= 0 && trigX <= vp_w);
            const double trig_time_sec = (samplerate > 0) ? (static_cast<double>(tc->index()) / samplerate) : 0.0;
            const QString trig_time_formatted = view.format_real_time(tc->index(), samplerate);

            QJsonObject cobj;
            cobj["class"] = QStringLiteral("pv::view::TriggerCursor");
            cobj["name"] = QStringLiteral("Trigger Cursor");
            cobj["is_virtual"] = true;
            cobj["virtual_type"] = QStringLiteral("cursor");
            cobj["color"] = tc->get_color().name();
            cobj["visible"] = is_vis;
            cobj["enabled"] = true;
            cobj["is_window"] = false;
            cobj["time"] = trig_time_sec;
            cobj["time_formatted"] = trig_time_formatted;
            cobj["index"] = static_cast<double>(tc->index());

            QJsonObject r;
            r["x"] = static_cast<int>(trigX);
            r["y"] = 0;
            r["w"] = 3;
            r["h"] = vp_h;
            r["gx"] = global_pos.x() + static_cast<int>(trigX);
            r["gy"] = global_pos.y();
            cobj["rect"] = r;

            cobj["text"] = QStringLiteral("Trig: %1").arg(trig_time_formatted);
            children.append(cobj);
        }
    }

    if (!children.isEmpty()) {
        obj["children"] = children;
    }

    return obj;
}

} // namespace

QString dump_ui_tree_json()
{
    QJsonObject root;
    root["headless"] = false;
    root["timestamp"] = QDateTime::currentMSecsSinceEpoch();

    QJsonArray screens_arr;
    for (QScreen *screen : QGuiApplication::screens()) {
        if (!screen) {
            continue;
        }
        QJsonObject s_obj;
        s_obj["name"] = screen->name();
        s_obj["dpr"] = screen->devicePixelRatio();
        const QRect g = screen->geometry();
        s_obj["x"] = g.x();
        s_obj["y"] = g.y();
        s_obj["w"] = g.width();
        s_obj["h"] = g.height();
        screens_arr.append(s_obj);
    }
    root["screens"] = screens_arr;

    QJsonArray windows_arr;
    for (QWidget *top : QApplication::topLevelWidgets()) {
        if (top) {
            windows_arr.append(serialize_widget(top));
        }
    }
    root["windows"] = windows_arr;

    const QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

} // namespace mainwindow
} // namespace pv
