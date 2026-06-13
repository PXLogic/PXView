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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "applicationpardlg.h"
#include "../ui/widgetinspector.h"
#include "dsdialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QString>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

#include "../appcontrol.h"
#include "../config/appconfig.h"
#include "../config/shortcutdefs.h"
#include "../log.h"
#include "../sigsession.h"
#include "../ui/dscombobox.h"
#include "../ui/langresource.h"
#include "../ui/uimanager.h"

ShortcutKeyCapture::ShortcutKeyCapture(QWidget *parent)
    : QLineEdit(parent), m_capturing(false) {
  setReadOnly(true);
  setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
}

void ShortcutKeyCapture::setKeySequence(const QString &key) {
  m_keySeq = key;
  setText(key.isEmpty() ? "" : key);
}

QString ShortcutKeyCapture::keySequence() const { return m_keySeq; }

void ShortcutKeyCapture::keyPressEvent(QKeyEvent *event) {
  int key = event->key();
  Qt::KeyboardModifiers modifiers = event->modifiers();

  if (key == Qt::Key_Escape) {
    event->accept();
    clearFocus();
    return;
  }

  if (key == Qt::Key_Backspace || key == Qt::Key_Delete) {
    m_keySeq = "";
    setText("");
    emit keySequenceChanged("");
    event->accept();
    return;
  }

  if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt ||
      key == Qt::Key_Meta) {
    event->accept();
    return;
  }

  QString str;
  if (modifiers & Qt::ControlModifier)
    str += "Ctrl+";
  if (modifiers & Qt::ShiftModifier)
    str += "Shift+";
  if (modifiers & Qt::AltModifier && !str.isEmpty())
    str += "Alt+";

  if (key >= Qt::Key_A && key <= Qt::Key_Z) {
    str += QChar(key);
  } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
    str += QChar(key);
  } else if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
    str += "F" + QString::number(key - Qt::Key_F1 + 1);
  } else {
    switch (key) {
    case Qt::Key_Space:
      str += "Space";
      break;
    case Qt::Key_Tab:
      str += "Tab";
      break;
    case Qt::Key_Backspace:
      str += "Backspace";
      break;
    case Qt::Key_Return:
      str += "Return";
      break;
    case Qt::Key_Enter:
      str += "Enter";
      break;
    case Qt::Key_Insert:
      str += "Insert";
      break;
    case Qt::Key_Delete:
      str += "Delete";
      break;
    case Qt::Key_Home:
      str += "Home";
      break;
    case Qt::Key_End:
      str += "End";
      break;
    case Qt::Key_PageUp:
      str += "PgUp";
      break;
    case Qt::Key_PageDown:
      str += "PgDown";
      break;
    case Qt::Key_Up:
      str += "Up";
      break;
    case Qt::Key_Down:
      str += "Down";
      break;
    case Qt::Key_Left:
      str += "Left";
      break;
    case Qt::Key_Right:
      str += "Right";
      break;
    case Qt::Key_BracketLeft:
      str += "[";
      break;
    case Qt::Key_BracketRight:
      str += "]";
      break;
    case Qt::Key_Pause:
      str += "Pause";
      break;
    case Qt::Key_ScrollLock:
      str += "ScrollLock";
      break;
    case Qt::Key_Minus:
      str += "-";
      break;
    case Qt::Key_Equal:
      str += "=";
      break;
    case Qt::Key_Semicolon:
      str += ";";
      break;
    case Qt::Key_Apostrophe:
      str += "'";
      break;
    case Qt::Key_Comma:
      str += ",";
      break;
    case Qt::Key_Period:
      str += ".";
      break;
    case Qt::Key_Slash:
      str += "/";
      break;
    case Qt::Key_Backslash:
      str += "\\";
      break;
    case Qt::Key_QuoteLeft:
      str += "`";
      break;
    default:
      event->ignore();
      return;
    }
  }

  m_keySeq = str;
  setText(str);
  emit keySequenceChanged(str);
  event->accept();
}

void ShortcutKeyCapture::focusOutEvent(QFocusEvent *event) {
  QLineEdit::focusOutEvent(event);
}

namespace pv {
namespace dialogs {

ApplicationParamDlg::ApplicationParamDlg()
    : _nav_list(nullptr), _page_stack(nullptr), _ck_quickScroll(nullptr),
      _ck_trigInMid(nullptr), _ck_profileBar(nullptr), _ck_abortData(nullptr),
      _ck_autoScrollLatestData(nullptr),
      _shortcut_list(nullptr), _shortcut_selected_row(-1), _btn_accept(nullptr),
      _btn_restore(nullptr), _btn_reset_default(nullptr), _btn_delete(nullptr),
      _clash_warning_label(nullptr), _style_category_tree(nullptr),
      _style_page_stack(nullptr), _preset_combo(nullptr), _live_preview_timer(nullptr),
      _file_watcher(nullptr) {}

ApplicationParamDlg::~ApplicationParamDlg() {
  if (_file_watcher) {
    delete _file_watcher;
  }
}

QWidget *ApplicationParamDlg::createDisplayPage() {
  AppConfig &app = AppConfig::Instance();

  QWidget *page = new QWidget();
  QVBoxLayout *lay = new QVBoxLayout();
  lay->setContentsMargins(10, 10, 10, 10);
  lay->setSpacing(8);
  lay->setAlignment(Qt::AlignTop);

  _ck_quickScroll = new QCheckBox();
  _ck_quickScroll->setChecked(app.appOptions.quickScroll);

  _ck_trigInMid = new QCheckBox();
  _ck_trigInMid->setChecked(app.appOptions.trigPosDisplayInMid);

  _ck_profileBar = new QCheckBox();
  _ck_profileBar->setChecked(app.appOptions.displayProfileInBar);

  _ck_abortData = new QCheckBox();
  _ck_abortData->setChecked(app.appOptions.swapBackBufferAlways);

  _ck_autoScrollLatestData = new QCheckBox();
  _ck_autoScrollLatestData->setChecked(app.appOptions.autoScrollLatestData);

  QGroupBox *logicGroup =
      new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_LOGIC), "Logic"));
  QGridLayout *logicLay = new QGridLayout();
  logicLay->setContentsMargins(10, 15, 15, 10);
  logicLay->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  logicGroup->setLayout(logicLay);
  logicLay->addWidget(
      new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_QUICK_SCROLL), "Quick scroll")),
      0, 0, Qt::AlignLeft);
  logicLay->addWidget(_ck_quickScroll, 0, 1, Qt::AlignRight);
  logicLay->addWidget(
      new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_USE_ABORT_DATA_REPEAT),
                     "Used abort data")),
      1, 0, Qt::AlignLeft);
  logicLay->addWidget(_ck_abortData, 1, 1, Qt::AlignRight);
  logicLay->addWidget(
      new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_AUTO_SCROLL_LATEAST_DATA),
                     "Auto scoll latest")),
      2, 0, Qt::AlignLeft);
  logicLay->addWidget(_ck_autoScrollLatestData, 2, 1, Qt::AlignRight);
  lay->addWidget(logicGroup);

  QGroupBox *dsoGroup =
      new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_DSO), "Scope"));
  QGridLayout *dsoLay = new QGridLayout();
  dsoLay->setContentsMargins(10, 15, 15, 10);
  dsoLay->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  dsoGroup->setLayout(dsoLay);
  dsoLay->addWidget(
      new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_TRIG_DISPLAY_MIDDLE),
                     "Tig pos in middle")),
      0, 0, Qt::AlignLeft);
  dsoLay->addWidget(_ck_trigInMid, 0, 1, Qt::AlignRight);
  lay->addWidget(dsoGroup);

  QGroupBox *uiGroup =
      new QGroupBox(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_GROUP_UI), "UI"));
  QGridLayout *uiLay = new QGridLayout();
  uiLay->setContentsMargins(10, 15, 15, 10);
  uiLay->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  uiGroup->setLayout(uiLay);
  uiLay->addWidget(
      new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_DISPLAY_PROFILE_IN_BAR),
                     "Profile in bar")),
      0, 0, Qt::AlignLeft);
  uiLay->addWidget(_ck_profileBar, 0, 1, Qt::AlignRight);
  lay->addWidget(uiGroup);

  lay->addStretch();
  page->setLayout(lay);
  return page;
}

