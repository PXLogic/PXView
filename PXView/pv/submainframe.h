
#ifndef PXVIEW_PV_SUBMAINFRAME_H
#define PXVIEW_PV_SUBMAINFRAME_H

#include "widgets/border.h"

#include <QMainWindow>
#include <QGridLayout>
#include <QRect>

#include "toolbars/titlebar.h"

namespace pv {

class WinNativeWidget;

class SubMainFrame :
    public QMainWindow,
    public ITitleParent,
    public IParentNativeEventCallback
{
    Q_OBJECT

public:
    static const int Margin = 5;

    enum BorderTypes {
        None = 0,
        TopLeft,
        Left,
        BottomLeft,
        Bottom,
        BottomRight,
        Right,
        TopRight,
        Top
    };

    SubMainFrame(QWidget *content, const QString &title, QWidget *parent = nullptr);
    ~SubMainFrame();

    void showAt(const QPoint &pos, const QSize &size);

    bool IsMaxsized();
    bool IsNormalsized();

    void SetFormRegion(int x, int y, int w, int h);
    QRect GetFormRegion();

signals:
    void windowClosed(QWidget *content, const QString &title);

public slots:
    void close();
    void showNormal();
    void showMaximized();
    void showMinimized();

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    void AttachNativeWindow();
    void hide_border();
    void show_border();

    void MoveWindow(int x, int y) override;
    QPoint GetParentPos() override;
    bool ParentIsMaxsized() override;
    void MoveBegin() override;
    void MoveEnd() override;
    void ParentShowMaximized() override;
    void ParentShowNormal() override;

    void OnParentNativeEvent(ParentNativeEvent msg) override;

private:
    toolbars::TitleBar *_titleBar;
    QWidget *_contentWidget;
    QString _title;

    QGridLayout *_layout;
    widgets::Border *_left;
    widgets::Border *_right;
    widgets::Border *_top;
    widgets::Border *_bottom;
    widgets::Border *_top_left;
    widgets::Border *_top_right;
    widgets::Border *_bottom_left;
    widgets::Border *_bottom_right;

    bool _bDraging;
    int _hit_border;
    QPoint _clickPos;
    QRect _dragStartRegion;

    bool _is_win32_parent_window;
    WinNativeWidget *_parentNativeWidget;
};

} // namespace pv

#endif
