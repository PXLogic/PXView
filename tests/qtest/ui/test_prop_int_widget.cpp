/*
 * test_prop_int_widget.cpp — pv::prop::Int 编辑器（SpinBox / 64 位 SpinBox）契约。
 *
 * 背景（2026-09-24 用户报告 → 2026-09-25 定案）：
 *   1) 导入 raw binary 时 "Sample rate (Hz)"（uint64 选项）永远显示 0：
 *      Int::get_widget() 把 range_max 设成 UINT64_MAX，而变量是 int64_t ——
 *      窄化后是 -1，setRange(0, -1) 让 Qt 把 0 当成唯一合法值。
 *   2) 修好显示后又暴露出第二个问题：QSpinBox 只装得下 int，VCD 的 "skip"
 *      （uint64 默认 UINT64_MAX，语义"从文件第一个时间戳开始"）显示成
 *      2147483647，而且对话框一确定就把这个夹紧值提交了回去，语义被悄悄改写。
 *
 * 定案方案（与参考 PulseView 的 pv/widgets/timestampspinbox.cpp 同路，
 * 正是 pv/prop/int.cpp 里那句 @todo 要的 custom widget）：
 *   * 窄类型（byte/int16/uint16/int32）继续用 QSpinBox —— 它本来就装得下；
 *   * 宽类型（uint32/int64/uint64）改用 pv::ui::IntSpinBox：QAbstractSpinBox
 *     派生，内部用 int64_t/uint64_t，覆盖完整 64 位范围，带范围校验与后缀；
 *   * 有"明确且在 int 范围内"的 range 时仍用 QSpinBox（值不可能越界）。
 *
 * 本文件锁住：
 *   1) 窄类型仍是 QSpinBox，取值与范围不受影响；
 *   2) 宽类型是 IntSpinBox，能精确显示 UINT64_MAX 这类哨兵值；
 *   3) 超过 INT_MAX 的值现在**可以手动输入**（4 GHz 采样率）；
 *   4) 校验：无符号拒绝负号/非数字，有符号接受负数，越界被拒；
 *   5) 提交：把编辑器里的值原样交给 setter。
 *
 * 链接方式：只编 pv/prop/{property,int}.cpp 与 pv/ui/{dsspinbox,intspinbox}.cpp，
 * Qt Widgets 环境（ctest 已设 QT_QPA_PLATFORM=offscreen），不需要 libsigrok。
 */

#include <QtTest/QtTest>

#include <QLineEdit>
#include <QSpinBox>

#include <climits>
#include <cstdint>
#include <optional>
#include <utility>

#include "pv/prop/int.h"
#include "pv/ui/intspinbox.h"

namespace {

using IntSpinBox = pv::ui::IntSpinBox;

/* 造一个 Int 属性：getter 每次交出一个新引用（Property::Getter 契约），
 * setter 记录最后一次写入的值供断言。 */
class IntHarness
{
public:
    IntHarness(GVariant *initial, std::optional<std::pair<int64_t, int64_t>> range = std::nullopt)
        : _initial(g_variant_ref_sink(initial))
    {
        pv::prop::Property::Getter getter = [this]() -> GVariant * {
            return g_variant_ref(_initial);
        };
        pv::prop::Property::Setter setter = [this](GVariant *v) {
            if (_committed)
                g_variant_unref(_committed);
            _committed = v ? g_variant_ref_sink(v) : nullptr;
        };

        _prop = new pv::prop::Int(QStringLiteral("opt"), QStringLiteral("label"),
                                  QString(), range, getter, setter);
    }

    ~IntHarness()
    {
        delete _prop;
        if (_committed)
            g_variant_unref(_committed);
        if (_initial)
            g_variant_unref(_initial);
    }

    IntHarness(const IntHarness &) = delete;
    IntHarness &operator=(const IntHarness &) = delete;

    /* 编辑器必须能显式指定父对象：这里用 nullptr 即可（无窗口环境）。 */
    QWidget *widget()
    {
        if (!_widget)
            _widget = _prop->get_widget_deferred(nullptr);
        return _widget;
    }

