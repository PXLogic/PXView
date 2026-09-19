/*
 * test_voltage_scale_factors.cpp — DSO 电压换算因子契约
 *
 * 背景
 * ----
 * P1-d 审计（devdoc/P0-P1-P2架构设计与实施计划.md §4.9）结论：
 * 4 处 V/div 换算**不是重复实现**，不能按原计划合并。审计顺带定位了一个真缺陷，
 * 已在 §4.9.2 修复。本文件是该修复的契约与回归测试。
 *
 * 上游 DSView 的电压公式（Reference/DSView-master/DSView/pv/view/dsosignal.cpp
 * get_voltage 与 view/component/dso_measure.cpp）:
 *     v_mV = v * data_scale * k * _vDial->get_factor() * DS_CONF_DSO_VDIVS / height
 *   k                    = ch->get_vDial()->get_value()   ← V/div 档位 (mV/div)
 *   _vDial->get_factor() = 探头衰减因子 (1/10/100)
 *   data_scale           = ch->get_scale()
 *                        = height / (ref_max - ref_min) * stop_scale
 * 即 **三个独立量**。
 *
 * 修复前的缺陷（§4.9.2）
 * ----------------------
 * 移植版在 session/sigsession.cpp:1318-1320 与 core/sessionstatecontext.cpp:291-293
 * 把两个量都写错了：
 *     set_measure_voltage_factor(m->vfactor())      ← 应为 m->vdiv_mv()
 *     set_data_scale(m->vdiv_mv())                  ← 应为 1/(ref_max-ref_min)
 * 后果有两个，且互相独立：
 *   1) ADC 计数路径（测量面板 / hover 读数）里 vfactor 被乘两次 → 读数偏大
 *      probe × 255/h 倍（10× 探头 + h=256 时约 9.96 倍）；
 *   2) 像素路径（游标 ΔV、触发电平，scaled=true 只乘 k 不乘 data_scale）偏差
 *      vdiv/probe 倍 → 1V/div + 1× 探头时读数小了 1000 倍。
 * 该处上方原作者留有注释 “// TODO: verify - vfactor and vdiv replace
 * view::DsoSignal getters.” —— 这正是被 verify 掉的那条 TODO。
 *
 * 修复
 * ----
 *   measure_voltage_factor ← m->vdiv_mv()   (mV/div)
 *   data_scale            ← kAdcScale       (1/255, ADC 满量程倒数)
 *   vfactor               ← m->vfactor()    (探头因子)
 * 并且去掉 convert_voltage() 里的 /view_rect_height —— 上游的 height 只存在于
 * get_scale() 内部，与消费者除以的 height 相消，净因子恒为 1/(ref_max-ref_min)。
 * 结果：GUI 与 headless 一致，且与 mathstack.cpp / spectrumstack.cpp 的
 * `vdiv * vfactor * 8 / 255` 标量**完全等价**（不再是近似）。
 *
 * 纯数据层，无 QWidget 依赖。
 */

#include <QtTest/QtTest>

#include <cmath>

#include "pv/core/measure_format.h"

using namespace pv::core;

namespace {

// 上游语义下的"每 ADC 计数的电压"标量因子（V/ADC count）
// 与 spectrumstack.cpp:169 / mathstack.cpp:446 同构
double adc_to_volt_scale(double vdiv, double vfactor)
{
    return vdiv * vfactor * kDsoVdivs / (1000.0 * (double)kAdcFullScale);
}

// 复现**修复前**的公式与赋值（仅本测试内使用，用于回归表征）。
// 旧公式带 /view_rect_height，且旧赋值把 vfactor 同时写进 measure_vf。
double legacy_convert_voltage(double raw_adc, double data_scale,
                              uint64_t measure_vf, uint64_t vfactor,
                              int view_rect_height)
{
    if (view_rect_height <= 0) {
        view_rect_height = 256;
    }
    return raw_adc * data_scale * (double)measure_vf * (double)vfactor
           * (double)kDsoVdivs / (double)view_rect_height;
}

} // anonymous namespace

