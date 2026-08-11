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
#include <QApplication>
#include "pv/config/appconfig.h"
#include "pv/ui/dockfonts.h"
#include "pv/widgets/smoothscrollarea.h"
#include "pv/base/log.h"

#ifdef WIN32
#include <windows.h>
#endif

DsComboPopup::DsComboPopup(QComboBox *combo, QWidget *parent)
    : QDialog(parent)
{
    // 使用 Qt::Tool 而非 Qt::Popup。
    //
    // Qt::Popup 会在 show() 时 grab 鼠标，导致 QComboBox::mousePressEvent
    // 中的鼠标按下→释放周期跨越弹窗显示，释放事件被 Qt::Popup 内置机制
    // 当作"弹窗外点击"从而关闭弹窗，然后合成 MouseButtonPress 转发给
    // combobox 触发新的 showPopup()，造成无限循环闪烁。
    //
    // Qt::Tool 不 grab 鼠标，鼠标释放事件直接到达 combobox，弹窗不受影响。
    // 点击外部关闭通过 qApp 全局事件过滤器手动实现。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
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

    // 全局事件过滤器：检测点击弹窗外时关闭弹窗（替代 Qt::Popup 的内置行为）
    qApp->installEventFilter(this);

    pxv_info("[DsComboPopup] constructor: this=%p combo=%p items=%d",
             this, combo, combo->count());
}

void DsComboPopup::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange) {
        bool active = this->isActiveWindow();
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

    // 确保弹窗显示在最前面
    this->raise();
    this->activateWindow();

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
    });
}

void DsComboPopup::hideEvent(QHideEvent *event)
{
    QDialog::hideEvent(event);
}

void DsComboPopup::closeEvent(QCloseEvent *event)
{
    QDialog::closeEvent(event);
}

bool DsComboPopup::eventFilter(QObject *watched, QEvent *event)
{
    // 全局事件过滤器：检测鼠标点击落在弹窗外时关闭弹窗。
    // 这替代了 Qt::Popup 的内置"点击外部关闭"行为（Qt::Tool 没有此行为）。
    if (event->type() == QEvent::MouseButtonPress && this->isVisible()) {
        auto *me = static_cast<QMouseEvent*>(event);
        QPoint globalPos = me->globalPosition().toPoint();
        QPoint localPos = this->mapFromGlobal(globalPos);

        if (!this->rect().contains(localPos)) {
            // 点击在弹窗外部 → 关闭弹窗
            pxv_info("[DsComboPopup#%d] eventFilter: outside click at (%d,%d), closing",
                     _id, globalPos.x(), globalPos.y());
            this->close();
        }
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
    if (_bPopup || _popup) {
        return;
    }

    _bPopup = true;

    if (count() == 0) {
        _bPopup = false;
        return;
    }

    // 延迟到下一个事件循环迭代再创建和显示弹窗。
    // 让 mousePressEvent → mouseReleaseEvent 的完整周期先结束，
    // 避免弹窗在鼠标按下期间显示。
    int seq = ++_popup_seq;

    QTimer::singleShot(0, this, [this, seq]() {
        if (!_bPopup || _popup) {
            return;
        }

        _popup = new DsComboPopup(this, this);
        _popup->_id = seq;

        connect(_popup, &QObject::destroyed, this, [this, seq]() {
            _bPopup = false;
        });

        _popup->show();
    });
}

void DsComboBox::hidePopup()
{
    if (_popup) {
        _popup->close();
    }
    _bPopup = false;
}