    /// 窄类型编辑器（QSpinBox）；宽类型返回 nullptr。
    QSpinBox *spin() { return qobject_cast<QSpinBox *>(widget()); }

    /// 宽类型编辑器（64 位）。
    IntSpinBox *wide() { return qobject_cast<IntSpinBox *>(widget()); }

    /// 驱动 OK 路径：commit() 应把编辑器里的值交给 setter。
    void commit() { _prop->commit(); }

    /// 最近一次 setter 收到的值（新引用，属于本对象）。
    GVariant *committed() const { return _committed; }

private:
    GVariant *_initial = nullptr;
    GVariant *_committed = nullptr;
    pv::prop::Int *_prop = nullptr;
    QWidget *_widget = nullptr;
};

/* 模拟用户输入：把文本放进编辑框，再让编辑器认为输入结束（Enter 与失焦
 * 走同一条 submitting 路径 —— QAbstractSpinBox::lineEdit() 是 protected，
 * 这里用公开的 findChild 取内部编辑框）。 */
void typeInto(IntSpinBox *editor, const QString &text)
{
    QLineEdit *const line_edit = editor->findChild<QLineEdit *>();
    QVERIFY(line_edit != nullptr);
    line_edit->setText(text);
    QMetaObject::invokeMethod(line_edit, "editingFinished");
}

} // namespace

class TestPropIntWidget : public QObject
{
    Q_OBJECT

private slots:
    /* 窄类型（byte/int16/uint16/int32）继续用 QSpinBox：取值与范围不变。 */
    void narrowTypesKeepTheSpinBox();

    /* 宽类型（uint64）改用 64 位编辑器，显示精确值。 */
    void wideTypesUseTheIntSpinBox();

    /* UINT64_MAX（VCD 的 skip）必须被精确显示并精确提交。 */
    void uint64SentinelIsShownAndCommittedExactly();

    /* 关键修复：超过 INT_MAX 的值现在可以手动输入（4 GHz 采样率）。 */
    void valuesBeyondIntMaxCanBeTyped();

    /* 校验：无符号拒绝负号/非数字，越界被拒并修复；有符号接受负数。 */
    void wideEditorValidatesAgainstItsRange();

    /* 有明确且在 int 范围内的 range 时仍用 QSpinBox（值不可能越界）。 */
    void explicitSmallRangeKeepsTheSpinBox();

    /* 编辑器里改值后 commit() 会带着新值回调 setter。 */
    void commitDeliversTheEditedValue();
};

void TestPropIntWidget::narrowTypesKeepTheSpinBox()
{
    IntHarness int32_harness(g_variant_new_int32(32));
    QSpinBox *const spin = int32_harness.spin();
    QVERIFY(spin != nullptr);
    QVERIFY(int32_harness.wide() == nullptr);
    QCOMPARE(spin->value(), 32);
    QCOMPARE(spin->maximum(), INT_MAX);

    // 显式范围优先于类型推导范围。
    IntHarness ranged(g_variant_new_int32(32),
                      std::make_pair<int64_t, int64_t>(1, 64));
    QSpinBox *const ranged_spin = ranged.spin();
    QVERIFY(ranged_spin != nullptr);
    QCOMPARE(ranged_spin->value(), 32);
    QCOMPARE(ranged_spin->minimum(), 1);
    QCOMPARE(ranged_spin->maximum(), 64);

    IntHarness uint16_harness(g_variant_new_uint16(60000));
    QSpinBox *const uint16_spin = uint16_harness.spin();
    QVERIFY(uint16_spin != nullptr);
    QCOMPARE(uint16_spin->value(), 60000);
    QCOMPARE(uint16_spin->maximum(), 65535);
}