QWidget *ApplicationParamDlg::createShortcutPage() {
  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);

  _shortcut_keys.clear();
  _shortcut_original_keys.clear();
  _shortcut_clash_ids.clear();
  _shortcut_selected_row = -1;

  AppConfig &app = AppConfig::Instance();
  for (int i = 0; i < infoCount; i++) {
    QString keySeq(infos[i].keySequence);
    for (int j = 0; j < app.shortcutOptions.items.size(); j++) {
      if (app.shortcutOptions.items[j].actionId == infos[i].actionId) {
        keySeq = app.shortcutOptions.items[j].keySequence;
        break;
      }
    }
    _shortcut_keys[infos[i].actionId] = keySeq;
    _shortcut_original_keys[infos[i].actionId] = keySeq;
  }

  QWidget *page = new QWidget();
  QVBoxLayout *pageLay = new QVBoxLayout();
  pageLay->setContentsMargins(10, 10, 10, 10);
  pageLay->setSpacing(5);

  QHBoxLayout *headerLay = new QHBoxLayout();
  headerLay->setSpacing(5);
  QLabel *actionLabel =
      new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_ACTION), "Action"));
  actionLabel->setFixedWidth(150);
  QLabel *shortcutLabel =
      new QLabel(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_SHORTCUT), "Shortcut"));
  headerLay->addWidget(actionLabel);
  headerLay->addWidget(shortcutLabel, 1);
  pageLay->addLayout(headerLay);

  QHBoxLayout *contentLay = new QHBoxLayout();
  contentLay->setSpacing(5);

  _shortcut_list = new QListWidget();
  _shortcut_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  _shortcut_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  _shortcut_list->setSelectionMode(QAbstractItemView::SingleSelection);
  _shortcut_list->setFocusPolicy(Qt::NoFocus);

  for (int i = 0; i < infoCount; i++) {
    QString actionName =
        L_S(STR_PAGE_DLG, infos[i].displayName, infos[i].displayName);
    QString keyStr = _shortcut_keys[infos[i].actionId];

    QWidget *itemWidget = new QWidget();
    QHBoxLayout *itemLay = new QHBoxLayout(itemWidget);
    itemLay->setContentsMargins(5, 0, 5, 0);
    itemLay->setSpacing(0);

    QLabel *nameLabel = new QLabel(actionName);
    nameLabel->setFixedWidth(140);
    nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    QLabel *keyLabel = new QLabel(keyStr.isEmpty() ? "" : keyStr);
    keyLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    keyLabel->setObjectName("shortcut_key_label");

    QLabel *clashIcon = new QLabel();
    clashIcon->setFixedWidth(16);
    clashIcon->setTextFormat(Qt::PlainText);
    clashIcon->setObjectName("clash_icon");
    clashIcon->hide();

    itemLay->addWidget(nameLabel);
    itemLay->addWidget(keyLabel, 1);
    itemLay->addWidget(clashIcon);

    QListWidgetItem *listItem = new QListWidgetItem(_shortcut_list);
    listItem->setSizeHint(QSize(0, 24));
    _shortcut_list->addItem(listItem);
    _shortcut_list->setItemWidget(listItem, itemWidget);
  }

  QObject::connect(_shortcut_list, &QListWidget::currentRowChanged,
                   [this](int row) { onShortcutRowSelected(row); });

  contentLay->addWidget(_shortcut_list, 1);

  QVBoxLayout *btnLay = new QVBoxLayout();
  btnLay->setSpacing(4);
  btnLay->setContentsMargins(0, 0, 0, 0);

  _btn_accept =
      new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_ACCEPT), "Accept"));
  _btn_accept->setFixedWidth(82);
  _btn_accept->setEnabled(false);

  _btn_restore =
      new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_RESTORE), "Restore"));
  _btn_restore->setFixedWidth(82);
  _btn_restore->setEnabled(false);

  _btn_reset_default = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_RESET_DEFAULT), "Reset Default"));
  _btn_reset_default->setFixedWidth(82);
  _btn_reset_default->setEnabled(false);

  _btn_delete = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_DELETE_KEY), "Delete Key"));
  _btn_delete->setFixedWidth(82);
  _btn_delete->setEnabled(false);

  QFrame *sepLine = new QFrame();
  sepLine->setFrameShape(QFrame::HLine);
  sepLine->setFixedHeight(1);

  QPushButton *btnResetAll = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_RESET_ALL), "Reset All"));
  btnResetAll->setFixedWidth(82);

  btnLay->addWidget(_btn_accept);
  btnLay->addWidget(_btn_restore);
  btnLay->addWidget(_btn_reset_default);
  btnLay->addWidget(_btn_delete);
  btnLay->addSpacing(6);
  btnLay->addWidget(sepLine);
  btnLay->addSpacing(6);
  btnLay->addWidget(btnResetAll);
  btnLay->addStretch();

  contentLay->addLayout(btnLay);
  pageLay->addLayout(contentLay, 1);

  _clash_warning_label = new QLabel();
  _clash_warning_label->setObjectName("clash_warning");
  _clash_warning_label->setFixedHeight(28);
  _clash_warning_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  _clash_warning_label->hide();
  pageLay->addWidget(_clash_warning_label);

  QObject::connect(_btn_accept, &QPushButton::clicked, _btn_accept,
                   [this]() { onShortcutAccept(); });
  QObject::connect(_btn_restore, &QPushButton::clicked, _btn_restore,
                   [this]() { onShortcutRestore(); });
  QObject::connect(_btn_reset_default, &QPushButton::clicked,
                   _btn_reset_default, [this]() { onShortcutResetDefault(); });
  QObject::connect(_btn_delete, &QPushButton::clicked, _btn_delete,
                   [this]() { onShortcutDelete(); });
  QObject::connect(btnResetAll, &QPushButton::clicked, btnResetAll,
                   [this]() { onResetShortcuts(); });

  checkShortcutClash();

  page->setLayout(pageLay);
  return page;
}

