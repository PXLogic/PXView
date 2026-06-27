/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2026 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "draggabletabwidget.h"
#include "draggabletabbar.h"
#include "../submainframe.h"
#include "../config/appconfig.h"
#include "../sessionmanager.h"
#include "../tabcontext.h"

#include <QPushButton>
#include <QHBoxLayout>
#include <QStyle>
#include <QLineEdit>
#include <QTimer>
#include <QResizeEvent>
#include <QPaintEvent>
#include <QStylePainter>
#include <QStackedWidget>

namespace pv {
namespace ui {

class TabCloseButton : public QAbstractButton {
public:
    explicit TabCloseButton(QWidget *parent = nullptr) : QAbstractButton(parent), _hovered(false), _pressed(false) {
        setFixedSize(16, 16);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(tr("Close"));
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (_pressed) {
            QString pressColorStr = AppConfig::Instance().GetThemeTokenValue("@bg-overlay");
            QColor pressColor(pressColorStr);
            if (!pressColor.isValid()) pressColor = QColor(128, 128, 128, 80);
            p.setBrush(pressColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(rect(), 4, 4);
        } else if (_hovered) {
            QString hoverColorStr = AppConfig::Instance().GetThemeTokenValue("@bg-overlay");
            QColor hoverColor(hoverColorStr);
            if (!hoverColor.isValid()) hoverColor = QColor(128, 128, 128, 50);
            p.setBrush(hoverColor);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(rect(), 4, 4);
        }

        QString fgColorStr = AppConfig::Instance().GetThemeTokenValue("@fg-muted");
        QColor fgColor(fgColorStr);
        if (!fgColor.isValid()) fgColor = QColor(150, 150, 150);

        if (_hovered || _pressed) {
            QString hoverFgStr = AppConfig::Instance().GetThemeTokenValue("@fg-bright");
            QColor hoverFg(hoverFgStr);
            if (hoverFg.isValid()) fgColor = hoverFg;
            else fgColor = Qt::white;
        }

        QPen pen(fgColor, 1.2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);

        int margin = 5;
        p.drawLine(margin, margin, width() - margin, height() - margin);
        p.drawLine(width() - margin, margin, margin, height() - margin);
    }

    void enterEvent(QEnterEvent *event) override {
        _hovered = true;
        update();
        QAbstractButton::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        _hovered = false;
        update();
        QAbstractButton::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            _pressed = true;
            update();
        }
        QAbstractButton::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            _pressed = false;
            update();
        }
        QAbstractButton::mouseReleaseEvent(e);
    }

private:
    bool _hovered;
    bool _pressed;
};

DraggableTabWidget::DraggableTabWidget(QWidget *parent)
    : QTabWidget(parent)
{
    _draggable_tab_bar = new DraggableTabBar(this);
    setTabBar(_draggable_tab_bar);

    // Ensure QSS fully overrides native frame drawing (fixes Linux Fusion style white line)
    setAttribute(Qt::WA_StyledBackground, true);

    // Add left padding to the content pane so the tab bar can extend
    // to the left edge while the content area stays offset
    if (auto *pane = findChild<QStackedWidget*>()) {
        pane->setContentsMargins(10, 0, 0, 0);
        // Force styled background so QSS border:none overrides native Fusion frame on Linux
        pane->setAttribute(Qt::WA_StyledBackground, true);
    }

    setTabsClosable(true);
    setMovable(false);
    setDocumentMode(false); // Do not use document mode on Linux, it draws native separators

    connect(_draggable_tab_bar, &DraggableTabBar::detachTab,
            this, &DraggableTabWidget::onDetachTab);
    connect(_draggable_tab_bar, &DraggableTabBar::tabMoveRequested,
            this, &DraggableTabWidget::onTabMoveRequested);
    connect(this, &QTabWidget::tabCloseRequested,
            this, &DraggableTabWidget::onTabCloseRequested);
    connect(_draggable_tab_bar, &DraggableTabBar::tabRenameRequested,
            this, &DraggableTabWidget::onTabRenameRequested);
    connect(_draggable_tab_bar, &DraggableTabBar::tabRenameRequested,
            this, &DraggableTabWidget::tabRenameRequested);
    connect(_draggable_tab_bar, &DraggableTabBar::tabCloseOthersRequested,
            this, &DraggableTabWidget::tabCloseOthersRequested);
    connect(_draggable_tab_bar, &DraggableTabBar::tabCloseRightRequested,
            this, &DraggableTabWidget::tabCloseRightRequested);

    _add_button = new QPushButton(QStringLiteral("+"), this);
    _add_button->setFixedSize(QSize(24, 24));
    _add_button->setToolTip(tr("New Tab"));
    _add_button->setCursor(Qt::PointingHandCursor);
    QString addBtnColor = AppConfig::Instance().GetThemeTokenValue("@fg-muted");
    QString addBtnHoverColor = AppConfig::Instance().GetThemeTokenValue("@fg-bright");
    if (addBtnColor.isEmpty()) addBtnColor = "#aaa";
    if (addBtnHoverColor.isEmpty()) addBtnHoverColor = "#fff";
    _add_button->setStyleSheet(
        QStringLiteral("QPushButton {"
            "border: none;"
            "font-size: 20px;"
            "font-weight: normal;"
            "padding: 0px;"
            "color: %1;"
            "background: transparent;"
            "}"
            "QPushButton:hover {"
            "color: %2;"
            "}").arg(addBtnColor, addBtnHoverColor));
    _add_button->raise();
    _add_button->show();

    _tab_bottom_line = new QWidget(this);
    _tab_bottom_line->setObjectName("TabBottomLine");
    _tab_bottom_line->setFixedHeight(1);
    _tab_bottom_line->setAttribute(Qt::WA_StyledBackground, true);
    _tab_bottom_line->raise();
    _tab_bottom_line->show();

    connect(_add_button, &QPushButton::clicked,
            this, &DraggableTabWidget::newTabRequested);

    QTimer::singleShot(0, this, [this]() { update_add_button_position(); });
}

int DraggableTabWidget::addTab(QWidget *page, const QString &label)
{
    int result = QTabWidget::addTab(page, label);
    QTimer::singleShot(0, this, [this]() { update_add_button_position(); });
    return result;
}

int DraggableTabWidget::addTab(QWidget *page, const QIcon &icon, const QString &label)
{
    int result = QTabWidget::addTab(page, icon, label);
    QTimer::singleShot(0, this, [this]() { update_add_button_position(); });
    return result;
}

void DraggableTabWidget::update_add_button_position()
{
    if (count() == 0) {
        _add_button->move(4, 2);
    } else {
        QRect last_tab_rect = _draggable_tab_bar->tabRect(count() - 1);
        QPoint tab_bar_pos = _draggable_tab_bar->mapToParent(QPoint(0, 0));
        int x = tab_bar_pos.x() + last_tab_rect.right() + 4;
        int y = tab_bar_pos.y() + (_draggable_tab_bar->height() - _add_button->height()) / 2;
        _add_button->move(x, y);
        _add_button->raise();
    }

    if (_tab_bottom_line && _draggable_tab_bar) {
        int lineY = _draggable_tab_bar->geometry().bottom();
        _tab_bottom_line->setGeometry(0, lineY, width(), 1);
        _tab_bottom_line->raise();
    }
}

void DraggableTabWidget::tabInserted(int index)
{
    QTabWidget::tabInserted(index);
    
    TabCloseButton *btn = new TabCloseButton(this);
    connect(btn, &QAbstractButton::clicked, this, [this, btn]() {
        int idx = -1;
        for (int i = 0; i < count(); ++i) {
            if (tabBar()->tabButton(i, QTabBar::RightSide) == btn) {
                idx = i;
                break;
            }
        }
        if (idx != -1) {
            emit tabCloseRequested(idx);
        }
    });
    tabBar()->setTabButton(index, QTabBar::RightSide, btn);
    
    QTimer::singleShot(0, this, [this]() { update_add_button_position(); });
}

void DraggableTabWidget::tabRemoved(int index)
{
    QTabWidget::tabRemoved(index);
    QTimer::singleShot(0, this, [this]() { update_add_button_position(); });
}

void DraggableTabWidget::resizeEvent(QResizeEvent *event)
{
    QTabWidget::resizeEvent(event);
    update_add_button_position();
}

void DraggableTabWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
}

