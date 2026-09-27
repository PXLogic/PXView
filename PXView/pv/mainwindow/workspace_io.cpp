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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "pv/mainwindow/workspace_io.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>

#include "pv/base/log.h"
#include "pv/config/appconfig.h"

namespace pv {

namespace {
constexpr const char *kFileTag = "workspace";

// Test hook: non-empty → used instead of the GetProfileDir() location.
QString g_workspace_path_override;
} // namespace

void set_workspace_path_override(const QString &path) {
  g_workspace_path_override = path;
  pxv_info("%s: path override %s", kFileTag,
           path.isEmpty() ? "(cleared)" : path.toUtf8().constData());
}

QString workspace_file_path() {
  if (!g_workspace_path_override.isEmpty())
    return g_workspace_path_override;

  const QString dir = GetProfileDir();
  if (dir.isEmpty())
    return QString();
  QDir d(dir);
  if (!d.exists())
    d.mkpath(".");
  return d.absolutePath() + "/workspace.json";
}

bool write_workspace_file(const Workspace &ws) {
  const QString path = workspace_file_path();
  if (path.isEmpty()) {
    pxv_warn("%s: profile dir unavailable, skipping workspace save", kFileTag);
    return false;
  }

  QJsonObject root;
  root["Version"] = Workspace::kVersion;
  root["activeTab"] = ws.activeTab;

  QJsonArray tabs;
  for (const WorkspaceTab &t : ws.tabs) {
    QJsonObject o;
    o["title"] = t.title;
    o["filePath"] = t.filePath;
    QJsonObject dev;
    dev["driver"] = t.driver;
    dev["connid"] = t.connid;
    dev["workMode"] = t.workMode;
    dev["isFileDevice"] = t.isFileDevice;
    o["device"] = dev;
    o["session"] = t.session;
    tabs.append(o);
  }
  root["tabs"] = tabs;

  // QSaveFile writes to a temp file and renames on commit(), so a crash or a
  // full disk can never leave a half-written workspace behind.
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
    pxv_warn("%s: cannot open '%s' for write", kFileTag,
             path.toUtf8().constData());
    return false;
  }
  const QByteArray payload = QJsonDocument(root).toJson();
  if (f.write(payload) != payload.size() || !f.commit()) {
    pxv_warn("%s: write/commit failed for '%s'", kFileTag,
             path.toUtf8().constData());
    return false;
  }

  pxv_info("%s: wrote %d tabs (active=%d) to '%s'", kFileTag,
           static_cast<int>(ws.tabs.size()), ws.activeTab,
           path.toUtf8().constData());
  return true;
}

bool read_workspace_file(Workspace &out) {
  out = Workspace();

  const QString path = workspace_file_path();
  if (path.isEmpty())
    return false;

  QFile f(path);
  if (!f.exists()) {
    pxv_info("%s: no workspace file, using default single-tab startup",
             kFileTag);
    return false;
  }
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    pxv_warn("%s: cannot open '%s' for read", kFileTag,
             path.toUtf8().constData());
    return false;
  }

  QJsonParseError err{};
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  f.close();
  if (err.error != QJsonParseError::NoError || !doc.isObject()) {
    pxv_warn("%s: parse error in '%s' (%s), falling back to single tab",
             kFileTag, path.toUtf8().constData(),
             err.errorString().toUtf8().constData());
    return false;
  }

  const QJsonObject root = doc.object();
  const int version = root.value("Version").toInt(0);
  if (version <= 0 || version > Workspace::kVersion) {
    pxv_warn("%s: unsupported version %d (supported <= %d), ignoring", kFileTag,
             version, Workspace::kVersion);
    return false;
  }

  out.version = version;
  out.activeTab = root.value("activeTab").toInt(0);

  const QJsonArray tabs = root.value("tabs").toArray();
  for (const QJsonValue &v : tabs) {
    const QJsonObject o = v.toObject();
    // A tab without a session object is useless — skip rather than create an
    // empty tab that would confuse the user.
    if (!o.value("session").isObject())
      continue;

    WorkspaceTab t;
    t.title = o.value("title").toString();
    t.filePath = o.value("filePath").toString();
    const QJsonObject dev = o.value("device").toObject();
    t.driver = dev.value("driver").toString();
    t.connid = dev.value("connid").toString();
    t.workMode = dev.value("workMode").toInt(0);
    t.isFileDevice = dev.value("isFileDevice").toBool(false);
    t.session = o.value("session").toObject();
    out.tabs.push_back(t);
  }

  if (out.tabs.empty()) {
    pxv_warn("%s: no usable tab entries in '%s', ignoring", kFileTag,
             path.toUtf8().constData());
    return false;
  }

  pxv_info("%s: loaded %d tabs (active=%d) from '%s'", kFileTag,
           static_cast<int>(out.tabs.size()), out.activeTab,
           path.toUtf8().constData());
  return true;
}

} // namespace pv
