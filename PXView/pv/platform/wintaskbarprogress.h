#ifndef WINTASKBARPROGRESS_H
#define WINTASKBARPROGRESS_H

#ifdef _WIN32

#include <QWindow>
#include <windows.h>
#include <shobjidl.h>

class WinTaskbarProgress
{
public:
    WinTaskbarProgress();
    ~WinTaskbarProgress();

    void setWindow(QWindow *window);
    void setVisible(bool visible);
    void setValue(int value);
    void setMaximum(int maximum);
    void setMinimum(int minimum);
    void reset();

private:
    bool initTaskbarList3();
    void updateProgress();

    QWindow *_window;
    ITaskbarList3 *_taskbarList3;
    int _value;
    int _minimum;
    int _maximum;
    bool _visible;
};

#endif // _WIN32

#endif // WINTASKBARPROGRESS_H