QWidget *ApplicationParamDlg::createStylePage() {
  QWidget *page = new QWidget();
  QVBoxLayout *lay = new QVBoxLayout();
  lay->setContentsMargins(10, 10, 10, 10);
  lay->setSpacing(8);

  _default_style_tokens.clear();
  _style_tokens.clear();
  _style_preview_widgets.clear();
  _style_button_widgets.clear();
  _style_line_edit_widgets.clear();

  AppConfig &app = AppConfig::Instance();
  QString style = app.frameOptions.style;

  // Load defaults from current JSON theme
  QString jsonRes = ":/" + style + ".json";
  QFile jsonFile(jsonRes);
  if (jsonFile.open(QFile::ReadOnly | QFile::Text)) {
    QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonFile.readAll());
    QJsonObject rootObj = jsonDoc.object();
    QJsonObject tokensObj = rootObj.value("tokens").toObject();
    for (const QString &key : tokensObj.keys()) {
      _default_style_tokens[key] = tokensObj.value(key).toString();
    }
    jsonFile.close();
  }

  _style_tokens = _default_style_tokens;

  for (int i = 0; i < app.styleOptions.items.size(); i++) {
    _style_tokens[app.styleOptions.items[i].tokenName] =
        app.styleOptions.items[i].value;
  }

  _original_style_tokens = _style_tokens;

  QFile schemaFile(":/theme-schema.json");
  QJsonArray categoriesArr;
  QSet<QString> hiddenSet;
  if (schemaFile.open(QFile::ReadOnly | QFile::Text)) {
    QJsonDocument doc = QJsonDocument::fromJson(schemaFile.readAll());
    QJsonObject schemaRoot = doc.object();
    categoriesArr = schemaRoot.value("categories").toArray();

    QJsonArray hiddenTokensArr = schemaRoot.value("hidden").toArray();
    for (int i = 0; i < hiddenTokensArr.size(); ++i) {
      hiddenSet.insert(hiddenTokensArr[i].toString());
    }
    schemaFile.close();
  }

  QHBoxLayout *presetLay = new QHBoxLayout();
  QLabel *presetLbl = new QLabel(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_PRESET), "Preset Theme:"));
  _preset_combo = new DsComboBox();
  _preset_combo->addItem(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_CUSTOM), "Custom"), "");
  _preset_combo->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_DARK), "Dark"),
                         ":/dark.json");
  _preset_combo->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_LIGHT), "Light"),
                         ":/light.json");
  _preset_combo->addItem(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_ATOM), "Atom One Dark"),
      ":/atom.json");
  _preset_combo->addItem(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_AYU), "Ayu Light"),
      ":/ayu.json");
  _preset_combo->addItem(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_DARK_CARDS), "Dark (Colored Cards)"),
      ":/dark_cards.json");
  _preset_combo->addItem(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_LIGHT_CARDS), "Light (Colored Cards)"),
      ":/light_cards.json");

  QString userThemePath =
      QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
      "/themes";
  QDir dir(userThemePath);
  if (!dir.exists())
    dir.mkpath(".");
  QStringList userThemes =
      dir.entryList(QStringList() << "*.json", QDir::Files);
  for (const QString &themeFile : userThemes) {
    QString absPath = dir.absoluteFilePath(themeFile);
    if (QFileInfo(absPath).isFile()) {
      _preset_combo->addItem(QFileInfo(absPath).baseName(), absPath);
    }
  }

  QPushButton *savePresetBtn = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_SAVE_AS_PRESET), "Save as Preset"));
  QPushButton *deletePresetBtn = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_DELETE_PRESET), "Delete Preset"));

  // System presets start with ":/", disable delete for them
  bool isSystemPreset = _preset_combo->currentData().toString().startsWith(":/");
  deletePresetBtn->setEnabled(!isSystemPreset);

  presetLay->addWidget(presetLbl);
  presetLay->addWidget(_preset_combo);
  presetLay->addWidget(savePresetBtn);
  presetLay->addWidget(deletePresetBtn);
  presetLay->addStretch();
  lay->addLayout(presetLay);

  QSplitter *splitter = new QSplitter(Qt::Horizontal);

  QObject::connect(_preset_combo,
                   QOverload<int>::of(&QComboBox::currentIndexChanged),
                   splitter, [this, deletePresetBtn](int index) {
                     if (index > 0) {
                       QString path = _preset_combo->currentData().toString();
                       if (!path.isEmpty()) {
                         applyPresetTheme(path);
                       }
                     }
                     bool isSystem = _preset_combo->currentData().toString().startsWith(":/");
                     deletePresetBtn->setEnabled(!isSystem);
                   });

  QObject::connect(
      savePresetBtn, &QPushButton::clicked, splitter, [this, userThemePath]() {
        bool ok;
        QWidget *dlgWindow =
            _style_page_stack ? _style_page_stack->window() : nullptr;
        QString name = QInputDialog::getText(
            dlgWindow,
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_SAVE_PRESET), "Save Preset"),
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_PRESET_NAME), "Preset Name:"),
            QLineEdit::Normal, "", &ok);
        if (ok && !name.isEmpty()) {
          QJsonObject tokensObj;
          for (auto it = _style_tokens.begin(); it != _style_tokens.end();
               ++it) {
            tokensObj[it.key()] = it.value();
          }
          QJsonObject rootObj;
          rootObj["tokens"] = tokensObj;
          QJsonDocument doc(rootObj);
          QFile file(userThemePath + "/" + name + ".json");
          if (file.open(QFile::WriteOnly)) {
            file.write(doc.toJson(QJsonDocument::Indented));
            file.close();
            _preset_combo->addItem(name, file.fileName());
            QWidget *dlgWindow =
                _style_page_stack ? _style_page_stack->window() : nullptr;
            QMessageBox::information(
                dlgWindow,
                L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_SAVE_SUCCESS), "Success"),
                L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_SAVE_SUCCESS_MSG),
                    "Preset saved successfully."));
          }
        }
      });

  QObject::connect(
      deletePresetBtn, &QPushButton::clicked, splitter, [this, deletePresetBtn]() {
        int index = _preset_combo->currentIndex();
        if (index <= 0)
          return;
        QString path = _preset_combo->currentData().toString();
        if (path.startsWith(":/"))
          return;
        QWidget *dlgWindow =
            _style_page_stack ? _style_page_stack->window() : nullptr;
        auto ret = QMessageBox::question(
            dlgWindow,
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_DELETE_PRESET), "Delete Preset"),
            L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_DELETE_CONFIRM),
                "Are you sure you want to delete this preset?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret == QMessageBox::Yes) {
          QFile::remove(path);
          _preset_combo->removeItem(index);
        }
      });

  _style_category_tree = new QTreeWidget();
  _style_category_tree->setObjectName("styleCategoryTree");
  _style_category_tree->setHeaderHidden(true);
  _style_category_tree->setRootIsDecorated(false);
  _style_category_tree->setFixedWidth(200);

  _style_page_stack = new QStackedWidget();

  for (int i = 0; i < categoriesArr.size(); ++i) {
    QJsonObject catObj = categoriesArr[i].toObject();
    QString catId = catObj.value("id").toString();
    QString catName = catObj.value("name").toString();

    QTreeWidgetItem *item = new QTreeWidgetItem(_style_category_tree);
    item->setText(0, QString(LangResource::Instance()->get_lang_text(
                         STR_PAGE_DLG, catName.toUtf8().constData(),
                         catName.toUtf8().constData())));
    item->setData(0, Qt::UserRole, catId);

    QScrollArea *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    QWidget *catWidget = new QWidget();
    QVBoxLayout *catLay = new QVBoxLayout(catWidget);
    catLay->setContentsMargins(10, 10, 10, 10);
    catLay->setSpacing(10);

    QJsonArray tokensArr = catObj.value("tokens").toArray();
    for (int j = 0; j < tokensArr.size(); ++j) {
      QJsonObject tokenObj = tokensArr[j].toObject();
      QString tId = tokenObj.value("id").toString();
      QString tType = tokenObj.value("type").toString();
      QString tLabel = tokenObj.value("label").toString();

      if (hiddenSet.contains(tId))
        continue;

      QWidget *rowWidget = new QWidget();
      QHBoxLayout *rowLay = new QHBoxLayout(rowWidget);
      rowLay->setContentsMargins(0, 0, 0, 0);

      QString displayLabel = tLabel.isEmpty() ? tId : tLabel;
      QLabel *lbl = new QLabel(QString(LangResource::Instance()->get_lang_text(
          STR_PAGE_DLG, tLabel.toUtf8().constData(),
          displayLabel.toUtf8().constData())));
      lbl->setToolTip(tId);
      lbl->setMinimumWidth(150);
      rowLay->addWidget(lbl);

      QString currentValue = _style_tokens.value(tId, "");

      if (tType == "color") {
        QWidget *previewWidget = new QWidget();
        QColor c(currentValue);
        if (c.isValid()) {
          previewWidget->setStyleSheet(
              QString("background-color: %1; border: 1px solid #555; "
                      "border-radius: 4px;")
                  .arg(currentValue));
        } else {
          previewWidget->setStyleSheet("background-color: transparent; border: "
                                       "1px solid #555; border-radius: 4px;");
        }
        previewWidget->setFixedSize(30, 20);

        QWidget *previewContainer = new QWidget();
        QHBoxLayout *previewLay = new QHBoxLayout(previewContainer);
        previewLay->setContentsMargins(0, 0, 0, 0);
        previewLay->setAlignment(Qt::AlignCenter);
        previewLay->addWidget(previewWidget);
        rowLay->addWidget(previewContainer);

        QPushButton *colorBtn = new QPushButton(currentValue);
        colorBtn->setFixedWidth(100);
        rowLay->addWidget(colorBtn);

        _style_preview_widgets[tId] = previewWidget;
        _style_button_widgets[tId] = colorBtn;

        QObject::connect(colorBtn, &QPushButton::clicked, colorBtn,
                         [this, tId]() { onStyleTokenColorChanged(tId); });
      } else if (tType == "bool") {
        QCheckBox *checkBox = new QCheckBox();
        checkBox->setChecked(currentValue == "true");
        rowLay->addStretch();
        rowLay->addWidget(checkBox);

        _style_checkbox_widgets[tId] = checkBox;

        QObject::connect(checkBox, &QCheckBox::toggled, checkBox,
                         [this, tId](bool checked) {
                           onStyleTokenBoolChanged(tId, checked);
                         });
      } else {
        QLineEdit *lineEdit = new QLineEdit(currentValue);
        lineEdit->setFixedWidth(130);
        rowLay->addStretch();
        rowLay->addWidget(lineEdit);

        _style_line_edit_widgets[tId] = lineEdit;

        QObject::connect(lineEdit, &QLineEdit::textEdited, lineEdit,
                         [this, tId](const QString &text) {
                           onStyleTokenTextChanged(tId, text);
                         });
      }

      catLay->addWidget(rowWidget);
    }

    catLay->addStretch();
    scrollArea->setWidget(catWidget);
    _style_page_stack->addWidget(scrollArea);
    item->setData(0, Qt::UserRole + 1, _style_page_stack->count() - 1);
  }

  splitter->addWidget(_style_category_tree);
  splitter->addWidget(_style_page_stack);
  lay->addWidget(splitter, 1);

  if (!_live_preview_timer) {
    _live_preview_timer = new QTimer(_style_page_stack);
    QObject::connect(_live_preview_timer, &QTimer::timeout, _style_page_stack,
                     [this]() {
                       _live_preview_timer->stop();
                       applyLivePreview();
                     });
  }

  QObject::connect(_style_category_tree, &QTreeWidget::currentItemChanged,
                   _style_category_tree,
                   [this](QTreeWidgetItem *current, QTreeWidgetItem *previous) {
                     onStyleCategoryChanged(current, previous);
                   });
  if (_style_category_tree->topLevelItemCount() > 0) {
    _style_category_tree->setCurrentItem(_style_category_tree->topLevelItem(0));
  }

  QPushButton *pickerBtn = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_PICKER), "Picker Tool"));
  pickerBtn->setFixedWidth(100);
  pickerBtn->setCheckable(true);

  QPushButton *editJsonBtn = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_EDIT_JSON), "Edit JSON Source"));
  editJsonBtn->setFixedWidth(120);
  QObject::connect(editJsonBtn, &QPushButton::clicked, _style_page_stack,
                   [this]() { openExternalJsonEditor(); });

  QObject::connect(
      pickerBtn, &QPushButton::clicked, _style_page_stack, [this, pickerBtn]() {
        if (pickerBtn->isChecked()) {
          QWidget *dlg = _style_page_stack->window();
          if (dlg)
            dlg->hide();
          pv::ui::WidgetInspector::Instance()->setPickerModeEnabled(true);
        }
      });

  QObject::connect(
      pv::ui::WidgetInspector::Instance(),
      &pv::ui::WidgetInspector::widgetPicked, _style_page_stack,
      [this, pickerBtn](QWidget *w) {
        if (pv::ui::WidgetInspector::Instance()->isPickerModeEnabled()) {
          pv::ui::WidgetInspector::Instance()->setPickerModeEnabled(false);
          pickerBtn->setChecked(false);
          QWidget *dlg = _style_page_stack->window();
          if (dlg) {
            dlg->show();
            dlg->raise();
            dlg->activateWindow();
          }
          onWidgetPicked(w);
        }
      });

  QPushButton *importBtn =
      new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_IMPORT), "Import"));
  importBtn->setFixedWidth(80);
  QObject::connect(importBtn, &QPushButton::clicked, importBtn,
                   [this]() { onImportStyle(); });

  QPushButton *exportBtn =
      new QPushButton(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_EXPORT), "Export"));
  exportBtn->setFixedWidth(80);
  QObject::connect(exportBtn, &QPushButton::clicked, exportBtn,
                   [this]() { onExportStyle(); });

  QPushButton *resetBtn = new QPushButton(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SC_RESET_ALL), "Reset All"));
  resetBtn->setFixedWidth(80);
  QObject::connect(resetBtn, &QPushButton::clicked, resetBtn,
                   [this]() { onResetStyle(); });

  QHBoxLayout *btnLay = new QHBoxLayout();
  btnLay->addWidget(pickerBtn);
  btnLay->addWidget(editJsonBtn);
  btnLay->addStretch();
  btnLay->addWidget(importBtn);
  btnLay->addWidget(exportBtn);
  btnLay->addWidget(resetBtn);
  lay->addLayout(btnLay);

  page->setLayout(lay);
  return page;
}

