
#include "submainframe.h"

#include "toolbars/titlebar.h"

#include <QVBoxLayout>
#include <QEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QWindow>
#include <QTimer>
#include <QApplication>

#include "dsvdef.h"
#include "config/appconfig.h"
#include "log.h"

#ifdef _WIN32
#include "winnativewidget.h"
#include <Windows.h>
#endif

namespace pv {

SubMainFrame::SubMainFrame(QWidget *content, const QString &title, QWidget *parent) :
    QMainWindow(parent),
    _contentWidget(content),
    _title(title)
{
    _layout = NULL;
    _bDraging = false;
    _hit_border = 0;
    _titleBar = NULL;
    _is_win32_parent_window = false;
    _parentNativeWidget = NULL;

    _left = NULL;
    _right = NULL;
    _top = NULL;
    _bottom = NULL;
    _top_left = NULL;
    _top_right = NULL;
    _bottom_left = NULL;
    _bottom_right = NULL;

    bool isWin32 = false;

#ifdef _WIN32
    setWindowFlags(Qt::FramelessWindowHint);
    _is_win32_parent_window = true;
    isWin32 = true;
#else
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
    setAttribute(Qt::WA_TranslucentBackground);
    _is_win32_parent_window = false;
#endif

#ifdef _WIN32
    if (!_is_win32_parent_window){
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
        setAttribute(Qt::WA_TranslucentBackground);
    }
#endif

    setMinimumSize(400, 300);

    QIcon icon;
    icon.addFile(QString::fromUtf8(":/icons/logo.svg"), QSize(), QIcon::Normal, QIcon::Off);
    setWindowIcon(icon);

    _titleBar = new toolbars::TitleBar(true, this, this, true, false);
    _titleBar->setTitle(title);

    setMenuBar(_titleBar);

    QWidget *centralWidget = new QWidget(this);
    _layout = new QGridLayout(centralWidget);
    _layout->setSpacing(0);
    _layout->setContentsMargins(0, 0, 0, 0);

    if (!isWin32 || !_is_win32_parent_window)
    {
        _top_left = new widgets::Border(TopLeft, this);
        _top_left->setFixedSize(Margin, Margin);
        _top_left->installEventFilter(this);
        _top = new widgets::Border(Top, this);
        _top->setFixedHeight(Margin);
        _top->installEventFilter(this);
        _top_right = new widgets::Border(TopRight, this);
        _top_right->setFixedSize(Margin, Margin);
        _top_right->installEventFilter(this);

        _left = new widgets::Border(Left, this);
        _left->setFixedWidth(Margin);
        _left->installEventFilter(this);
        _right = new widgets::Border(Right, this);
        _right->setFixedWidth(Margin);
        _right->installEventFilter(this);

        _bottom_left = new widgets::Border(BottomLeft, this);
        _bottom_left->setFixedSize(Margin, Margin);
        _bottom_left->installEventFilter(this);
        _bottom = new widgets::Border(Bottom, this);
        _bottom->setFixedHeight(Margin);
        _bottom->installEventFilter(this);
        _bottom_right = new widgets::Border(BottomRight, this);
        _bottom_right->setFixedSize(Margin, Margin);
        _bottom_right->installEventFilter(this);

        _layout->addWidget(_top_left, 0, 0);
        _layout->addWidget(_top, 0, 1);
        _layout->addWidget(_top_right, 0, 2);
        _layout->addWidget(_left, 1, 0);
        if (content) {
            content->setParent(this);
            _layout->addWidget(content, 1, 1);
            content->show();
        }
        _layout->addWidget(_right, 1, 2);
        _layout->addWidget(_bottom_left, 2, 0);
        _layout->addWidget(_bottom, 2, 1);
        _layout->addWidget(_bottom_right, 2, 2);
    }
    else{
        if (content) {
            content->setParent(this);
            _layout->addWidget(content, 0, 0);
            content->show();
        }
    }

    setCentralWidget(centralWidget);

    installEventFilter(this);
}

SubMainFrame::~SubMainFrame()
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL) {
        SetWindowLongPtr(_parentNativeWidget->Handle(), GWLP_USERDATA, 0);
        delete _parentNativeWidget;
        _parentNativeWidget = NULL;
    }