void DraggableTabWidget::onDetachTab(int index, const QPoint &dropPos)
{
    if (index < 0 || index >= count())
        return;

    QWidget *widget = this->widget(index);
    QString title = tabText(index);

    removeTab(index);

    SubMainFrame *floating_window = new SubMainFrame(widget, title);
    connect(floating_window, &SubMainFrame::windowClosed,
            this, &DraggableTabWidget::onDetachedWindowClosed);

    _detached_windows.append(QPointer<SubMainFrame>(floating_window));

    QPoint window_pos = dropPos - QPoint(100, 20);
    floating_window->showAt(window_pos, QSize(800, 600));

    emit tabDetached(index, widget, title);
}

void DraggableTabWidget::onTabCloseRequested(int index)
{
    if (index < 0 || index >= count())
        return;

    emit tabCloseRequested(index);
}

void DraggableTabWidget::onTabRenameRequested(int index)
{
    if (index < 0 || index >= count())
        return;

    QRect tab_rect = _draggable_tab_bar->tabRect(index);
    QLineEdit *editor = new QLineEdit(_draggable_tab_bar);
    editor->setGeometry(tab_rect.adjusted(2, 2, -2, -2));
    editor->setText(tabText(index));
    editor->selectAll();
    editor->setFocus();

    connect(editor, &QLineEdit::returnPressed, this, [this, editor, index]() {
        QString new_title = editor->text();
        setTabText(index, new_title);
        emit tabRenamed(index, new_title);
        editor->setParent(nullptr);
        editor->deleteLater();
    });

    connect(editor, &QLineEdit::editingFinished, this, [this, editor, index]() {
        if (editor->parent() != nullptr) {
            QString new_title = editor->text();
            setTabText(index, new_title);
            emit tabRenamed(index, new_title);
            editor->setParent(nullptr);
            editor->deleteLater();
        }
    });

    editor->show();
}

