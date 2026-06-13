/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2023 DreamSourceLab <support@dreamsourcelab.com>
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

#include "fn.h"
#include "dockfonts.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QFontMetrics>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolBar>
#include <QWidget>
#include <assert.h>

#include "../config/appconfig.h"
#include "../ui/xtoolbutton.h"

namespace ui {

void set_font_param(QFont &font, struct FontParam &param) {
  font.setPixelSize(param.size >= 12.0f ? (int)param.size : dock_font_content().pixelSize());

  if (param.name != "") {
    font.setFamily(param.name);
  }
}

void set_toolbar_font(QToolBar *bar, QFont font) {
  assert(bar);

  auto buttons = bar->findChildren<QToolButton *>();
  for (auto o : buttons) {
    o->setFont(font);
  }

  auto buttons2 = bar->findChildren<QPushButton *>();
  for (auto o : buttons2) {
    o->setFont(font);
  }

  auto comboxs = bar->findChildren<QComboBox *>();
  for (auto o : comboxs) {
    o->setFont(font);
  }

  auto labels = bar->findChildren<QLabel *>();
  for (auto o : labels) {
    o->setFont(font);
  }

  auto actions = bar->findChildren<QAction *>();
  for (auto o : actions) {
    o->setFont(font);
  }
}

void set_form_font(QWidget *wid, QFont font) {
  assert(wid);

  auto buttons2 = wid->findChildren<QPushButton *>();
  for (auto o : buttons2) {
    o->setFont(font);
  }

  auto comboxs = wid->findChildren<QComboBox *>();
  for (auto o : comboxs) {
    o->setFont(font);
  }

  auto labels = wid->findChildren<QLabel *>();
  for (auto o : labels) {
    o->setFont(font);
  }

  auto edits = wid->findChildren<QLineEdit *>();
  for (auto o : edits) {
    o->setFont(font);
  }

  auto textEdits = wid->findChildren<QTextEdit *>();
  for (auto o : textEdits) {
    o->setFont(font);
  }

  auto radios = wid->findChildren<QRadioButton *>();
  for (auto o : radios) {
    o->setFont(font);
  }

  auto checks = wid->findChildren<QCheckBox *>();
  for (auto o : checks) {
    o->setFont(font);
  }

  // Magnify the size.
  font.setPixelSize(font.pixelSize() > 0 ? font.pixelSize() + 1 : dock_font_label().pixelSize());

  auto tabs = wid->findChildren<QTabWidget *>();
  for (auto o : tabs) {
    o->setFont(font);
  }

  auto groups = wid->findChildren<QGroupBox *>();
  for (auto o : groups) {
    o->setFont(font);
  }
}

void set_dock_form_font(QWidget *wid) {
  assert(wid);

  QFont sectionFont = dock_font_section_title();
  QFont labelFont = dock_font_label();
  QFont contentFont = dock_font_content();

  auto labels = wid->findChildren<QLabel *>();
  for (auto o : labels) {
    if (o->objectName() == QLatin1String("dock_section_title"))
      o->setFont(sectionFont);
    else
      o->setFont(labelFont);
  }

  auto buttons = wid->findChildren<QPushButton *>();
  for (auto o : buttons) {
    o->setFont(contentFont);
  }

  auto comboxs = wid->findChildren<QComboBox *>();
  for (auto o : comboxs) {
    o->setFont(contentFont);
  }

  auto edits = wid->findChildren<QLineEdit *>();
  for (auto o : edits) {
    o->setFont(contentFont);
  }

  auto textEdits = wid->findChildren<QTextEdit *>();
  for (auto o : textEdits) {
    o->setFont(contentFont);
  }

  auto radios = wid->findChildren<QRadioButton *>();
  for (auto o : radios) {
    o->setFont(contentFont);
  }

  auto checks = wid->findChildren<QCheckBox *>();
  for (auto o : checks) {
    o->setFont(contentFont);
  }

  auto spinboxes = wid->findChildren<QSpinBox *>();
  for (auto o : spinboxes) {
    o->setFont(contentFont);
  }

  auto tabs = wid->findChildren<QTabWidget *>();
  for (auto o : tabs) {
    o->setFont(labelFont);
  }

  auto groups = wid->findChildren<QGroupBox *>();
  for (auto o : groups) {
    o->setFont(labelFont);
  }
}

QSize measure_string(QFont font, QString str) {
  QFontMetrics fm(font);
  QRect rc = fm.boundingRect(str);
  return rc.size();
}

void adjust_label_size(QLabel *ctrl, AdjustSizeAction action) {
  QSize sz = measure_string(ctrl->font(), ctrl->text());

  if (action == ADJUST_WIDTH) {
    ctrl->setFixedWidth(sz.width() + 5);
  } else if (action == ADJUST_HEIGHT) {
    ctrl->setFixedHeight(sz.height() + 5);
  } else {
    ctrl->setFixedHeight(sz.height() + 5);
    ctrl->setFixedWidth(sz.width() + 5);
  }
}

} // namespace ui