class TestVoltageScaleFactors : public QObject
{
    Q_OBJECT

private slots:
    void test_constants();
    void test_matches_upstream_formula();
    void test_factor_roles_are_independent();
    void test_full_scale_sanity();
    void test_equivalence_with_spectrum_and_math();
    void test_regression_probe_factor_was_squared();
    void test_regression_cursor_pixel_path_used_probe_as_vdiv();
    void test_unit_boundary_mv_v_roundtrip();
    void test_regression_api_probe_config_wrote_vdiv_as_mv();
};

// ---------------------------------------------------------------------------
// 契约 1：常量
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_constants()
{
    QCOMPARE(kDsoVdivs, 8);              // DS_CONF_DSO_VDIVS
    QCOMPARE(kAdcFullScale, 255);        // 8-bit DSO: ref_max - ref_min
    QCOMPARE(kAdcScale, 1.0 / 255.0);    // counts -> 0..1
}

// ---------------------------------------------------------------------------
// 契约 2：公式形状 —— v_mV = raw * data_scale * measure_vf * vfactor * 8
//        （**没有** view height：它在上游是与 get_scale() 相消的）
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_matches_upstream_formula()
{
    const double raw = 100.0;
    const double vdiv = 500.0;      // mV/div
    const uint64_t probe = 10;      // 10× 探头

    QCOMPARE(convert_voltage(raw, kAdcScale, (uint64_t)vdiv, probe),
             raw * kAdcScale * vdiv * (double)probe * (double)kDsoVdivs);
}

// ---------------------------------------------------------------------------
// 契约 3：三个因子相互独立且线性
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_factor_roles_are_independent()
{
    const double raw = 100.0;
    const double vdiv = 500.0;
    const uint64_t probe = 10;

    const double base = convert_voltage(raw, kAdcScale, 1, 1);

    // data_scale（ADC 归一化）线性
    QCOMPARE(convert_voltage(raw, kAdcScale * 2.0, 1, 1), base * 2.0);

    // measure_vf（V/div 档位）线性
    QCOMPARE(convert_voltage(raw, kAdcScale, (uint64_t)vdiv, 1),
             base * vdiv);

    // vfactor（探头因子）线性
    QCOMPARE(convert_voltage(raw, kAdcScale, 1, probe),
             base * (double)probe);

    // 三者相乘 —— 与逐项相乘一致，没有任何因子被重复计入
    QCOMPARE(convert_voltage(raw, kAdcScale, (uint64_t)vdiv, probe),
             base * vdiv * (double)probe);
}

// ---------------------------------------------------------------------------
// 契约 4：满量程标定
//   1 V/div × 8 格 = 8 V = 8000 mV（1× 探头）
//   10× 探头 → 80 V = 80000 mV
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_full_scale_sanity()
{
    const double vdiv = 1000.0;   // 1 V/div，单位 mV/div

    // ADC 满量程 (255 counts) 应恰好对应 8 格
    QCOMPARE(convert_voltage((double)kAdcFullScale, kAdcScale, (uint64_t)vdiv, 1),
             8000.0);
    QCOMPARE(convert_voltage((double)kAdcFullScale, kAdcScale, (uint64_t)vdiv, 10),
             80000.0);

    // 半量程
    QCOMPARE(convert_voltage((double)kAdcFullScale / 2.0, kAdcScale,
                             (uint64_t)vdiv, 1),
             4000.0);
}