void ApplicationParamDlg::onWidgetPicked(QWidget *w) {
  if (!w)
    return;
  QString className = w->metaObject()->className();
  QString objName = w->objectName();

  QString targetCategory = "global.colors";

  if (className.contains("LogicSignal") ||
      objName.contains("logic", Qt::CaseInsensitive)) {
    targetCategory = "logic.colors";
  } else if (className.contains("AnalogSignal") ||
             className.contains("DsoSignal") ||
             objName.contains("analog", Qt::CaseInsensitive)) {
    targetCategory = "signal.colors";
  } else if (className.contains("Dock") || className.contains("Panel")) {
    targetCategory = "typography";
  }

  for (int i = 0; i < _style_category_tree->topLevelItemCount(); ++i) {
    QTreeWidgetItem *item = _style_category_tree->topLevelItem(i);
    if (item->data(0, Qt::UserRole).toString() == targetCategory) {
      _style_category_tree->setCurrentItem(item);
      break;
    }
  }
}

void ApplicationParamDlg::onStyleCategoryChanged(QTreeWidgetItem *current,
                                                 QTreeWidgetItem *previous) {
  (void)previous;
  if (!current)
    return;
  int index = current->data(0, Qt::UserRole + 1).toInt();
  if (index >= 0 && index < _style_page_stack->count()) {
    _style_page_stack->setCurrentIndex(index);
  }
}

void ApplicationParamDlg::onStyleTokenColorChanged(const QString &tokenName) {
  QString currentValue = _style_tokens.value(tokenName, "");
  QColor c(currentValue);
  if (!c.isValid()) {
    c = Qt::white;
  }

  QColor newColor = QColorDialog::getColor(
      c, nullptr, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SELECT_COLOR), "Select Color"),
      QColorDialog::ShowAlphaChannel);
  if (newColor.isValid()) {
    QString value;
    if (newColor.alpha() < 255) {
      value = newColor.name(QColor::HexArgb).toUpper(); // #AARRGGBB
    } else {
      value = newColor.name(QColor::HexRgb).toUpper(); // #RRGGBB
    }

    _style_tokens[tokenName] = value;

    if (_preset_combo) {
      // Block signals temporarily to avoid double applying theme
      bool oldState = _preset_combo->blockSignals(true);
      _preset_combo->setCurrentIndex(0);
      _preset_combo->blockSignals(oldState);
    }

    if (_style_button_widgets.contains(tokenName)) {
      _style_button_widgets[tokenName]->setText(value);
    }
    if (_style_preview_widgets.contains(tokenName)) {
      _style_preview_widgets[tokenName]->setStyleSheet(
          QString("background-color: %1; border: 1px solid #555; "
                  "border-radius: 4px;")
              .arg(value));
    }

    // scheduleLivePreview();
  }
}

