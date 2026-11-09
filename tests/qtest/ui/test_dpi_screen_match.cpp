/*
 * test_dpi_screen_match.cpp — 跨屏 DPI 选屏逻辑回归
 *
 * 现场现象：4K 笔记本屏（150%）启动，把窗口拖到 1080p 扩展屏（100%）后，
 * 顶部菜单栏点不动，高 DPI 缩放没有关掉；拖回自带屏就正常。
 *
 * 根因在 WinNativeWidget::screenFromCurrentMonitorHandle()：拿
 * MONITORINFO.rcMonitor.topLeft()（物理像素）和
 * QScreen::geometry().topLeft()（Qt 逻辑像素）做相等比较。4K@150% 主屏 +
 * 右侧 1080p@100% 副屏时，副屏 Win32 left=3840、Qt left=2560，永远匹配失败；
 * 返回 nullptr 后 GetDevicePixelRatio()/UpdateChildDpi() 统统回退
 * QGuiApplication::primaryScreen()（4K 屏），DPR 就被钉在 1.5。
 *
 * 本测试锁住替代判据：HMONITOR 精确匹配 + Win32 DPR 兜底，且**不允许**
 * 在无匹配时默认选主屏（索引 0）。旧行为确定性失败。
 */

#include <QtTest>
#include <vector>

#include "pv/platform/dpi_screen_match.h"

using pv::platform::select_screen_by_dpr;
using pv::platform::select_screen_by_monitor;

class TestDpiScreenMatch : public QObject {
    Q_OBJECT

private slots:
    void exactMonitorMatch_data();
    void exactMonitorMatch();
    void mixedDpiSecondaryIsNotPrimary();
    void unknownMonitorIsRejected();
    void nullTargetIsRejected();
    void emptyHandleListIsRejected();

    void dprFallbackPicksCorrectScreen_data();
    void dprFallbackPicksCorrectScreen();
    void dprFallbackNeverDefaultsToPrimary();
    void dprFallbackRejectsOutOfTolerance();
    void dprFallbackRejectsInvalidInputs();
};

void TestDpiScreenMatch::exactMonitorMatch_data()
{
    QTest::addColumn<int>("targetIndex");
    QTest::newRow("primary") << 0;
    QTest::newRow("secondary") << 1;
    QTest::newRow("third") << 2;
}

void TestDpiScreenMatch::exactMonitorMatch()
{
    int a = 0;
    int b = 0;
    int c = 0;
    const std::vector<const void *> handles{&a, &b, &c};

    QFETCH(int, targetIndex);
    const void *target = handles.at(static_cast<std::size_t>(targetIndex));
    QCOMPARE(select_screen_by_monitor(handles, target), targetIndex);
}

void TestDpiScreenMatch::mixedDpiSecondaryIsNotPrimary()
{
    int primary = 0;
    int secondary = 0;
    const std::vector<const void *> handles{&primary, &secondary};

    // The regression: on a mixed-DPI desktop the old coordinate-equality test
    // could not match the secondary monitor and silently fell back to the
    // primary screen (index 0), keeping the 4K scale factor alive.
    QCOMPARE(select_screen_by_monitor(handles, &secondary), 1);
    QCOMPARE(select_screen_by_monitor(handles, &primary), 0);
}

void TestDpiScreenMatch::unknownMonitorIsRejected()
{
    int primary = 0;
    int secondary = 0;
    int stranger = 0;
    const std::vector<const void *> handles{&primary, &secondary};

    QCOMPARE(select_screen_by_monitor(handles, &stranger), -1);
}

void TestDpiScreenMatch::nullTargetIsRejected()
{
    int primary = 0;
    const std::vector<const void *> handles{&primary};

    QCOMPARE(select_screen_by_monitor(handles, nullptr), -1);
}

void TestDpiScreenMatch::emptyHandleListIsRejected()
{
    int primary = 0;
    const std::vector<const void *> handles;

    QCOMPARE(select_screen_by_monitor(handles, &primary), -1);
}

void TestDpiScreenMatch::dprFallbackPicksCorrectScreen_data()
{
    QTest::addColumn<double>("targetDpr");
    QTest::addColumn<int>("expectedIndex");

    QTest::newRow("4K at 150 percent") << 1.5 << 0;
    QTest::newRow("1080p at 100 percent") << 1.0 << 1;
    QTest::newRow("QHD at 125 percent") << 1.25 << 2;
}

void TestDpiScreenMatch::dprFallbackPicksCorrectScreen()
{
    const std::vector<double> dprs{1.5, 1.0, 1.25};

    QFETCH(double, targetDpr);
    QFETCH(int, expectedIndex);
    QCOMPARE(select_screen_by_dpr(dprs, targetDpr), expectedIndex);
}

void TestDpiScreenMatch::dprFallbackNeverDefaultsToPrimary()
{
    // When the native HMONITOR interface is unavailable, the DPR of the
    // monitor the window was dragged to (1.0) must select the 1080p screen,
    // not the 4K primary at index 0.
    const std::vector<double> dprs{1.5, 1.0};
    QCOMPARE(select_screen_by_dpr(dprs, 1.0), 1);

    // A screen with a null/invalid DPR must not be chosen just because it is
    // the first entry.
    const std::vector<double> with_invalid{0.0, 1.0};
    QCOMPARE(select_screen_by_dpr(with_invalid, 1.0), 1);
}

void TestDpiScreenMatch::dprFallbackRejectsOutOfTolerance()
{
    const std::vector<double> dprs{1.5, 1.0};

    QCOMPARE(select_screen_by_dpr(dprs, 1.75), -1);
    QCOMPARE(select_screen_by_dpr(dprs, 1.1, 0.01), -1);
    QCOMPARE(select_screen_by_dpr(dprs, 1.1, 0.2), 1);
}

void TestDpiScreenMatch::dprFallbackRejectsInvalidInputs()
{
    const std::vector<double> dprs{1.5, 1.0};
    const std::vector<double> empty;

    QCOMPARE(select_screen_by_dpr(dprs, 0.0), -1);
    QCOMPARE(select_screen_by_dpr(dprs, -1.0), -1);
    QCOMPARE(select_screen_by_dpr(dprs, 1.0, -0.5), -1);
    QCOMPARE(select_screen_by_dpr(empty, 1.0), -1);
}

QTEST_MAIN(TestDpiScreenMatch)
#include "test_dpi_screen_match.moc"