#endif
}

void SubMainFrame::showAt(const QPoint &pos, const QSize &size)
{
    move(pos);
    resize(size);

    if (!_is_win32_parent_window){
        QMainWindow::show();
        return;
    }

#ifdef _WIN32
    if (_is_win32_parent_window){
        AttachNativeWindow();
    }
#endif
}

void SubMainFrame::AttachNativeWindow()
{
#ifdef _WIN32
    assert(_parentNativeWidget == NULL);

    int k = window()->devicePixelRatio();
    int x = pos().x() * k;
    int y = pos().y() * k;
    int w = width() * k;
    int h = height() * k;

    QColor bkColor = AppConfig::Instance().GetStyleColor();
    WinNativeWidget *nativeWindow = new WinNativeWidget(x, y, w, h, bkColor);

    if (nativeWindow->Handle() == NULL){
        pxv_info("ERROR: native window is invalid for sub window.");
        delete nativeWindow;
        return;
    }

    nativeWindow->SetChildWidget(this);
    nativeWindow->SetNativeEventCallback(this);
    nativeWindow->SetTitleBarWidget(_titleBar);
    nativeWindow->SetBodyViewWidget(_contentWidget);
    _titleBar->EnableAbleDrag(false);

    (void)winId();
    SetWindowLong((HWND)winId(), GWL_STYLE, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    SetWindowLong((HWND)winId(), GWL_EXSTYLE, 0);
    SetParent((HWND)winId(), nativeWindow->Handle());

    setVisible(true);

    nativeWindow->Show(true);
    nativeWindow->ResizeChild();

    nativeWindow->SetBorderColor(QColor(0x80, 0x80, 0x80));
    _parentNativeWidget = nativeWindow;

    QTimer::singleShot(100, this, [this](){
        if (_parentNativeWidget)
            _parentNativeWidget->ResizeChild();
    });
#endif
}

void SubMainFrame::MoveWindow(int x, int y)
{
#ifdef _WIN32
    assert(_parentNativeWidget == NULL);
#endif
    move(x, y);
}

QPoint SubMainFrame::GetParentPos()
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        RECT rc;
        int k = window()->devicePixelRatio();
        GetWindowRect(_parentNativeWidget->Handle(), &rc);
        return QPoint(rc.left / k, rc.top / k);
    }
#endif
    return pos();
}

bool SubMainFrame::ParentIsMaxsized()
{
    return IsMaxsized();
}

void SubMainFrame::MoveBegin()
{
}

void SubMainFrame::MoveEnd()
{
}

void SubMainFrame::ParentShowMaximized() { showMaximized(); }
void SubMainFrame::ParentShowNormal() { showNormal(); }

void SubMainFrame::OnParentNativeEvent(ParentNativeEvent msg)
{
    (void)msg;
}

void SubMainFrame::close()
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        _parentNativeWidget->SetClosing(true);
        ::ShowWindow(_parentNativeWidget->Handle(), SW_HIDE);
    }
#endif
    QMainWindow::close();
}

void SubMainFrame::closeEvent(QCloseEvent *event)
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        SetParent((HWND)winId(), NULL);
        _parentNativeWidget->SetChildWidget(NULL);
    }
#endif

    QWidget *content = _contentWidget;
    if (content) {
        content->setParent(NULL);
        content->hide();
    }

    emit windowClosed(content, _title);
    event->accept();
}

void SubMainFrame::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    if (_layout == NULL){
        return;
    }

    if (IsMaxsized()) {
        hide_border();
    }
    else if (IsNormalsized()) {
        show_border();
    }

    _titleBar->setRestoreButton(IsMaxsized());
    _layout->update();
}