void TestPropIntWidget::wideTypesUseTheIntSpinBox()
{
    IntHarness harness(g_variant_new_uint64(1000000));
    IntSpinBox *const editor = harness.wide();
    QVERIFY(editor != nullptr);
    QVERIFY(harness.spin() == nullptr);  // QSpinBox 装不下 uint64

    QCOMPARE(editor->unsignedValue(), static_cast<quint64>(1000000));
    QCOMPARE(editor->text(), QStringLiteral("1000000"));
    QCOMPARE(editor->unsignedMinimum(), static_cast<quint64>(0));
    QCOMPARE(editor->unsignedMaximum(), static_cast<quint64>(UINT64_MAX));
    QVERIFY(!editor->isSigned());

    // int64 选项走有符号分支（负数可输入）。
    IntHarness signed_harness(g_variant_new_int64(-12345));
    IntSpinBox *const signed_editor = signed_harness.wide();
    QVERIFY(signed_editor != nullptr);
    QVERIFY(signed_editor->isSigned());
    QCOMPARE(signed_editor->signedValue(), static_cast<qint64>(-12345));
    QCOMPARE(signed_editor->text(), QStringLiteral("-12345"));
}

void TestPropIntWidget::uint64SentinelIsShownAndCommittedExactly()
{
    // VCD 的 "skip" 默认 UINT64_MAX = "从文件第一个时间戳开始"：旧实现显示
    // 2147483647 并把夹紧值提交回去，语义被改写。
    IntHarness harness(g_variant_new_uint64(static_cast<guint64>(UINT64_MAX)));
    IntSpinBox *const editor = harness.wide();
    QVERIFY(editor != nullptr);

    QCOMPARE(editor->text(),
             QString::number(static_cast<qulonglong>(UINT64_MAX)));
    QCOMPARE(editor->unsignedValue(), static_cast<quint64>(UINT64_MAX));

    harness.commit();
    QVERIFY(harness.committed() != nullptr);
    QVERIFY(g_variant_is_of_type(harness.committed(), G_VARIANT_TYPE_UINT64));
    QCOMPARE(g_variant_get_uint64(harness.committed()),
             static_cast<guint64>(UINT64_MAX));
}

void TestPropIntWidget::valuesBeyondIntMaxCanBeTyped()
{
    // 4 GHz 采样率：QSpinBox 时代最大只能填到 INT_MAX(≈2.1e9)，填不进去。
    IntHarness harness(g_variant_new_uint64(1000000));
    IntSpinBox *const editor = harness.wide();
    QVERIFY(editor != nullptr);

    typeInto(editor, QStringLiteral("4000000000"));
    QCOMPARE(editor->unsignedValue(), static_cast<quint64>(4000000000));

    harness.commit();
    QVERIFY(harness.committed() != nullptr);
    QCOMPARE(g_variant_get_uint64(harness.committed()),
             static_cast<guint64>(4000000000));

    // 步进也要能跨过 INT_MAX（滚轮/上下箭头走同一条路）。
    editor->stepBy(1);
    QCOMPARE(editor->unsignedValue(), static_cast<quint64>(4000000001));
    editor->setUnsignedValue(static_cast<quint64>(UINT64_MAX));
    editor->stepBy(1);  // 饱和，不得回绕
    QCOMPARE(editor->unsignedValue(), static_cast<quint64>(UINT64_MAX));
    editor->stepBy(-1);
    QCOMPARE(editor->unsignedValue(), static_cast<quint64>(UINT64_MAX) - 1);
}