void ApplicationParamDlg::onStyleTokenTextChanged(const QString &tokenName,
                                                  const QString &value) {
  _style_tokens[tokenName] = value;

  if (_preset_combo) {
    bool oldState = _preset_combo->blockSignals(true);
    _preset_combo->setCurrentIndex(0);
    _preset_combo->blockSignals(oldState);
  }
  // scheduleLivePreview();
}

void ApplicationParamDlg::onStyleTokenBoolChanged(const QString &tokenName,
                                                   bool checked) {
  _style_tokens[tokenName] = checked ? "true" : "false";

  if (_preset_combo) {
    bool oldState = _preset_combo->blockSignals(true);
    _preset_combo->setCurrentIndex(0);
    _preset_combo->blockSignals(oldState);
  }
  // scheduleLivePreview();
}

void ApplicationParamDlg::scheduleLivePreview() {
  if (_live_preview_timer) {
    _live_preview_timer->start(50);
  }
}

void ApplicationParamDlg::openExternalJsonEditor() {
  _external_json_path =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
      QDir::separator() + "pxview_theme_edit.json";

  QJsonObject tokensObj;
  QStringList tokenNames = _style_tokens.keys();
  std::sort(tokenNames.begin(), tokenNames.end());
  for (const QString &name : tokenNames) {
    tokensObj[name] = _style_tokens[name];
  }
  QJsonDocument doc(tokensObj);
  QFile file(_external_json_path);
  if (file.open(QFile::WriteOnly | QFile::Text)) {
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
  }

  if (!_file_watcher) {
    _file_watcher = new QFileSystemWatcher();
    QObject::connect(
        _file_watcher, &QFileSystemWatcher::fileChanged,
        [this](const QString &path) { onExternalJsonChanged(path); });
  }

  if (!_file_watcher->files().contains(_external_json_path)) {
    _file_watcher->addPath(_external_json_path);
  }

  QDesktopServices::openUrl(QUrl::fromLocalFile(_external_json_path));
}

void ApplicationParamDlg::onExternalJsonChanged(const QString &path) {
  if (path != _external_json_path)
    return;

  QFile file(path);
  if (!file.open(QFile::ReadOnly | QFile::Text))
    return;
  QByteArray data = file.readAll();
  file.close();

  QJsonParseError error;
  QJsonDocument doc = QJsonDocument::fromJson(data, &error);
  if (error.error == QJsonParseError::NoError) {
    QJsonObject tokensObj = doc.object();
    for (auto it = tokensObj.begin(); it != tokensObj.end(); ++it) {
      if (it.value().isString()) {
        _style_tokens[it.key()] = it.value().toString();
      }
    }
    refreshStyleWidgets();
    scheduleLivePreview();
  }
}

void ApplicationParamDlg::applyPresetTheme(const QString &presetPath) {
  QFile file(presetPath);
  if (file.open(QFile::ReadOnly | QFile::Text)) {
    QByteArray data = file.readAll();
    file.close();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    QJsonObject root = doc.object();

    // Update frameOptions.style based on theme type ("dark" or "light")
    QString themeType = root.value("type").toString();
    if (!themeType.isEmpty()) {
      AppConfig &app = AppConfig::Instance();
      if (app.frameOptions.style != themeType) {
        app.frameOptions.style = themeType;
        app.SaveFrame();
      }
    }

    QJsonObject tokens = root.value("tokens").toObject();
    for (auto it = tokens.begin(); it != tokens.end(); ++it) {
      if (it.value().isString()) {
        _style_tokens[it.key()] = it.value().toString();
      }
    }
    refreshStyleWidgets();
    scheduleLivePreview();
  }
}

void ApplicationParamDlg::applyLivePreview() {
  AppConfig &app = AppConfig::Instance();
  QString style = app.frameOptions.style;
  QString qssRes = ":/theme.qss";
  QFile qss(qssRes);
  if (qss.open(QFile::ReadOnly | QFile::Text)) {
    QString qssContent = qss.readAll();
    qss.close();

    QHash<QString, QString> tokens;
    QRegularExpression tokenRe(
        "@([\\w-]+):\\s*([^\\r\\n]+?)\\s*(?:\\*/|\\r|\\n)");
    QRegularExpressionMatchIterator it = tokenRe.globalMatch(qssContent);
    while (it.hasNext()) {
      QRegularExpressionMatch match = it.next();
      QString tokenName = "@" + match.captured(1);
      QString tokenValue = match.captured(2).trimmed();
      tokens[tokenName] = tokenValue;
    }

    QMap<QString, QString>::const_iterator styleIt;
    for (styleIt = _style_tokens.constBegin();
         styleIt != _style_tokens.constEnd(); ++styleIt) {
      tokens[styleIt.key()] = styleIt.value();
    }

    QList<QString> keys = tokens.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
      return a.length() > b.length();
    });

    for (const QString &key : keys) {
      qssContent.replace(key, tokens[key]);
    }

    // Process SVG files that contain token placeholders (e.g. @accent)
    // Read each referenced SVG, replace token placeholders, write to temp dir
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                      "/pxview_themed_svgs";
    QDir().mkpath(tempDir);

    QRegularExpression svgRe("image:\\s*url\\((:[^)]+\\.svg)\\)");
    QRegularExpressionMatchIterator svgIt = svgRe.globalMatch(qssContent);
    QSet<QString> processedSvgs;
    while (svgIt.hasNext()) {
      QRegularExpressionMatch match = svgIt.next();
      QString svgResPath = match.captured(1);

      if (processedSvgs.contains(svgResPath))
        continue;
      processedSvgs.insert(svgResPath);

      QFile svgFile(svgResPath);
      if (!svgFile.open(QFile::ReadOnly | QFile::Text))
        continue;
      QString svgContent = svgFile.readAll();
      svgFile.close();

      // Check if SVG contains any token placeholders
      bool hasPlaceholders = false;
      for (const QString &key : keys) {
        if (svgContent.contains(key)) {
          hasPlaceholders = true;
          break;
        }
      }
      if (!hasPlaceholders)
        continue;

      // Replace token placeholders in SVG content
      for (const QString &key : keys) {
        svgContent.replace(key, tokens[key]);
      }

      // Write modified SVG to temp directory
      QString fileName = svgResPath;
      fileName.replace(":/", "");
      fileName.replace("/", "_");
      QString tempPath = tempDir + "/" + fileName;
      QFile tempFile(tempPath);
      if (tempFile.open(QFile::WriteOnly | QFile::Text)) {
        tempFile.write(svgContent.toUtf8());
        tempFile.close();
      }

      // Update QSS to reference temp file
      qssContent.replace(svgResPath, tempPath);
    }

    app.SetThemeTokens(tokens);
    qApp->setStyleSheet(qssContent);
    UiManager::Instance()->Update(UI_UPDATE_ACTION_THEME);
    UiManager::Instance()->Update(UI_UPDATE_ACTION_FONT);
  }
}

