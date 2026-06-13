/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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

#define BOOST_BIND_GLOBAL_PLACEHOLDERS
#include <boost/bind.hpp>

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QMetaObject>
#include <QUrl>
#include <assert.h>

#include "../config/appconfig.h"
#include "../dialogs/about.h"
#include "../ui/fn.h"
#include "../ui/dockfonts.h"
#include "../ui/iconcache.h"
#include "../ui/langresource.h"
#include "logobar.h"

namespace pv {
namespace toolbars {

LogoBar::LogoBar(SigSession *session, QWidget *parent)
    : QToolBar("File Bar", parent), _enable(true), _connected(false),
      _session(session)
// _logo_button(this)

{
  _mainForm = NULL;

  setMovable(false);
  setContentsMargins(0, 0, 0, 0);

  _action_en = new QAction(this);
  _action_en->setObjectName(QString::fromUtf8("actionEn"));

  _action_cn = new QAction(this);
  _action_cn->setObjectName(QString::fromUtf8("actionCn"));

  _action_traditional = new QAction(this);
  _action_traditional->setObjectName(QString::fromUtf8("actionTraditional"));

  _language = new QMenu(this);
  _language->setObjectName(QString::fromUtf8("menuLanguage"));
  _language->addAction(_action_cn);
  _language->addAction(_action_traditional);
  _language->addAction(_action_en);

  _action_en->setIcon(QIcon(":/icons/English.svg"));
  _action_cn->setIcon(QIcon(":/icons/Chinese.svg"));
  _action_traditional->setIcon(QIcon(":/icons/Chinese.svg"));

  _about = new QAction(this);
  _about->setObjectName(QString::fromUtf8("actionAbout"));
  // _logo_button.addAction(_about);

  _manual = new QAction(this);
  _manual->setObjectName(QString::fromUtf8("actionManual"));
  // _logo_button.addAction(_manual);

  _issue = new QAction(this);
  _issue->setObjectName(QString::fromUtf8("actionIssue"));
  // _logo_button.addAction(_issue);

  _update = new QAction(this);

  _menu = new QMenu(this);
  _menu->addMenu(_language);
  _menu->addAction(_about);
  _menu->addAction(_manual);
  _menu->addAction(_issue);
  _menu->addAction(_update);
  // _logo_button.setMenu(_menu);

  // _logo_button.setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  // _logo_button.setPopupMode(QToolButton::InstantPopup);

  // QWidget *spacer = new QWidget(this);
  // spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  // addWidget(spacer);
  // addWidget(&_logo_button);
  // QWidget *margin = new QWidget(this);
  // margin->setMinimumWidth(20);
  // addWidget(margin);

  connect(_action_en, &QAction::triggered, this,
          &LogoBar::on_actionEn_triggered);
  connect(_action_cn, &QAction::triggered, this,
          &LogoBar::on_actionCn_triggered);
  connect(_action_traditional, &QAction::triggered, this,
          &LogoBar::on_actionTraditional_triggered);
  connect(_about, &QAction::triggered, this,
          &LogoBar::on_actionAbout_triggered);
  connect(_manual, &QAction::triggered, this, &LogoBar::sig_open_doc);
  connect(_issue, &QAction::triggered, this,
          &LogoBar::on_actionIssue_triggered);
  connect(_update, &QAction::triggered, this, &LogoBar::on_action_update);

  ADD_UI(this);
}

LogoBar::~LogoBar() { REMOVE_UI(this); }

void LogoBar::retranslateUi() {

  // _logo_button.setText(L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP),
  // "Help"));
  _language->setTitle(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_LANG), "&Language"));
  _action_en->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_LANG_EN), "English"));
  _action_cn->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_LANG_CN), "中文(简体)"));
  _action_traditional->setText(L_S(
      STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_LANG_TRADITIONAL), "中文(繁體)"));
  _about->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_ABOUT), "&About..."));
  _manual->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_MANUAL), "&Manual..."));
  _issue->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_BUG), "&Bug Report"));
  _update->setText(
      L_S(STR_PAGE_TOOLBAR, S_ID(IDS_TOOLBAR_HELP_UPDATE), "&Update"));

  reStyle();
}