void TestPropIntWidget::wideEditorValidatesAgainstItsRange()
{
    IntHarness unsigned_harness(g_variant_new_uint64(1000));
    IntSpinBox *const unsigned_editor = unsigned_harness.wide();
    QVERIFY(unsigned_editor != nullptr);

    int pos = 0;
    QString minus(QStringLiteral("-1"));
    QCOMPARE(unsigned_editor->validate(minus, pos), QValidator::Invalid);
    QString letters(QStringLiteral("12a"));
    QCOMPARE(unsigned_editor->validate(letters, pos), QValidator::Invalid);
    QString pending(QStringLiteral(""));
    QCOMPARE(unsigned_editor->validate(pending, pos), QValidator::Intermediate);
    QString too_big(QString::number(static_cast<qulonglong>(UINT64_MAX)) + QLatin1Char('0'));
    QCOMPARE(unsigned_editor->validate(too_big, pos), QValidator::Invalid);

    // fixup()：非法输入回落到当前值，绝不让非法文本留在框里。
    QString garbage(QStringLiteral("abc"));
    unsigned_editor->fixup(garbage);
    QCOMPARE(garbage, QStringLiteral("1000"));

    // 无符号：输入负号后被修复成合法值（不提交负数）。
    typeInto(unsigned_editor, QStringLiteral("-5"));
    QCOMPARE(unsigned_editor->unsignedValue(), static_cast<quint64>(1000));

    // 有符号：负数合法，范围同样受校验。
    IntHarness signed_harness(g_variant_new_int64(5));
    IntSpinBox *const signed_editor = signed_harness.wide();
    QVERIFY(signed_editor != nullptr);
    QString negative(QStringLiteral("-42"));
    QCOMPARE(signed_editor->validate(negative, pos), QValidator::Acceptable);
    typeInto(signed_editor, QStringLiteral("-42"));
    QCOMPARE(signed_editor->signedValue(), static_cast<qint64>(-42));

    // 类型/范围上界：uint32 选项的上界是 4294967295。
    IntHarness uint32_harness(g_variant_new_uint32(1));
    IntSpinBox *const uint32_editor = uint32_harness.wide();
    QVERIFY(uint32_editor != nullptr);
    QCOMPARE(uint32_editor->unsignedMaximum(), static_cast<quint64>(4294967295));
    QString over32(QStringLiteral("4294967296"));
    QCOMPARE(uint32_editor->validate(over32, pos), QValidator::Invalid);
    typeInto(uint32_editor, QStringLiteral("3000000000"));
    QCOMPARE(uint32_editor->unsignedValue(), static_cast<quint64>(3000000000));
}

void TestPropIntWidget::explicitSmallRangeKeepsTheSpinBox()
{
    // 明确且落在 int 内的范围：值不可能越界，用熟悉的自旋框即可。
    IntHarness harness(g_variant_new_uint64(4),
                       std::make_pair<int64_t, int64_t>(1, 1000));
    QSpinBox *const spin = harness.spin();
    QVERIFY(spin != nullptr);
    QVERIFY(harness.wide() == nullptr);
    QCOMPARE(spin->value(), 4);
    QCOMPARE(spin->minimum(), 1);
    QCOMPARE(spin->maximum(), 1000);
}

void TestPropIntWidget::commitDeliversTheEditedValue()
{
    // 宽类型：编辑器里的值原样交给 setter。
    IntHarness wide_harness(g_variant_new_uint64(1000000));
    IntSpinBox *const editor = wide_harness.wide();
    QVERIFY(editor != nullptr);
    QVERIFY(wide_harness.committed() == nullptr);  // 未提交前不该有回调

    typeInto(editor, QStringLiteral("2000000"));
    wide_harness.commit();
    QVERIFY(wide_harness.committed() != nullptr);
    QVERIFY(g_variant_is_of_type(wide_harness.committed(), G_VARIANT_TYPE_UINT64));
    QCOMPARE(g_variant_get_uint64(wide_harness.committed()),
             static_cast<guint64>(2000000));

    // 窄类型：未改动时提交模块声明的原值（int32 分支不受宽类型改动影响）。
    IntHarness narrow_harness(g_variant_new_int32(32));
    QSpinBox *const spin = narrow_harness.spin();
    QVERIFY(spin != nullptr);
    narrow_harness.commit();
    QVERIFY(narrow_harness.committed() != nullptr);
    QVERIFY(g_variant_is_of_type(narrow_harness.committed(), G_VARIANT_TYPE_INT32));
    QCOMPARE(g_variant_get_int32(narrow_harness.committed()), 32);

    spin->setValue(64);
    narrow_harness.commit();
    QCOMPARE(g_variant_get_int32(narrow_harness.committed()), 64);
}

QTEST_MAIN(TestPropIntWidget)
#include "test_prop_int_widget.moc"