void ApplicationParamDlg::onShortcutRowSelected(int row) {
  _shortcut_selected_row = row;

  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);

  for (int i = 0; i < infoCount; i++) {
    QListWidgetItem *item = _shortcut_list->item(i);
    QWidget *w = _shortcut_list->itemWidget(item);
    if (!w)
      continue;

    ShortcutKeyCapture *capture = w->findChild<ShortcutKeyCapture *>();
    if (capture) {
      capture->deleteLater();
      QLabel *keyLabel = new QLabel(_shortcut_keys[infos[i].actionId].isEmpty()
                                        ? ""
                                        : _shortcut_keys[infos[i].actionId]);
      keyLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      keyLabel->setObjectName("shortcut_key_label");
      QHBoxLayout *lay = qobject_cast<QHBoxLayout *>(w->layout());
      if (lay) {
        QLayoutItem *oldItem = lay->itemAt(1);
        if (oldItem && oldItem->widget()) {
          lay->removeWidget(oldItem->widget());
          oldItem->widget()->deleteLater();
        }
        lay->insertWidget(1, keyLabel, 1);
      }
    }
  }

  if (row >= 0 && row < infoCount) {
    QListWidgetItem *item = _shortcut_list->item(row);
    QWidget *w = _shortcut_list->itemWidget(item);
    if (w) {
      QHBoxLayout *lay = qobject_cast<QHBoxLayout *>(w->layout());
      if (lay) {
        QLayoutItem *oldItem = lay->itemAt(1);
        if (oldItem && oldItem->widget()) {
          lay->removeWidget(oldItem->widget());
          oldItem->widget()->deleteLater();
        }

        ShortcutKeyCapture *capture = new ShortcutKeyCapture();
        capture->setKeySequence(_shortcut_keys[infos[row].actionId]);
        capture->setObjectName("shortcut_key_capture");

        QObject::connect(capture, &ShortcutKeyCapture::keySequenceChanged,
                         capture, [this, row](const QString &newKey) {
                           onShortcutKeyCaptured(row, newKey);
                         });

        lay->insertWidget(1, capture, 1);
        capture->setFocus();
      }
    }
  }

  updateShortcutButtons();
}

void ApplicationParamDlg::onShortcutKeyCaptured(int row,
                                                const QString &newKey) {
  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);
  if (row < 0 || row >= infoCount)
    return;

  _shortcut_keys[infos[row].actionId] = newKey;
  checkShortcutClash();
  updateShortcutButtons();
}

void ApplicationParamDlg::onShortcutAccept() { saveShortcutOptions(); }

void ApplicationParamDlg::onShortcutRestore() {
  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);
  if (_shortcut_selected_row < 0 || _shortcut_selected_row >= infoCount)
    return;

  int actionId = infos[_shortcut_selected_row].actionId;
  _shortcut_keys[actionId] = _shortcut_original_keys[actionId];

  refreshShortcutList();
  onShortcutRowSelected(_shortcut_selected_row);
}

void ApplicationParamDlg::onShortcutResetDefault() {
  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);
  if (_shortcut_selected_row < 0 || _shortcut_selected_row >= infoCount)
    return;

  int actionId = infos[_shortcut_selected_row].actionId;
  for (int i = 0; i < infoCount; i++) {
    if (infos[i].actionId == actionId) {
      _shortcut_keys[actionId] = QString(infos[i].keySequence);
      break;
    }
  }

  refreshShortcutList();
  onShortcutRowSelected(_shortcut_selected_row);
}

void ApplicationParamDlg::onShortcutDelete() {
  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);
  if (_shortcut_selected_row < 0 || _shortcut_selected_row >= infoCount)
    return;

  int actionId = infos[_shortcut_selected_row].actionId;
  _shortcut_keys[actionId] = "";

  refreshShortcutList();
  onShortcutRowSelected(_shortcut_selected_row);
}

void ApplicationParamDlg::onResetShortcuts() {
  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);

  _shortcut_keys.clear();
  for (int i = 0; i < infoCount; i++) {
    _shortcut_keys[infos[i].actionId] = QString(infos[i].keySequence);
  }

  _shortcut_selected_row = -1;
  refreshShortcutList();
  checkShortcutClash();
  updateShortcutButtons();
}

void ApplicationParamDlg::checkShortcutClash() {
  _shortcut_clash_ids.clear();

  QMap<int, QString>::const_iterator it1, it2;
  for (it1 = _shortcut_keys.constBegin(); it1 != _shortcut_keys.constEnd();
       ++it1) {
    if (it1.value().isEmpty())
      continue;
    for (it2 = _shortcut_keys.constBegin(); it2 != _shortcut_keys.constEnd();
         ++it2) {
      if (it1.key() != it2.key() && it1.value() == it2.value()) {
        _shortcut_clash_ids.insert(it1.key());
        _shortcut_clash_ids.insert(it2.key());
      }
    }
  }

  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);

  for (int i = 0; i < infoCount; i++) {
    QListWidgetItem *item = _shortcut_list->item(i);
    QWidget *w = _shortcut_list->itemWidget(item);
    if (!w)
      continue;

    QLabel *clashIcon = w->findChild<QLabel *>("clash_icon");
    if (clashIcon) {
      if (_shortcut_clash_ids.contains(infos[i].actionId)) {
        clashIcon->setText("!");
        clashIcon->setStyleSheet(
            "color: #ff6600; font-weight: bold; font-size: 14px;");
        clashIcon->show();
      } else {
        clashIcon->hide();
      }
    }
  }

  if (!_shortcut_clash_ids.isEmpty()) {
    _clash_warning_label->setText(L_S(STR_PAGE_DLG,
                                      S_ID(IDS_DLG_SC_CLASH_WARNING),
                                      "Clashing shortcuts will be ignored."));
    _clash_warning_label->setStyleSheet("color: #ff6600; padding-left: 5px;");
    _clash_warning_label->show();
  } else {
    _clash_warning_label->hide();
  }
}

void ApplicationParamDlg::updateShortcutButtons() {
  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);

  bool hasSelection =
      _shortcut_selected_row >= 0 && _shortcut_selected_row < infoCount;
  bool isEditing = false;
  bool isModified = false;
  bool isDefault = false;
  bool hasKey = false;

  if (hasSelection) {
    int actionId = infos[_shortcut_selected_row].actionId;
    QString currentKey = _shortcut_keys[actionId];
    QString originalKey = _shortcut_original_keys[actionId];
    QString defaultKey;

    for (int i = 0; i < infoCount; i++) {
      if (infos[i].actionId == actionId) {
        defaultKey = QString(infos[i].keySequence);
        break;
      }
    }

    QListWidgetItem *item = _shortcut_list->item(_shortcut_selected_row);
    QWidget *w = _shortcut_list->itemWidget(item);
    if (w) {
      ShortcutKeyCapture *capture = w->findChild<ShortcutKeyCapture *>();
      isEditing = (capture != nullptr && capture->hasFocus());
    }

    isModified = (currentKey != originalKey);
    isDefault = (currentKey != defaultKey);
    hasKey = !currentKey.isEmpty();
  }

  _btn_accept->setEnabled(hasSelection && isEditing);
  _btn_restore->setEnabled(hasSelection && isModified);
  _btn_reset_default->setEnabled(hasSelection && isDefault);
  _btn_delete->setEnabled(hasSelection && hasKey);
}

void ApplicationParamDlg::refreshShortcutList() {
  if (!_shortcut_list)
    return;

  int infoCount = 0;
  const ShortcutActionInfo *infos = GetShortcutActionInfos(&infoCount);

  for (int i = 0; i < infoCount; i++) {
    QListWidgetItem *item = _shortcut_list->item(i);
    QWidget *w = _shortcut_list->itemWidget(item);
    if (!w)
      continue;

    QLabel *keyLabel = w->findChild<QLabel *>("shortcut_key_label");
    ShortcutKeyCapture *capture = w->findChild<ShortcutKeyCapture *>();

    if (capture && i != _shortcut_selected_row) {
      QHBoxLayout *lay = qobject_cast<QHBoxLayout *>(w->layout());
      if (lay) {
        int idx = lay->indexOf(capture);
        if (idx >= 0) {
          lay->removeWidget(capture);
          capture->deleteLater();

          QLabel *newLabel =
              new QLabel(_shortcut_keys[infos[i].actionId].isEmpty()
                             ? ""
                             : _shortcut_keys[infos[i].actionId]);
          newLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
          newLabel->setObjectName("shortcut_key_label");
          lay->insertWidget(idx, newLabel, 1);
        }
      }
    } else if (keyLabel) {
      keyLabel->setText(_shortcut_keys[infos[i].actionId].isEmpty()
                            ? ""
                            : _shortcut_keys[infos[i].actionId]);
    } else if (capture && i == _shortcut_selected_row) {
      capture->setKeySequence(_shortcut_keys[infos[i].actionId]);
    }
  }

  checkShortcutClash();
}