void SubMainFrame::showNormal()
{
    show_border();

#ifdef _WIN32
    if (_parentNativeWidget){
        _parentNativeWidget->ShowNormal();
        return;
    }
#endif

    QMainWindow::showNormal();
}

void SubMainFrame::showMaximized()
{
    hide_border();

#ifdef _WIN32
    if (_parentNativeWidget){
        _parentNativeWidget->ShowMax();
        return;
    }
#endif

    QMainWindow::showMaximized();
}

void SubMainFrame::showMinimized()
{
#ifdef _WIN32
    if (_parentNativeWidget){
        _parentNativeWidget->ShowMin();
        return;
    }
#endif

    QMainWindow::showMinimized();
}

void SubMainFrame::hide_border()
{
    if (_top_left == NULL)
        return;

    _top_left->setVisible(false);
    _top_right->setVisible(false);
    _top->setVisible(false);
    _left->setVisible(false);
    _right->setVisible(false);
    _bottom_left->setVisible(false);
    _bottom->setVisible(false);
    _bottom_right->setVisible(false);
}

void SubMainFrame::show_border()
{
    if (_top_left == NULL)
        return;

    _top_left->setVisible(true);
    _top_right->setVisible(true);
    _top->setVisible(true);
    _left->setVisible(true);
    _right->setVisible(true);
    _bottom_left->setVisible(true);
    _bottom->setVisible(true);
    _bottom_right->setVisible(true);
}

void SubMainFrame::SetFormRegion(int x, int y, int w, int h)
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        int k = _parentNativeWidget->GetDevicePixelRatio();
        x *= k;
        y *= k;
        w *= k;
        h *= k;
        _parentNativeWidget->setGeometry(x, y, w, h);
        return;
    }
#endif
    setGeometry(x, y, w, h);
}

QRect SubMainFrame::GetFormRegion()
{
    QRect rc;

#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        int k = _parentNativeWidget->GetDevicePixelRatio();
        RECT r;
        GetWindowRect(_parentNativeWidget->Handle(), &r);
        int x = r.left;
        int y = r.top;
        int w = r.right - r.left;
        int h = r.bottom - r.top;
        rc = QRect(x / k, y / k, w / k, h / k);
    }
    else{
        rc = geometry();
    }
#else
    rc = geometry();
#endif

    return rc;
}

bool SubMainFrame::IsMaxsized()
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        return _parentNativeWidget->IsMaxsized();
    }
#endif
    return QMainWindow::isMaximized();
}

bool SubMainFrame::IsNormalsized()
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        return _parentNativeWidget->IsNormalsized();
    }
#endif
    if (!QMainWindow::isMaximized() && !QMainWindow::isMinimized()){
        return true;
    }
    return false;
}