// ---------------------------------------------------------------------------
// 契约 5：与 spectrumstack.cpp:169 / mathstack.cpp:446 的标量完全等价
//
//   convert_voltage(raw, kAdcScale, vdiv, vfactor)  [mV]
//     == 1000 * raw * adc_to_volt_scale(vdiv, vfactor)  [mV]
//
// 修复前这条只在 `k == h/255`（h 为 255 的倍数）时近似成立；修复后恒等成立。
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_equivalence_with_spectrum_and_math()
{
    const double raw = 137.0;
    const double vdiv = 500.0;
    const double vfactor = 1.0;

    const double spectrum_mv = 1000.0 * raw * adc_to_volt_scale(vdiv, vfactor);
    QCOMPARE(convert_voltage(raw, kAdcScale, (uint64_t)vdiv, (uint64_t)vfactor),
             spectrum_mv);

    // 探头因子同样恒等
    const double vfactor10 = 10.0;
    const double spectrum_mv10 = 1000.0 * raw * adc_to_volt_scale(vdiv, vfactor10);
    QCOMPARE(convert_voltage(raw, kAdcScale, (uint64_t)vdiv, 10), spectrum_mv10);
}

// ---------------------------------------------------------------------------
// 回归 1：ADC 计数路径 —— vfactor 曾经被乘两次
//
// 修复前 sigsession.cpp:1318 把 m->vfactor() 写进 measure_voltage_factor，
// 而 vfactor 又被单独传入 → probe²，且残留 255/256 的 height 误差。
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_regression_probe_factor_was_squared()
{
    const double raw = 100.0;
    const double vdiv = 500.0;
    const uint64_t probe = 10;
    const int legacy_h = 256;   // 旧 headless 默认高度

    // 修复后：探头因子只乘一次
    const double fixed = convert_voltage(raw, kAdcScale, (uint64_t)vdiv, probe);
    const double fixed_unit = convert_voltage(raw, kAdcScale, (uint64_t)vdiv, 1);
    QCOMPARE(fixed, fixed_unit * (double)probe);

    // 修复前：data_scale=vdiv、measure_vf=probe、vfactor=probe
    const double legacy =
        legacy_convert_voltage(raw, vdiv, probe, probe, legacy_h);

    // 旧值 / 正确值 == probe * kAdcFullScale / legacy_h
    //   probe 倍来自 vfactor 被平方；kAdcFullScale/legacy_h (=255/256) 来自
    //   旧公式里那个本应相消的 /view_rect_height。
    const double expected_ratio =
        (double)probe * (double)kAdcFullScale / (double)legacy_h;
    const double ratio = legacy / fixed;
    QVERIFY2(std::abs(ratio - expected_ratio) / expected_ratio < 1e-9,
             qPrintable(QString("legacy/correct = %1, expected %2")
                            .arg(ratio).arg(expected_ratio)));

    // probe == 1 时缺陷几乎不可见（只剩 255/256 ≈ 0.4% 的 height 残留），
    // 这正是它长期潜伏、只在小数点后暴露的原因
    const double legacy_unit =
        legacy_convert_voltage(raw, vdiv, 1, 1, legacy_h);
    const double unit_ratio = legacy_unit / fixed_unit;
    QVERIFY(std::abs(unit_ratio - (double)kAdcFullScale / (double)legacy_h) < 1e-9);
    QVERIFY(std::abs(unit_ratio - 1.0) < 0.005);
}

// ---------------------------------------------------------------------------
// 回归 2：像素路径（scaled=true）—— k 曾经是 probe，而不是 vdiv
//
// view::DsoMeasure::get_voltage(v, p, scaled=true) 只乘 k，不乘 data_scale：
//     v_mV = v_pixels * k * probe * DS_CONF_DSO_VDIVS / height
// 修复前 k = probe → 读数偏差 probe/vdiv 倍（1V/div + 1× 探头时小 1000 倍）。
// 本用例在纯算术层钉住正确语义：k 必须是 V/div 档位。
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_regression_cursor_pixel_path_used_probe_as_vdiv()
{
    const double vdiv = 1000.0;    // 1 V/div
    const uint64_t probe = 1;
    const int height = 256;        // 8 格 × 32 px
    const double pixels = 32.0;    // 恰好 1 格

    // 正确：1 格 × 1V/div × 1× 探头 = 1000 mV
    const double correct = pixels * vdiv * (double)probe * (double)kDsoVdivs
                           / (double)height;
    QCOMPARE(correct, 1000.0);

    // 修复前 k = probe = 1 → 只有 32 mV，小 31.25 倍
    const double legacy = pixels * (double)probe * (double)probe
                          * (double)kDsoVdivs / (double)height;
    QVERIFY(legacy < correct);
    QVERIFY(std::abs(correct / legacy - vdiv / (double)probe) < 1e-9);
}

