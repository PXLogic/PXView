/*
 * test_popup_dlg_list.cpp — PopupDlgList 遍历安全性的回归测试。
 *
 * 背景：PopupDlgList::TryCloseAllByScreenChanged() 会在遍历过程中调用 w->close()，
 * 而 close() 会走对话框的 closeEvent → 析构 → ~PxDialog / ~DSMessageBox →
 * PopupDlgList::RemoveDlgFromList()，**重入**修改同一个 vector。
 *
 * 旧实现（已修）：
 *     int num = size();
 *     for (int i = 0; i < num; i++) {
 *         auto it = begin() + i;
 *         ...
 *         w->close();            // 这里可能已经 erase 掉了下标 i
 *         g_popup_dlg_list.erase(it);   // 于是删掉的是"当时的 i+1"——一个旁观者
 *         --num; --i;
 *     }
 * 两个后果：
 *   1) 旁观者被从表里摘掉 → 之后不再被关（**确定性逻辑 bug**，本文件主要锁这个）；
 *   2) i 已是最后一个下标时 erase(end()) → 迭代器越界 → 堆损坏 / 崩溃。
 *
 * 新实现：先摘表再 close()，且每轮重读 size()，因此两个后果都不存在。
 *
 * 依赖链：popupdlglist.cpp（含 pv/base/log.h，故内部 stub xlog）+ QtWidgets。
 * 无需 PXView.exe；offscreen 平台由 CTest 的 ENVIRONMENT 提供。
 */

#include <QtTest/QtTest>

// ── xlog stub：popupdlglist.cpp 经 log.h 引入 pxv_log / xlog_* 声明 ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include <QDialog>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>

#include "pv/ui/popupdlglist.h"

namespace {

/// 模拟 PxDialog / ~DSMessageBox：在 closeEvent 里把自己从 PopupDlgList 摘掉。
/// 这正是让旧实现的 erase(it) 变成"删旁观者"的那次重入。
class SelfRemovingDialog : public QDialog {
public:
    explicit SelfRemovingDialog(QWidget *parent = nullptr) : QDialog(parent) {}

protected:
    void closeEvent(QCloseEvent *event) override {
        if (_remove_on_close)
            PopupDlgList::RemoveDlgFromList(this);
        QDialog::closeEvent(event);
    }

public:
    bool _remove_on_close = true;
};

/// 普通对话框：closeEvent 不动列表（模拟不重入的对话框）。
class PlainDialog : public QDialog {
public:
    explicit PlainDialog(QWidget *parent = nullptr) : QDialog(parent) {}
};

/// 测试结束时的兜底清理：表是文件级静态的，跨用例会残留。
void purge(const QList<QWidget *> &ws) {
    for (QWidget *w : ws)
        PopupDlgList::RemoveDlgFromList(w);
}

} // namespace

class TestPopupDlgList : public QObject
{
    Q_OBJECT

private slots:
    void testReentrantRemovalDoesNotDropBystanders();
    void testReentrantRemovalAtLastIndexDoesNotCorrupt();
    void testStaleEntryIsPrunedAndListStaysUsable();
    void testSameScreenIsNotClosed();
    void testHiddenWidgetIsNotClosed();
    void testRemoveIsIdempotent();
};

// 旧实现下：A 的 close() 重入摘掉 A 之后，erase(begin()+0) 删掉的是 B，
// 于是 C 再也不会被关。新实现下三个都必须被关掉。
void TestPopupDlgList::testReentrantRemovalDoesNotDropBystanders()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);

    PopupDlgList::SetCurrentScreen(screen);

    auto *a = new SelfRemovingDialog();
    auto *b = new SelfRemovingDialog();
    auto *c = new SelfRemovingDialog();
    a->_remove_on_close = true;   // 重入
    b->_remove_on_close = false;
    c->_remove_on_close = false;

    a->show(); b->show(); c->show();
    QVERIFY(a->isVisible() && b->isVisible() && c->isVisible());

    PopupDlgList::AddDlgTolist(a);
    PopupDlgList::AddDlgTolist(b);
    PopupDlgList::AddDlgTolist(c);

    // 传一个与记录不同的 screen（nullptr 即可，代码只做指针比较）→ 全部应被关闭
    PopupDlgList::TryCloseAllByScreenChanged(nullptr);

    // 旧实现：重入摘掉 a 后，erase(begin()+0) 删的是 b → c 留在表里、仍然可见。
    QVERIFY2(!a->isVisible(), "A should have been closed");
    QVERIFY2(!b->isVisible(), "B (bystander) should still have been closed");
    QVERIFY2(!c->isVisible(), "C (bystander) should still have been closed");

    purge({a, b, c});
    delete a; delete b; delete c;
}

