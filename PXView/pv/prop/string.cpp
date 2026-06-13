/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#include <assert.h>

#include <QLineEdit>
#include <QSpinBox>

#include <QFileDialog>
#include <QHBoxLayout>
#include <QToolButton>

#include "../config/appconfig.h"
#include "../ui/iconcache.h"
#include "string.h"

namespace pv {
namespace prop {

String::String(QString name, QString label, Getter getter, Setter setter)
    : Property(name, label, getter, setter), _line_edit(NULL),
      _browse_btn(NULL), _container(NULL) {}

bool String::is_path_or_dir() {
  return name().contains("path", Qt::CaseInsensitive) ||
         name().contains("dir", Qt::CaseInsensitive);
}

QWidget *String::get_widget(QWidget *parent, bool auto_commit) {
  if (_container)
    return _container;

  GVariant *const value = _getter ? _getter() : NULL;
  if (!value)
    return NULL;

  _container = new QWidget(parent);
  QHBoxLayout *layout = new QHBoxLayout(_container);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(4);

  if (is_path_or_dir()) {
    _browse_btn = new QToolButton(_container);
    QString iconPath = GetIconPath();
    _browse_btn->setIcon(IconCache::Instance().icon(iconPath + "/open.svg"));
    _browse_btn->setToolTip("...");
    layout->addWidget(_browse_btn);
    connect(_browse_btn, &QToolButton::clicked, this, [this, parent]() {
      QString dir = QFileDialog::getExistingDirectory(
          parent, tr("Select Directory"), _line_edit->text(),
          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
      if (!dir.isEmpty()) {
        _line_edit->setText(dir);
        commit();
      }
    });
  }

  _line_edit = new QLineEdit(_container);
  _line_edit->setText(QString::fromUtf8(g_variant_get_string(value, NULL)));
  g_variant_unref(value);
  layout->addWidget(_line_edit);

  if (auto_commit)
    connect(_line_edit, &QLineEdit::textEdited, this, &String::on_text_edited);

  return _container;
}

void String::commit() {
  assert(_setter);

  if (!_line_edit)
    return;

  QByteArray ba = _line_edit->text().toUtf8();
  _setter(g_variant_new_string(ba.data()));
  emit committed();
}

void String::on_text_edited(const QString &) { commit(); }

} // namespace prop
} // namespace pv
