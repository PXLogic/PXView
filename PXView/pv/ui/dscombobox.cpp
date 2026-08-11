#include "pv/ui/dscombobox.h"
#include <QFontMetrics>
#include <QGuiApplication>
#include <QScreen>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QLibrary>
#include <QTimer>
#include <QHideEvent>
#include <QCloseEvent>
#include <QMouseEvent>
#include "pv/config/appconfig.h"
#include "pv/ui/dockfonts.h"
#include "pv/widgets/smoothscrollarea.h"
#include "pv/base/log.h"

#ifdef WIN32
#include <windows.h>
#endif

static const char* event_type_name(QEvent::Type t)
{
    switch (t) {
        case QEvent::Show:              return "Show";
        case QEvent::Hide:              return "Hide";
        case QEvent::Close:             return "Close";
        case QEvent::ActivationChange:   return "ActivationChange";
        case QEvent::WindowActivate:    return "WindowActivate";
        case QEvent::WindowDeactivate:  return "WindowDeactivate";
        case QEvent::FocusIn:           return "FocusIn";
        case QEvent::FocusOut:          return "FocusOut";
        case QEvent::MouseButtonPress:   return "MouseButtonPress";
        case QEvent::MouseButtonRelease: return "MouseButtonRelease";
        case QEvent::Resize:            return "Resize";
        case QEvent::Move:              return "Move";
        default:                        return "Other";
    }
}

DsComboPopup::DsComboPopup(QComboBox *combo, QWidget *parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("dsComboPopup");
    _combo = combo;
    _id = 0;

    int w = combo->width();
    int maxH = 400;

    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(0);

    QWidget *listPanel = new QWidget(this);
    QVBoxLayout *listLay = new QVBoxLayout(listPanel);
    listLay->setContentsMargins(2, 2, 2, 2);
    listLay->setSpacing(0);
    listLay->setAlignment(Qt::AlignTop);

    QFont font = dock_font_content();
    int curIndex = combo->currentIndex();
    int itemH = 0;

    for (int i = 0; i < combo->count(); i++) {
        QPushButton *bt = new QPushButton(combo->itemText(i), listPanel);
        bt->setObjectName("flat");
        bt->setFont(font);
        bt->setMinimumWidth(w - 8);
        bt->setMaximumWidth(w - 8);

        if (i == curIndex) {
            bt->setProperty("current", true);
        }

        connect(bt, &QPushButton::clicked, this, &DsComboPopup::on_item_clicked);
        _itemButtons.push_back(bt);
        listLay->addWidget(bt);

        if (itemH == 0) {
            itemH = bt->sizeHint().height();
        }
    }

    int totalH = combo->count() * itemH + 8;
    if (totalH > maxH)
        totalH = maxH;
    if (totalH < itemH + 8)
        totalH = itemH + 8;

    pv::widgets::SmoothScrollArea *scroll = new pv::widgets::SmoothScrollArea(this);
    scroll->setWidget(listPanel);
    scroll->setObjectName("dock_search_combo_scroll");
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedSize(w, totalH);
    scroll->setLongTailAnimation(true);

    mainLay->addWidget(scroll);

    this->setFixedSize(w, totalH);

    QPoint gp = combo->mapToGlobal(QPoint(0, combo->height()));
    QScreen *screen = QGuiApplication::screenAt(gp);
    if (screen) {
        QRect screenGeom = screen->availableGeometry();
        if (gp.y() + totalH > screenGeom.bottom()) {
            gp.setY(combo->mapToGlobal(QPoint(0, 0)).y() - totalH);
        }
    }
    this->move(gp);

    if (curIndex >= 0 && curIndex < _itemButtons.size()) {
        QPushButton *curBt = _itemButtons[curIndex];
        scroll->ensureWidgetVisible(curBt);
    }

    // 安装事件过滤器捕获所有关闭路径
    this->installEventFilter(this);

    pxv_info("[DsComboPopup] constructor: this=%p combo=%p items=%d",
             this, combo, combo->count());
}

void DsComboPopup::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange) {
        bool active = this->isActiveWindow();
        pxv_info("[DsComboPopup#%d] changeEvent ActivationChange: active=%d _bReady=%d",
                 _id, active, _bReady);

        if (_bReady && !active) {
            pxv_info("[DsComboPopup#%d] changeEvent: closing due to activation loss", _id);
            this->close();
            return;
        }
    }
    QDialog::changeEvent(event);
}

void DsComboPopup::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    pxv_info("[DsComboPopup#%d] showEvent: isVisible=%d isActive=%d",
             _id, this->isVisible(), this->isActiveWindow());