// 旧实现下若重入的是最后一个元素，erase 会拿到 end() → 迭代器越界。
// 这里只要求"不崩、且前一个旁观者仍被关掉"。
void TestPopupDlgList::testReentrantRemovalAtLastIndexDoesNotCorrupt()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    PopupDlgList::SetCurrentScreen(screen);

    auto *a = new PlainDialog();
    auto *b = new SelfRemovingDialog();   // 最后一个元素，且 close 时重入摘自己
    a->show(); b->show();

    PopupDlgList::AddDlgTolist(a);
    PopupDlgList::AddDlgTolist(b);

    PopupDlgList::TryCloseAllByScreenChanged(nullptr);

    QVERIFY2(!a->isVisible(), "A should have been closed");
    QVERIFY2(!b->isVisible(), "B should have been closed");

    purge({a, b});
    delete a; delete b;
}

// 对话框被控件树销毁（没走 RemoveDlgFromList）时，表里会留下一条"指向已释放对象"的
// 条目。旧实现是裸指针 → 遍历时 w->isVisible() 就是 use-after-free；
// 新实现用 QPointer，自动置空并丢弃该条目，且不能影响后续条目。
void TestPopupDlgList::testStaleEntryIsPrunedAndListStaysUsable()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    PopupDlgList::SetCurrentScreen(screen);

    auto *stale = new PlainDialog();
    stale->show();
    PopupDlgList::AddDlgTolist(stale);
    delete stale;                     // 故意不调 RemoveDlgFromList

    auto *live = new PlainDialog();
    live->show();
    PopupDlgList::AddDlgTolist(live);

    // 必须不崩；同时 live 仍应被正常关闭（说明表没被写坏）
    PopupDlgList::TryCloseAllByScreenChanged(nullptr);

    QVERIFY2(!live->isVisible(), "live dialog must still be closed after a stale entry");

    purge({live});
    delete live;
}

void TestPopupDlgList::testSameScreenIsNotClosed()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    PopupDlgList::SetCurrentScreen(screen);

    auto *w = new PlainDialog();
    w->show();
    PopupDlgList::AddDlgTolist(w);

    PopupDlgList::TryCloseAllByScreenChanged(screen);   // 同一个 screen → 不关

    QVERIFY2(w->isVisible(), "same-screen dialog must not be closed");

    purge({w});
    delete w;
}

void TestPopupDlgList::testHiddenWidgetIsNotClosed()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    PopupDlgList::SetCurrentScreen(screen);

    auto *w = new PlainDialog();      // 不 show() → isVisible() == false
    PopupDlgList::AddDlgTolist(w);

    PopupDlgList::TryCloseAllByScreenChanged(nullptr);
    QVERIFY(!w->isVisible());

    purge({w});
    delete w;
}

void TestPopupDlgList::testRemoveIsIdempotent()
{
    auto *w = new PlainDialog();
    w->show();

    PopupDlgList::AddDlgTolist(w);
    PopupDlgList::RemoveDlgFromList(w);
    PopupDlgList::RemoveDlgFromList(w);   // 再摘一次：必须是 no-op，不能崩
    PopupDlgList::RemoveDlgFromList(nullptr);

    delete w;
}

QTEST_MAIN(TestPopupDlgList)
#include "test_popup_dlg_list.moc"