bool SubMainFrame::eventFilter(QObject *object, QEvent *event)
{
    const QEvent::Type type = event->type();
    const QMouseEvent *const mouse_event = (QMouseEvent*)event;

#ifdef _WIN32
    if (_parentNativeWidget != NULL){
        return QMainWindow::eventFilter(object, event);
    }
#endif

    if (type != QEvent::MouseMove
        && type != QEvent::MouseButtonPress
        && type != QEvent::MouseButtonRelease
        && type != QEvent::Leave){
        return QMainWindow::eventFilter(object, event);
    }

    if (IsMaxsized()){
        return QMainWindow::eventFilter(object, event);
    }

    if (!_bDraging && type == QEvent::MouseMove && (!(mouse_event->buttons() | Qt::NoButton))){
        if (object == _top_left) {
            _hit_border = TopLeft;
            setCursor(Qt::SizeFDiagCursor);
        } else if (object == _bottom_right) {
            _hit_border = BottomRight;
            setCursor(Qt::SizeFDiagCursor);
        } else if (object == _top_right) {
            _hit_border = TopRight;
            setCursor(Qt::SizeBDiagCursor);
        } else if (object == _bottom_left) {
            _hit_border = BottomLeft;
            setCursor(Qt::SizeBDiagCursor);
        } else if (object == _left) {
            _hit_border = Left;
            setCursor(Qt::SizeHorCursor);
        } else if (object == _right) {
            _hit_border = Right;
            setCursor(Qt::SizeHorCursor);
        } else if (object == _bottom) {
            _hit_border = Bottom;
            setCursor(Qt::SizeVerCursor);
        } else if (object == _top) {
            _hit_border = Top;
            setCursor(Qt::SizeVerCursor);
        } else {
            _hit_border = 0;
            setCursor(Qt::ArrowCursor);
        }
        return QMainWindow::eventFilter(object, event);
    }

    if (type == QEvent::MouseMove) {
        QPoint pt = mouse_event->globalPosition().toPoint();
        int datX = pt.x() - _clickPos.x();
        int datY = pt.y() - _clickPos.y();

        int l = _dragStartRegion.left();
        int t = _dragStartRegion.top();
        int r = _dragStartRegion.right();
        int b = _dragStartRegion.bottom();

        if (mouse_event->buttons().testFlag(Qt::LeftButton)) {
            int minW = minimumWidth();
            int minH = minimumHeight();

            switch (_hit_border) {
                case TopLeft:
                    l += datX; t += datY;
                    if (r - l < minW) l = r - minW;
                    if (b - t < minH) t = b - minH;
                    break;
                case BottomLeft:
                    l += datX; b += datY;
                    if (r - l < minW) l = r - minW;
                    if (b - t < minH) b = t + minH;
                    break;
                case TopRight:
                    r += datX; t += datY;
                    if (r - l < minW) r = l + minW;
                    if (b - t < minH) t = b - minH;
                    break;
                case BottomRight:
                    r += datX; b += datY;
                    if (r - l < minW) r = l + minW;
                    if (b - t < minH) b = t + minH;
                    break;
                case Left:
                    l += datX;
                    if (r - l < minW) l = r - minW;
                    break;
                case Right:
                    r += datX;
                    if (r - l < minW) r = l + minW;
                    break;
                case Top:
                    t += datY;
                    if (b - t < minH) t = b - minH;
                    break;
                case Bottom:
                    b += datY;
                    if (b - t < minH) b = t + minH;
                    break;
                default:
                    r = l;
                    break;
            }

            if (r != l){
                SetFormRegion(l, t, r - l, b - t);
            }
            return true;
        }
    }
    else if (type == QEvent::MouseButtonPress) {
        if (mouse_event->button() == Qt::LeftButton)
        if (_hit_border != 0)
            _bDraging = true;
        _clickPos = mouse_event->globalPosition().toPoint();
        _dragStartRegion = GetFormRegion();
    }
    else if (type == QEvent::MouseButtonRelease) {
        if (mouse_event->button() == Qt::LeftButton) {
            _bDraging = false;
        }
    }
    else if (!_bDraging && type == QEvent::Leave) {
        _hit_border = 0;
        setCursor(Qt::ArrowCursor);
    }

    return QMainWindow::eventFilter(object, event);
}

bool SubMainFrame::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
#ifdef _WIN32
    if (_parentNativeWidget != NULL)
    {
        MSG *msg = static_cast<MSG*>(message);
        HWND hwnd = _parentNativeWidget->Handle();

        switch (msg->message)
        {
            case WM_NCMOUSEMOVE:
            case WM_NCLBUTTONDOWN:
            case WM_NCLBUTTONUP:
            case WM_NCLBUTTONDBLCLK:
            case WM_NCHITTEST:
            {
                *result = static_cast<qintptr>(SendMessageW(hwnd,
                        msg->message, msg->wParam, msg->lParam));
                return true;
            }
        }
    }
#endif

    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace pv