// ---------------------------------------------------------------------------
// 契约 8：mV <-> V 单位边界（core::kMvPerVolt / volts_to_millivolts）
//
// 项目里两种单位并存：内部/驱动/UI 是 mV/div，对外 API 的 ProbeConfig::vdiv
// 是 V/div。跨边界只允许走具名换算。
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_unit_boundary_mv_v_roundtrip()
{
    QCOMPARE(kMvPerVolt, 1000.0);
    QCOMPARE(volts_to_millivolts(1.0), 1000.0);
    QCOMPARE(millivolts_to_volts(1000.0), 1.0);

    const double samples[] = {0.01, 0.1, 0.5, 1.0, 2.0, 10.0};
    for (double v : samples) {
        QCOMPARE(millivolts_to_volts(volts_to_millivolts(v)), v);
    }

    // convert_voltage() 的返回值单位是**毫伏**，跨到"伏特"必须显式换算
    const double v_mv = convert_voltage(255.0, kAdcScale, 1000, 1);
    QCOMPARE(v_mv, 8000.0);                              // 4 格 × 1V/div × 8
    QCOMPARE(millivolts_to_volts(v_mv), 8.0);
}

// ---------------------------------------------------------------------------
// 回归 3：API 路径把 V/div 直接写进 mV/div 的字段（1000× 偏差）
//
// SessionService::set_probe_config() 曾经执行 `m->set_vdiv(config.vdiv)`，
// 而 API 契约的 config.vdiv 是 **V/div**、SignalModel 内部是 **mV/div**。
// 于是 1 V/div 变成 1 mV/div，`(uint64_t)m->vdiv_mv()` 取到 1 而不是 1000，
// 灌进 measure_voltage_factor 后 DSO 测量电压整体偏小 1000×。
//
// 修复：跨边界用 core::volts_to_millivolts()。
// ---------------------------------------------------------------------------
void TestVoltageScaleFactors::test_regression_api_probe_config_wrote_vdiv_as_mv()
{
    const double api_vdiv_v = 1.0;   // API 契约单位：1 V/div
    const uint64_t probe = 1;
    const double raw_full = 255.0;   // 8-bit 满量程

    // 修复后：API V/div -> SignalModel mV/div -> measure_voltage_factor
    const uint64_t measure_vf_fixed =
        (uint64_t)volts_to_millivolts(api_vdiv_v);
    QCOMPARE(measure_vf_fixed, (uint64_t)1000);

    // 满量程 255 对应 ±4 格 × 1V/div × 1× = 8000 mV = 8 V
    const double fixed_mv =
        convert_voltage(raw_full, kAdcScale, measure_vf_fixed, probe);
    QCOMPARE(fixed_mv, 8000.0);

    // 修复前：V/div 被当成 mV/div 存 → measure_voltage_factor = 1
    const uint64_t measure_vf_legacy = (uint64_t)api_vdiv_v;   // == 1
    const double legacy_mv =
        convert_voltage(raw_full, kAdcScale, measure_vf_legacy, probe);
    QCOMPARE(legacy_mv, 8.0);

    // 偏差恰好 1000×，且与 kMvPerVolt 一致
    QCOMPARE(fixed_mv / legacy_mv, kMvPerVolt);
}

QTEST_GUILESS_MAIN(TestVoltageScaleFactors)
#include "test_voltage_scale_factors.moc"