void LogoBar::reStyle() {
  QString iconPath = GetIconPath();
  QColor normalColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-color");
  QColor activeColor = AppConfig::Instance().GetThemeColor("@titlebar-icon-accent");

  auto getIcon = [&](const QString &name) {
      return normalColor.isValid() ? IconCache::Instance().tintedIcon(iconPath + name, normalColor)
                                 : IconCache::Instance().icon(iconPath + name);
  };

  auto getQrcIcon = [&](const QString &path, bool active) {
      QColor c = active ? activeColor : normalColor;
      return c.isValid() ? IconCache::Instance().tintedIcon(path, c)
                         : IconCache::Instance().icon(path);
  };

  AppConfig &app = AppConfig::Instance();
  
  _action_en->setIcon(getQrcIcon(":/icons/English.svg", app.frameOptions.language == LAN_EN));
  _action_cn->setIcon(getQrcIcon(":/icons/Chinese.svg", app.frameOptions.language == LAN_CN));
  _action_traditional->setIcon(getQrcIcon(":/icons/Chinese.svg", app.frameOptions.language == LAN_TRADITIONAL));

  if (app.frameOptions.language == LAN_EN)
    _language->setIcon(getQrcIcon(":/icons/English.svg", false));
  else
    _language->setIcon(getQrcIcon(":/icons/Chinese.svg", false));

  _about->setIcon(getIcon("/about.svg"));
  _manual->setIcon(getIcon("/manual.svg"));
  _issue->setIcon(getIcon("/bug.svg"));
  _update->setIcon(getIcon("/update.svg"));

  // if (_connected)
  //     _logo_button.setIcon(QIcon(iconPath+"/logo_color.svg"));
  // else
  //     _logo_button.setIcon(QIcon(iconPath+"/logo_noColor.svg"));
}

void LogoBar::dsl_connected(bool conn) {
  _connected = conn;
  QString iconPath = GetIconPath();
  // if (_connected)
  //     _logo_button.setIcon(QIcon(iconPath+"/logo_color.svg"));
  // else
  //     _logo_button.setIcon(QIcon(iconPath+"/logo_noColor.svg"));
}

void LogoBar::on_actionEn_triggered() {
  assert(_mainForm);
  _mainForm->switchLanguage(LAN_EN);
  reStyle();
}

void LogoBar::on_actionCn_triggered() {
  assert(_mainForm);
  _mainForm->switchLanguage(LAN_CN);
  reStyle();
}

void LogoBar::on_actionTraditional_triggered() {
  assert(_mainForm);
  _mainForm->switchLanguage(LAN_TRADITIONAL);
  reStyle();
}

void LogoBar::on_actionAbout_triggered() {
  dialogs::About dlg(this);
  dlg.exec();
}

void LogoBar::on_actionManual_triggered() {
  QDir dir(GetAppDataDir());
  QDesktopServices::openUrl(QUrl("file:///" + dir.absolutePath() + "/ug.pdf"));
}

void LogoBar::on_actionIssue_triggered() {
  QDesktopServices::openUrl(
      QUrl(QLatin1String("https://github.com/PXLogic/PXView/issues")));
}

void LogoBar::on_action_update() {
  if (AppConfig::Instance().frameOptions.language == LAN_CN) {
    QDesktopServices::openUrl(
        QUrl(QLatin1String("https://github.com/PXLogic/PXView")));
  } else {
    QDesktopServices::openUrl(
        QUrl(QLatin1String("https://github.com/PXLogic/PXView")));
  }
}

void LogoBar::enable_toggle(bool enable) { (void)enable; }

void LogoBar::UpdateLanguage() { retranslateUi(); }

void LogoBar::UpdateTheme() { reStyle(); }

void LogoBar::UpdateFont() {
  QFont font = theme_font_toolbar();
  ui::set_toolbar_font(this, font);
}

} // namespace toolbars
} // namespace pv