void ApplicationParamDlg::onResetStyle() {
  _style_tokens = _default_style_tokens;
  refreshStyleWidgets();
  scheduleLivePreview();
}

void ApplicationParamDlg::onExportStyle() {
  QString filePath = QFileDialog::getSaveFileName(
      nullptr, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_EXPORT), "Export"),
      QString(),
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_FILE_FILTER),
          "Style Files (*.pxstyle)"));
  if (filePath.isEmpty())
    return;

  if (!filePath.endsWith(".pxstyle"))
    filePath += ".pxstyle";

  QJsonObject root;
  root["version"] = 1;
  root["style"] = AppConfig::Instance().frameOptions.style;

  QJsonObject tokensObj;
  QStringList tokenNames = _style_tokens.keys();
  std::sort(tokenNames.begin(), tokenNames.end());
  for (const QString &name : tokenNames) {
    QString value = _style_tokens[name];
    QString defaultValue = _default_style_tokens.value(name, "");
    if (value != defaultValue) {
      tokensObj[name] = value;
    }
  }
  root["tokens"] = tokensObj;

  QJsonDocument doc(root);
  QFile file(filePath);
  if (file.open(QFile::WriteOnly | QFile::Text)) {
    file.write(doc.toJson());
    file.close();
  }
}

void ApplicationParamDlg::onImportStyle() {
  QString filePath = QFileDialog::getOpenFileName(
      nullptr, L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_IMPORT), "Import"),
      QString(),
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_STYLE_FILE_FILTER),
          "Style Files (*.pxstyle)"));
  if (filePath.isEmpty())
    return;

  QFile file(filePath);
  if (!file.open(QFile::ReadOnly | QFile::Text))
    return;

  QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  file.close();

  if (!doc.isObject())
    return;

  QJsonObject root = doc.object();
  if (root["version"].toInt() != 1)
    return;

  QJsonObject tokensObj = root["tokens"].toObject();

  _style_tokens = _default_style_tokens;

  for (auto it = tokensObj.constBegin(); it != tokensObj.constEnd(); ++it) {
    if (_style_tokens.contains(it.key())) {
      _style_tokens[it.key()] = it.value().toString();
    }
  }

  refreshStyleWidgets();
  scheduleLivePreview();
}

QString ApplicationParamDlg::getShortcutKey(int actionId) {
  if (_shortcut_keys.contains(actionId))
    return _shortcut_keys[actionId];
  return QString();
}

void ApplicationParamDlg::setShortcutKey(int actionId, const QString &keySeq) {
  _shortcut_keys[actionId] = keySeq;
}

void ApplicationParamDlg::refreshStyleWidgets() {
  for (auto it = _style_tokens.begin(); it != _style_tokens.end(); ++it) {
    QString tokenName = it.key();
    QString value = it.value();

    if (_style_button_widgets.contains(tokenName)) {
      _style_button_widgets[tokenName]->setText(value);
    }
    if (_style_preview_widgets.contains(tokenName)) {
      QColor c(value);
      if (c.isValid()) {
        _style_preview_widgets[tokenName]->setStyleSheet(
            QString("background-color: %1; border: 1px solid #555; "
                    "border-radius: 4px;")
                .arg(value));
      } else {
        _style_preview_widgets[tokenName]->setStyleSheet(
            "background-color: transparent; border: 1px solid #555; "
            "border-radius: 4px;");
      }
    }
    if (_style_line_edit_widgets.contains(tokenName)) {
      _style_line_edit_widgets[tokenName]->setText(value);
    }
    if (_style_checkbox_widgets.contains(tokenName)) {
      _style_checkbox_widgets[tokenName]->blockSignals(true);
      _style_checkbox_widgets[tokenName]->setChecked(value == "true");
      _style_checkbox_widgets[tokenName]->blockSignals(false);
    }
  }
}

void ApplicationParamDlg::saveDisplayOptions() {
  AppConfig &app = AppConfig::Instance();
  bool bAppChanged = false;

  if (app.appOptions.quickScroll != _ck_quickScroll->isChecked()) {
    app.appOptions.quickScroll = _ck_quickScroll->isChecked();
    bAppChanged = true;
  }
  if (app.appOptions.trigPosDisplayInMid != _ck_trigInMid->isChecked()) {
    app.appOptions.trigPosDisplayInMid = _ck_trigInMid->isChecked();
    bAppChanged = true;
  }
  if (app.appOptions.displayProfileInBar != _ck_profileBar->isChecked()) {
    app.appOptions.displayProfileInBar = _ck_profileBar->isChecked();
    bAppChanged = true;
  }
  if (app.appOptions.swapBackBufferAlways != _ck_abortData->isChecked()) {
    app.appOptions.swapBackBufferAlways = _ck_abortData->isChecked();
    bAppChanged = true;
  }
  if (app.appOptions.autoScrollLatestData !=
      _ck_autoScrollLatestData->isChecked()) {
    app.appOptions.autoScrollLatestData = _ck_autoScrollLatestData->isChecked();
    bAppChanged = true;
  }

  if (bAppChanged) {
    app.SaveApp();
    AppControl::Instance()->GetSession()->broadcast_msg(
        DSV_MSG_APP_OPTIONS_CHANGED);
  }
}

void ApplicationParamDlg::saveShortcutOptions() {
  AppConfig &app = AppConfig::Instance();

  QList<ShortcutItem> newItems;
  QMap<int, QString>::const_iterator it;
  for (it = _shortcut_keys.constBegin(); it != _shortcut_keys.constEnd();
       ++it) {
    ShortcutItem item;
    item.actionId = it.key();
    item.keySequence = it.value();
    newItems.append(item);
  }

  app.shortcutOptions.items = newItems;
  app.SaveShortcuts();
  app.flushPendingSaves();
  AppControl::Instance()->GetSession()->broadcast_msg(DSV_MSG_SHORTCUT_CHANGED);

  _shortcut_original_keys = _shortcut_keys;
  updateShortcutButtons();
}

