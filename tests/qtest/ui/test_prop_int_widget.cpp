/*
 * test_prop_int_widget.cpp — pv::prop::Int 编辑器（QSpinBox）的取值范围契约。
 *
 * 背景（2026-09-24 用户报告）：导入 raw binary 时，选项对话框里
 * "Number of logic channels"（int32 选项）能正确回填文件名里的 32，但
 * "Sample rate (Hz)"（uint64 选项）永远是 0，即使把 1000000 传进预填值。
 *
 * 根因在 Int::get_widget()：uint64 分支把 range_max 设成 UINT64_MAX，而这个
 * 变量是 int64_t —— UINT64_MAX 窄化后是 -1，随后
 *     range_max = min(range_max, (int64_t)INT_MAX)   → -1
 *     _spin_box->setRange(0, -1)
 * 而 Qt 的规则是 max < min 时 min 成为唯一合法值（文档明写），于是
 * setValue(1000000) 被吞掉、编辑器被钉死在 0。binary 模块的 "samplerate" 是
 * PXView 第一个绑定到控件的 uint64 选项，所以这个既有缺陷这时才暴露。
 *
 * 本文件锁住三件事：
 *   * uint64 选项必须显示传进来的值（且 maximum 是 INT_MAX 这种合法范围）；
 *   * 超出编辑器表示能力的值（uint32 > INT_MAX、uint64 >= 2^31）必须被夹到
 *     上界，而不是回绕成负数落到下界；
 *   * int32 选项的取值范围不受影响（回归保护）。
 *
 * 只编译 pv/prop/{property,int}.cpp 与 pv/ui/dsspinbox.cpp，Qt Widgets 环境
 * （ctest 已设 QT_QPA_PLATFORM=offscreen），不需要 libsigrok。
 */

#include <QtTest/QtTest>

#include <QSpinBox>

#include <climits>
#include <cstdint>
#include <optional>
#include <utility>

#include "pv/prop/int.h"

namespace {

/* 造一个只读的 Int 属性：getter 每次交出一个新引用（Property::Getter 契约），
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
    QSpinBox *spin()
    {
        QWidget *const widget = _prop->get_widget_deferred(nullptr);
        if (!_widget)
            _widget = widget;
        return qobject_cast<QSpinBox *>(widget);
    }

    /// 驱动 OK 路径：commit() 应把编辑器里的值交给 setter。
    void commit() { _prop->commit(); }

    /// 最近一次 setter 收到的值（新引用，属于本对象）。
    GVariant *committed() const { return _committed; }

    QWidget *widget() const { return _widget; }

private:
    GVariant *_initial = nullptr;
    GVariant *_committed = nullptr;
    pv::prop::Int *_prop = nullptr;
    QWidget *_widget = nullptr;
};

} // namespace

class TestPropIntWidget : public QObject
{
    Q_OBJECT

private slots:
    /* uint64 选项（binary 的 samplerate）：显示预填值，范围合法。 */
    void uint64OptionShowsTheGivenValue();

    /* 超过 INT_MAX 的 uint64 被夹到上界，而不是显示 0。 */
    void uint64AboveIntMaxClampsToUpperBound();

    /* uint32 > INT_MAX 同样夹紧（此前的窄化回绕会让它落到下界）。 */
    void uint32AboveIntMaxClampsToUpperBound();

    /* int32 选项（binary 的 numchannels）不受影响。 */
    void int32OptionKeepsItsValueAndRange();

    /* 编辑器里改值后 commit() 会带着新值回调 setter。 */
    void commitDeliversTheEditedValue();
};

void TestPropIntWidget::uint64OptionShowsTheGivenValue()
{
    IntHarness harness(g_variant_new_uint64(1000000));
    QSpinBox *const spin = harness.spin();
    QVERIFY(spin != nullptr);

    // 修复前：maximum() 是 0（setRange(0, -1) 之后 min 成为唯一合法值），
    // value() 也就是 0 —— 预填的 1000000 被吞掉。
    QCOMPARE(spin->value(), 1000000);
    QCOMPARE(spin->minimum(), 0);
    QVERIFY(spin->maximum() >= 1000000);
}

void TestPropIntWidget::uint64AboveIntMaxClampsToUpperBound()
{
    IntHarness harness(g_variant_new_uint64(static_cast<guint64>(UINT64_MAX)));
    QSpinBox *const spin = harness.spin();
    QVERIFY(spin != nullptr);

    QCOMPARE(spin->value(), INT_MAX);
    QCOMPARE(spin->minimum(), 0);
    QCOMPARE(spin->maximum(), INT_MAX);
}

void TestPropIntWidget::uint32AboveIntMaxClampsToUpperBound()
{
    IntHarness harness(g_variant_new_uint32(3000000000u));
    QSpinBox *const spin = harness.spin();
    QVERIFY(spin != nullptr);

    QCOMPARE(spin->value(), INT_MAX);
    QCOMPARE(spin->minimum(), 0);
    QCOMPARE(spin->maximum(), INT_MAX);
}

void TestPropIntWidget::int32OptionKeepsItsValueAndRange()
{
    IntHarness harness(g_variant_new_int32(32));
    QSpinBox *const spin = harness.spin();
    QVERIFY(spin != nullptr);

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
}

void TestPropIntWidget::commitDeliversTheEditedValue()
{
    IntHarness harness(g_variant_new_uint64(1000000));
    QSpinBox *const spin = harness.spin();
    QVERIFY(spin != nullptr);
    QVERIFY(harness.committed() == nullptr);  // 未提交前不该有回调

    // 用户在对话框里把采样率改掉（这里绕过键盘直接设值）。
    spin->setValue(2000000);
    QCOMPARE(spin->value(), 2000000);

    // accept() → Binding::commit() → Int::commit()：编辑器里的值必须原样交给
    // setter，否则"用户填的值"就永远进不了选项表（uint64 分支修好后这条才成立；
    // 之前编辑器被钉在 0，交回来的是 0）。
    harness.commit();
    QVERIFY(harness.committed() != nullptr);
    QVERIFY(g_variant_is_of_type(harness.committed(), G_VARIANT_TYPE_UINT64));
    QCOMPARE(g_variant_get_uint64(harness.committed()), static_cast<guint64>(2000000));
}

QTEST_MAIN(TestPropIntWidget)
#include "test_prop_int_widget.moc"