#ifdef WIN32
    const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
    const DWORD DWMWCP_DONOTROUND = 1;
    using tDwmSetWindowAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    tDwmSetWindowAttribute pDwmSetWindowAttribute =
        tDwmSetWindowAttribute(QLibrary::resolve("dwmapi", "DwmSetWindowAttribute"));
    if (pDwmSetWindowAttribute) {
        HWND hwnd = (HWND)this->winId();
        DWORD preference = DWMWCP_DONOTROUND;
        pDwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
    }
#endif

    _bReady = false;
    QTimer::singleShot(0, this, [this]() {
        _bReady = true;
        pxv_info("[DsComboPopup#%d] _bReady timer fired: _bReady=true", _id);
    });
}

void DsComboPopup::hideEvent(QHideEvent *event)
{
    pxv_info("[DsComboPopup#%d] hideEvent: isVisible=%d", _id, this->isVisible());
    QDialog::hideEvent(event);
}

void DsComboPopup::closeEvent(QCloseEvent *event)
{
    pxv_info("[DsComboPopup#%d] closeEvent", _id);
    QDialog::closeEvent(event);
}

bool DsComboPopup::eventFilter(QObject *watched, QEvent *event)
{
    // 捕获所有可能关闭弹窗的事件
    QEvent::Type t = event->type();
    if (t == QEvent::Close || t == QEvent::Hide ||
        t == QEvent::WindowDeactivate || t == QEvent::FocusOut ||
        t == QEvent::MouseButtonPress || t == QEvent::MouseButtonRelease) {
        pxv_info("[DsComboPopup#%d] eventFilter: watched=%p type=%s",
                 _id, watched, event_type_name(t));
    }
    return QDialog::eventFilter(watched, event);
}

void DsComboPopup::on_item_clicked()
{
    QPushButton *bt = qobject_cast<QPushButton *>(sender());
    if (!bt || !_combo)
        return;

    int index = _itemButtons.indexOf(bt);
    pxv_info("[DsComboPopup#%d] on_item_clicked: index=%d", _id, index);

    if (index >= 0) {
        _combo->setCurrentIndex(index);
    }

    this->close();
}

DsComboBox::DsComboBox(QWidget *parent)
    : QComboBox(parent)
{
    _bPopup = false;
    _popup_seq = 0;
    QComboBox::setSizeAdjustPolicy(QComboBox::AdjustToContents);
}

DsComboBox::~DsComboBox()
{
}

void DsComboBox::measureSize()
{
    int num = this->count();
    int maxWidth = 0;
    QFontMetrics fm = this->fontMetrics();

    for (int i = 0; i < num; i++) {
        QString text = this->itemText(i);
        QRect rc = fm.boundingRect(text);

        if (rc.width() > maxWidth) {
            maxWidth = rc.width();
        }
    }
}

void DsComboBox::showPopup()
{
    pxv_info("[DsComboBox] showPopup ENTER: this=%p _bPopup=%d _popup=%p",
             this, _bPopup, (QWidget*)_popup);

    if (_bPopup || _popup) {
        pxv_info("[DsComboBox] showPopup: REJECTED (re-entrancy guard)");
        return;
    }

    _bPopup = true;

    if (count() == 0) {
        pxv_info("[DsComboBox] showPopup: count=0, aborting");
        _bPopup = false;
        return;
    }

    int seq = ++_popup_seq;
    pxv_info("[DsComboBox] showPopup: scheduling deferred popup seq=%d", seq);

    QTimer::singleShot(0, this, [this, seq]() {
        pxv_info("[DsComboBox] deferred timer fired: seq=%d _bPopup=%d _popup=%p",
                 seq, _bPopup, (QWidget*)_popup);

        if (!_bPopup || _popup) {
            pxv_info("[DsComboBox] deferred: SKIPPED (state changed)");
            return;
        }

        _popup = new DsComboPopup(this, this);
        _popup->_id = seq;

        connect(_popup, &QObject::destroyed, this, [this, seq]() {
            pxv_info("[DsComboBox] popup#%d destroyed signal: resetting _bPopup", seq);
            _bPopup = false;
        });

        pxv_info("[DsComboBox] calling popup#%d->show()", seq);
        _popup->show();
        pxv_info("[DsComboBox] popup#%d->show() returned", seq);
    });

    pxv_info("[DsComboBox] showPopup EXIT (deferred)");
}

void DsComboBox::hidePopup()
{
    pxv_info("[DsComboBox] hidePopup ENTER: _bPopup=%d _popup=%p",
             _bPopup, (QWidget*)_popup);

    if (_popup) {
        pxv_info("[DsComboBox] hidePopup: closing popup=%p", (QWidget*)_popup);
        _popup->close();
    }
    _bPopup = false;
}
