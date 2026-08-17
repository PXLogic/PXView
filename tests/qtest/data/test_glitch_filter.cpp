/*
 * test_glitch_filter.cpp — Glitch Filter 纯单通道逻辑单元测试 (T9 / R4)
 *
 * 直接驱动从 apply_glitch_filter 抽出的纯函数 apply_glitch_filter_one_pass：
 *   给定一段单通道 0/1 电平样本 + 最小脉宽阈值 → 输出毛刺被滤除后的样本。
 * 与 test_logic_snapshot_raw 的「状态机单测」互补：本文件不依赖 LogicSnapshot
 * 实例/块树/锁，逐用例校验判定语义（窄脉冲拉平、宽沿保留、阈值边界、mode 反转）。
 *
 * 依赖链（CMake 注册时应与 test_logic_snapshot_raw 相同）:
 *   logicsnapshot_glitch_filter.cpp + logicsnapshot.cpp + snapshot.cpp
 *   + logicsnapshot_diskcache_writer.cpp + mmap_allocator.cpp
 * 头文件经 logicsnapshot_glitch_filter.h 传递包含 logicsnapshot.h → 需要
 * include 目录: libsigrok/include、common (xlog.h)。
 * 纯数据层, 无 QWidget 依赖。
 */

#include <QtTest/QtTest>

// ── 标准库头必须在 xlog stub 之前 include ──
#include <cstdint>
#include <vector>

// ── xlog stub: 链接 logicsnapshot.cpp / logicsnapshot_glitch_filter.cpp 经
//    log.h 引用 pxv_log + xlog_* 符号 (即便本文件只调用纯函数也会拉入整链)。
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include "pv/data/snapshot/logicsnapshot_glitch_filter.h"

using namespace pv::data;

namespace {

// 便捷封装：对单通道字节流 (1 字节/样本, 非 0 为高) 跑一趟纯滤波，返回输出。
std::vector<uint8_t> run_filter(const std::vector<uint8_t> &in,
                                uint32_t threshold,
                                GlitchFilterMode mode)
{
    std::vector<uint8_t> out(in.size());
    apply_glitch_filter_one_pass(in.data(), out.data(), in.size(),
                                 threshold, mode);
    return out;
}

} // anonymous namespace

class TestGlitchFilter : public QObject
{
    Q_OBJECT

private slots:
    // 窄毛刺（单样本/多样本）被拉平到周围稳定电平
    void test_narrow_single_sample_filtered();
    void test_narrow_multi_sample_filtered();

    // 正常宽度沿/电平保留
    void test_wide_pulse_kept();

    // 端到端：毛刺与正常沿混合
    void test_mixed_end_to_end();

    // 阈值边界：脉宽恰等于阈值（生产语义 <= 判定为毛刺）
    void test_threshold_boundary_is_filtered();

    // invert/mode 语义：High/Low 只滤特定偏置方向上的窄脉冲
    void test_mode_high_filters_low_dip_on_high();
    void test_mode_low_filters_high_spike_on_low();
};

void TestGlitchFilter::test_narrow_single_sample_filtered()
{
    // 基准低, [10,11) 单样本高刺, len=1 <= threshold=5 → 拉平为低
    std::vector<uint8_t> in(100, 0);
    in[10] = 1;

    const auto out = run_filter(in, 5, GlitchFilterMode::Both);
    QCOMPARE(out.size(), in.size());
    for (size_t i = 0; i < out.size(); ++i)
        QVERIFY2(out[i] == 0, "single-sample narrow high spike should be filtered");
}

void TestGlitchFilter::test_narrow_multi_sample_filtered()
{
    // 基准高, [20,24) 低凹, len=4 <= threshold=5 → 拉平为高
    std::vector<uint8_t> in(100, 1);
    for (size_t i = 20; i < 24; ++i)
        in[i] = 0;

    const auto out = run_filter(in, 5, GlitchFilterMode::Both);
    QCOMPARE(out.size(), in.size());
    for (size_t i = 0; i < out.size(); ++i)
        QVERIFY2(out[i] == 1, "multi-sample narrow low dip should be filtered");
}

void TestGlitchFilter::test_wide_pulse_kept()
{
    // 基准低, [100,1100) 宽高脉冲 len=1000 > threshold=20 → 保留（正常沿）
    const size_t N = 1200;
    std::vector<uint8_t> in(N, 0);
    for (size_t i = 100; i < 1100; ++i)
        in[i] = 1;

    const auto out = run_filter(in, 20, GlitchFilterMode::Both);
    for (size_t i = 0; i < 100; ++i)
        QVERIFY2(out[i] == 0, "leading low level preserved");
    for (size_t i = 100; i < 1100; ++i)
        QVERIFY2(out[i] == 1, "wide high pulse preserved");
    for (size_t i = 1100; i < N; ++i)
        QVERIFY2(out[i] == 0, "trailing low level preserved");
}