void DraggableTabWidget::onDetachedWindowClosed(QWidget *content, const QString &title)
{
    int idx = addTab(content, title);
    emit tabAttached(content, title);
    setCurrentIndex(idx);

    SubMainFrame *window = qobject_cast<SubMainFrame*>(sender());
    if (window) {
        _detached_windows.removeOne(QPointer<SubMainFrame>(window));
        window->deleteLater();
    }
}

void DraggableTabWidget::closeAllDetachedWindows()
{
    for (auto &ptr : _detached_windows) {
        if (ptr) {
            ptr->close();
        }
    }
    _detached_windows.clear();
}

void DraggableTabWidget::onTabMoveRequested(int from, int to)
{
    if (from < 0 || from >= count() || to < 0 || to > count() || from == to)
        return;
        
    QWidget *w = widget(from);
    QString text = tabText(from);
    QIcon icon = tabIcon(from);
    QString toolTip = tabToolTip(from);
    
    bool old_block = blockSignals(true);
    
    removeTab(from);
    
    int insert_idx = to;
    if (to > from) {
        insert_idx = to - 1; 
    }
    
    insertTab(insert_idx, w, icon, text);
    setTabToolTip(insert_idx, toolTip);
    
    SessionManager::instance()->move_context(from, insert_idx);
    
    blockSignals(old_block);
    
    emit tabMoved(from, insert_idx);
    
    setCurrentIndex(insert_idx);
}

} // namespace ui
} // namespace pv