void ApplicationParamDlg::saveStyleOptions() {
  AppConfig &app = AppConfig::Instance();
  bool bChanged = false;

  QList<StyleTokenItem> newItems;
  QStringList tokenNames = _style_tokens.keys();
  std::sort(tokenNames.begin(), tokenNames.end());

  for (int i = 0; i < tokenNames.size(); i++) {
    QString name = tokenNames[i];
    QString value = _style_tokens[name];
    QString defaultValue = _default_style_tokens.value(name, "");

    if (value != defaultValue) {
      StyleTokenItem item;
      item.tokenName = name;
      item.value = value;
      newItems.append(item);
    }
  }

  if (app.styleOptions.items.size() != newItems.size()) {
    bChanged = true;
  } else {
    for (int i = 0; i < newItems.size(); i++) {
      bool found = false;
      for (int j = 0; j < app.styleOptions.items.size(); j++) {
        if (app.styleOptions.items[j].tokenName == newItems[i].tokenName) {
          if (app.styleOptions.items[j].value != newItems[i].value) {
            bChanged = true;
          }
          found = true;
          break;
        }
      }
      if (!found) {
        bChanged = true;
      }
      if (bChanged)
        break;
    }
  }

  if (bChanged) {
    app.styleOptions.items = newItems;

    QString style = app.frameOptions.style;
    QString qssRes = ":/theme.qss";
    QFile qss(qssRes);
    if (qss.open(QFile::ReadOnly | QFile::Text)) {
      QString qssContent = qss.readAll();
      qss.close();

      QHash<QString, QString> tokens;
      QRegularExpression tokenRe(
          "@([\\w-]+):\\s*([^\\r\\n]+?)\\s*(?:\\*/|\\r|\\n)");
      QRegularExpressionMatchIterator it = tokenRe.globalMatch(qssContent);
      while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString tokenName = "@" + match.captured(1);
        QString tokenValue = match.captured(2).trimmed();
        tokens[tokenName] = tokenValue;
      }

      QMap<QString, QString>::const_iterator styleIt;
      for (styleIt = _style_tokens.constBegin();
           styleIt != _style_tokens.constEnd(); ++styleIt) {
        tokens[styleIt.key()] = styleIt.value();
      }

      QList<QString> keys = tokens.keys();
      std::sort(keys.begin(), keys.end(),
                [](const QString &a, const QString &b) {
                  return a.length() > b.length();
                });

      for (const QString &key : keys) {
        qssContent.replace(key, tokens[key]);
      }

      // Process SVG files that contain token placeholders (e.g. @accent)
      QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                        "/pxview_themed_svgs";
      QDir().mkpath(tempDir);

      QRegularExpression svgRe2("image:\\s*url\\((:[^)]+\\.svg)\\)");
      QRegularExpressionMatchIterator svgIt2 = svgRe2.globalMatch(qssContent);
      QSet<QString> processedSvgs2;
      while (svgIt2.hasNext()) {
        QRegularExpressionMatch match = svgIt2.next();
        QString svgResPath = match.captured(1);

        if (processedSvgs2.contains(svgResPath))
          continue;
        processedSvgs2.insert(svgResPath);

        QFile svgFile(svgResPath);
        if (!svgFile.open(QFile::ReadOnly | QFile::Text))
          continue;
        QString svgContent = svgFile.readAll();
        svgFile.close();

        bool hasPlaceholders = false;
        for (const QString &key : keys) {
          if (svgContent.contains(key)) {
            hasPlaceholders = true;
            break;
          }
        }
        if (!hasPlaceholders)
          continue;

        for (const QString &key : keys) {
          svgContent.replace(key, tokens[key]);
        }

        QString fileName = svgResPath;
        fileName.replace(":/", "");
        fileName.replace("/", "_");
        QString tempPath = tempDir + "/" + fileName;
        QFile tempFile(tempPath);
        if (tempFile.open(QFile::WriteOnly | QFile::Text)) {
          tempFile.write(svgContent.toUtf8());
          tempFile.close();
        }

        qssContent.replace(svgResPath, tempPath);
      }

      app.SetThemeTokens(tokens);
      qApp->setStyleSheet(qssContent);
      UiManager::Instance()->Update(UI_UPDATE_ACTION_THEME);
      UiManager::Instance()->Update(UI_UPDATE_ACTION_FONT);
    }

    app.SaveStyle();
    AppControl::Instance()->GetSession()->broadcast_msg(DSV_MSG_STYLE_CHANGED);
  }
}

bool ApplicationParamDlg::ShowDlg(QWidget *parent) {
  DSDialog dlg(parent, true, false);
  dlg.setTitle(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_SETTINGS), "Settings"));
  dlg.setMinimumSize(520, 420);

  QHBoxLayout *mainLay = new QHBoxLayout();
  mainLay->setContentsMargins(0, 10, 0, 0);
  mainLay->setSpacing(0);

  _nav_list = new QListWidget();
  _nav_list->setFixedWidth(120);
  _nav_list->setObjectName("settingsNavList");
  _nav_list->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_NAV_DISPLAY), "Display"));
  _nav_list->addItem(
      L_S(STR_PAGE_DLG, S_ID(IDS_DLG_NAV_SHORTCUTS), "Shortcuts"));
  _nav_list->addItem(L_S(STR_PAGE_DLG, S_ID(IDS_DLG_NAV_STYLE), "Style"));
  _nav_list->setCurrentRow(0);
  _nav_list->setFocusPolicy(Qt::NoFocus);

  class NavListDelegate : public QStyledItemDelegate {
  public:
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
      painter->save();
      painter->setRenderHint(QPainter::Antialiasing);

      bool isSelected = option.state & QStyle::State_Selected;
      bool isHovered = option.state & QStyle::State_MouseOver;

      // Draw rounded background for hover/selected
      QRect rect = option.rect.adjusted(2, 2, -2, -2);
      if (isSelected || isHovered) {
        bool isDark = AppConfig::Instance().IsDarkStyle();
        int rgb = isDark ? 255 : 0;
        int alpha = isSelected ? 15 : 8;
        QColor bgColor = QColor(rgb, rgb, rgb, alpha);
        painter->setPen(Qt::NoPen);
        painter->setBrush(bgColor);
        painter->drawRoundedRect(rect, 4, 4);
      }

      // Draw half-height pill indicator for selected item
      if (isSelected) {
        QColor accent = AppConfig::Instance().GetThemeColor("@accent");
        if (!accent.isValid()) accent = QColor("#5b8def");
        
        int pillHeight = 16;
        int pillY = rect.y() + (rect.height() - pillHeight) / 2;
        QRect pillRect(rect.x() + 2, pillY, 2, pillHeight);
        
        painter->setPen(Qt::NoPen);
        painter->setBrush(accent);
        painter->drawRect(pillRect);
      }

      // Draw text
      QColor fg = AppConfig::Instance().GetThemeColor(isSelected ? "@fg-bright" : "@fg-base");
      painter->setPen(fg.isValid() ? fg : (isSelected ? QColor("#ffffff") : QColor("#eff0f1")));
      
      QRect textRect = rect.adjusted(12, 0, 0, 0);
      painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, index.data().toString());

      painter->restore();
    }
  };
  _nav_list->setItemDelegate(new NavListDelegate());

  _page_stack = new QStackedWidget();
  _page_stack->addWidget(createDisplayPage());
  _page_stack->addWidget(createShortcutPage());
  _page_stack->addWidget(createStylePage());

  QObject::connect(_nav_list, &QListWidget::currentRowChanged, _page_stack,
                   &QStackedWidget::setCurrentIndex);

  mainLay->addWidget(_nav_list);
  mainLay->addWidget(_page_stack, 1);

  QVBoxLayout *rootLay = new QVBoxLayout();
  rootLay->addLayout(mainLay);

  QDialogButtonBox *btnBox =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                           QDialogButtonBox::Apply);
  rootLay->addWidget(btnBox);
  dlg.layout()->addLayout(rootLay);

  QObject::connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QObject::connect(btnBox->button(QDialogButtonBox::Apply),
                   &QPushButton::clicked, [this]() {
                     this->saveDisplayOptions();
                     this->saveShortcutOptions();
                     this->saveStyleOptions();
                     this->applyLivePreview();
                     this->_original_style_tokens = this->_style_tokens;
                   });

  dlg.exec();
  bool ret = dlg.IsClickYes();

  if (ret) {
    saveDisplayOptions();
    saveShortcutOptions();
    saveStyleOptions();
    applyLivePreview();
  } else {
    _style_tokens = _original_style_tokens;
    // If they cancelled, we only need to revert UI if it was changed
    // but since we removed live preview on typing, the UI hasn't changed.
    // Calling applyLivePreview here is still safe and handles the case
    // where we might add live preview back later or for other tabs.
    applyLivePreview();
  }

  return ret;
}

} // namespace dialogs
} // namespace pv
