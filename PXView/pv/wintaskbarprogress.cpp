#ifdef _WIN32

#include "wintaskbarprogress.h"
#include <shobjidl.h>
#include <windows.h>

WinTaskbarProgress::WinTaskbarProgress()
    : _window(nullptr)
    , _taskbarList3(nullptr)
    , _value(0)
    , _minimum(0)
    , _maximum(100)
    , _visible(false)
{
}

WinTaskbarProgress::~WinTaskbarProgress()
{
    if (_taskbarList3) {
        _taskbarList3->Release();
        _taskbarList3 = nullptr;
    }
}

bool WinTaskbarProgress::initTaskbarList3()
{
    if (_taskbarList3) {
        return true;
    }

    HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskbarList3, (void**)&_taskbarList3);
    if (FAILED(hr)) {
        _taskbarList3 = nullptr;
        return false;
    }

    hr = _taskbarList3->HrInit();
    if (FAILED(hr)) {
        _taskbarList3->Release();
        _taskbarList3 = nullptr;
        return false;
    }

    return true;
}

void WinTaskbarProgress::setWindow(QWindow *window)
{
    _window = window;
    if (_visible && _window) {
        initTaskbarList3();
    }
}

void WinTaskbarProgress::setVisible(bool visible)
{
    _visible = visible;
    if (!_window || !initTaskbarList3()) {
        return;
    }

    HWND hwnd = (HWND)_window->winId();
    if (visible) {
        _taskbarList3->SetProgressState(hwnd, TBPF_NORMAL);
        updateProgress();
    } else {
        _taskbarList3->SetProgressState(hwnd, TBPF_NOPROGRESS);
    }
}

void WinTaskbarProgress::setValue(int value)
{
    _value = value;
    updateProgress();
}

void WinTaskbarProgress::setMaximum(int maximum)
{
    _maximum = maximum;
    updateProgress();
}

void WinTaskbarProgress::setMinimum(int minimum)
{
    _minimum = minimum;
    updateProgress();
}

void WinTaskbarProgress::reset()
{
    _value = _minimum;
    _visible = false;
    if (_window && initTaskbarList3()) {
        HWND hwnd = (HWND)_window->winId();
        _taskbarList3->SetProgressState(hwnd, TBPF_NOPROGRESS);
    }
}

void WinTaskbarProgress::updateProgress()
{
    if (!_visible || !_window || !initTaskbarList3()) {
        return;
    }

    HWND hwnd = (HWND)_window->winId();
    _taskbarList3->SetProgressValue(hwnd, _value - _minimum, _maximum - _minimum);
}

#endif // _WIN32