void TestGlitchFilter::test_mixed_end_to_end()
{
    // 输入 (threshold=10, Both):
    //   [0,300) 低 (含 [50,52) 窄高刺 len=2           → 滤除为低)
    //   [300,N) 高 (含 [600,603) 窄低凹 len=3         → 滤除为高)
    // 唯一稳定沿 300 处被保留。预期输出: [0,300)=0, [300,N)=1。
    const size_t N = 1000;
    std::vector<uint8_t> in(N, 0);
    in[50] = 1; in[51] = 1;                       // 窄高刺 len=2
    for (size_t i = 300; i < 600; ++i)
        in[i] = 1;                                // 宽高脉冲 len=300
    in[600] = 0; in[601] = 0; in[602] = 0;        // 窄低凹 len=3
    for (size_t i = 603; i < N; ++i)
        in[i] = 1;

    const auto out = run_filter(in, 10, GlitchFilterMode::Both);
    for (size_t i = 0; i < 300; ++i)
        QVERIFY2(out[i] == 0,
                 "low segment (with narrow high glitch) flattened to low");
    for (size_t i = 300; i < N; ++i)
        QVERIFY2(out[i] == 1,
                 "high segment (with narrow low glitch) flattened to high");
}

void TestGlitchFilter::test_threshold_boundary_is_filtered()
{
    // 生产语义为 pulse_len <= threshold → 判为毛刺并滤除。因此脉宽恰等于阈值
    // 也应被滤除（未保留）。基准低, [10,15) 高脉冲 len=5 == threshold=5。
    std::vector<uint8_t> in(100, 0);
    for (size_t i = 10; i < 15; ++i)
        in[i] = 1;

    const auto out = run_filter(in, 5, GlitchFilterMode::Both);
    for (size_t i = 0; i < out.size(); ++i)
        QVERIFY2(out[i] == 0, "pulse exactly at threshold is filtered (<=)");

    // 相反地，len = threshold+1 的脉宽应保留（正常沿）
    std::vector<uint8_t> in2(100, 0);
    for (size_t i = 10; i <= 15; ++i)      // [10,16) len=6
        in2[i] = 1;
    const auto out2 = run_filter(in2, 5, GlitchFilterMode::Both);
    for (size_t i = 10; i <= 15; ++i)
        QVERIFY2(out2[i] == 1, "pulse just above threshold is kept");
}

void TestGlitchFilter::test_mode_high_filters_low_dip_on_high()
{
    // 基准高, [40,45) 窄低凹 len=5 <= threshold=8。
    const size_t N = 100;
    std::vector<uint8_t> in(N, 1);
    for (size_t i = 40; i < 45; ++i)
        in[i] = 0;

    // Both / High: 基准为高, 应滤除该低凹
    for (const auto mode : {GlitchFilterMode::Both, GlitchFilterMode::High}) {
        const auto out = run_filter(in, 8, mode);
        for (size_t i = 0; i < N; ++i)
            QVERIFY2(out[i] == 1, "low dip on high base filtered in Both/High mode");
    }

    // Low: 基准为高 → 不滤，低凹保留（作为稳定迁移）
    const auto out_low = run_filter(in, 8, GlitchFilterMode::Low);
    for (size_t i = 0; i < 40; ++i)
        QVERIFY2(out_low[i] == 1, "high level before dip preserved under Low mode");
    for (size_t i = 40; i < 45; ++i)
        QVERIFY2(out_low[i] == 0, "low dip kept under Low mode");
    for (size_t i = 45; i < N; ++i)
        QVERIFY2(out_low[i] == 1, "high level after dip preserved under Low mode");
}

void TestGlitchFilter::test_mode_low_filters_high_spike_on_low()
{
    // 基准低, [40,45) 窄高刺 len=5 <= threshold=8。
    const size_t N = 100;
    std::vector<uint8_t> in(N, 0);
    for (size_t i = 40; i < 45; ++i)
        in[i] = 1;

    // Both / Low: 基准为低, 应滤除该高刺
    for (const auto mode : {GlitchFilterMode::Both, GlitchFilterMode::Low}) {
        const auto out = run_filter(in, 8, mode);
        for (size_t i = 0; i < N; ++i)
            QVERIFY2(out[i] == 0, "high spike on low base filtered in Both/Low mode");
    }

    // High: 基准为低 → 不滤，高刺保留
    const auto out_high = run_filter(in, 8, GlitchFilterMode::High);
    for (size_t i = 0; i < 40; ++i)
        QVERIFY2(out_high[i] == 0, "low level before spike preserved under High mode");
    for (size_t i = 40; i < 45; ++i)
        QVERIFY2(out_high[i] == 1, "high spike kept under High mode");
    for (size_t i = 45; i < N; ++i)
        QVERIFY2(out_high[i] == 0, "low level after spike preserved under High mode");
}

QTEST_GUILESS_MAIN(TestGlitchFilter)
#include "test_glitch_filter.moc"