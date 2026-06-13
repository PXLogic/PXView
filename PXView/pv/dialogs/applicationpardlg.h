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

#pragma once

#include <QObject>
#include <QWidget>
#include <QDialog>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QFileSystemWatcher>
#include <QStringList>
#include <QListWidget>
#include <QMap>
#include <QSet>
#include <QLineEdit>

class QComboBox;
class DsComboBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QCheckBox;
class QTreeWidget;
class QTreeWidgetItem;

class ShortcutKeyCapture : public QLineEdit
{
    Q_OBJECT

public:
    explicit ShortcutKeyCapture(QWidget *parent = nullptr);

    void setKeySequence(const QString &key);
    QString keySequence() const;

signals:
    void keySequenceChanged(const QString &newKey);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    QString m_keySeq;
    bool m_capturing;
};

namespace pv
{
namespace dialogs
{
    class ApplicationParamDlg
    { 
    public:
        ApplicationParamDlg();
        ~ApplicationParamDlg();

        bool ShowDlg(QWidget *parent);

    private:
        QWidget* createDisplayPage();
        QWidget *createShortcutPage();
        QWidget *createStylePage();

        void saveDisplayOptions();
        void saveShortcutOptions();
        void saveStyleOptions();

        void onShortcutRowSelected(int row);
        void onShortcutKeyCaptured(int row, const QString &newKey);
        void onShortcutAccept();
        void onShortcutRestore();
        void onShortcutResetDefault();
        void onShortcutDelete();
        void onResetShortcuts();
        void checkShortcutClash();
        void updateShortcutButtons();
        void refreshShortcutList();
        void onStyleCategoryChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
        void onStyleTokenColorChanged(const QString &tokenName);
        void onStyleTokenTextChanged(const QString &tokenName, const QString &value);
        void onStyleTokenBoolChanged(const QString &tokenName, bool checked);
        void onResetStyle();
        void onExportStyle();
        void onImportStyle();
        void onWidgetPicked(QWidget *w);
        void scheduleLivePreview();
        void applyLivePreview();
        
        void applyPresetTheme(const QString &path);
        void openExternalJsonEditor();
        void onExternalJsonChanged(const QString &path);

        QString getShortcutKey(int actionId);
        void setShortcutKey(int actionId, const QString &keySeq);
        void refreshStyleWidgets();

    private:
        QListWidget *_nav_list;
        QStackedWidget *_page_stack;

        QCheckBox *_ck_quickScroll;
        QCheckBox *_ck_trigInMid;
        QCheckBox *_ck_profileBar;
        QCheckBox *_ck_abortData;
        QCheckBox *_ck_autoScrollLatestData;
        QListWidget *_shortcut_list;
        int _shortcut_selected_row;
        QPushButton *_btn_accept;
        QPushButton *_btn_restore;
        QPushButton *_btn_reset_default;
        QPushButton *_btn_delete;
        QLabel *_clash_warning_label;

        QTreeWidget *_style_category_tree;
        QStackedWidget *_style_page_stack;
        QMap<QString, QString> _style_tokens;
        QMap<QString, QString> _default_style_tokens;
        QMap<QString, QString> _original_style_tokens;
        QMap<QString, QWidget*> _style_preview_widgets;
        QMap<QString, QPushButton*> _style_button_widgets;
        QMap<QString, class QLineEdit*> _style_line_edit_widgets;
        QMap<QString, QCheckBox*> _style_checkbox_widgets;
        DsComboBox *_preset_combo;
        
        class QTimer *_live_preview_timer;
        ::QFileSystemWatcher *_file_watcher;
        QString _external_json_path;

        QMap<int, QString> _shortcut_keys;
        QMap<int, QString> _shortcut_original_keys;
        QSet<int> _shortcut_clash_ids;
    };

}//
}//
