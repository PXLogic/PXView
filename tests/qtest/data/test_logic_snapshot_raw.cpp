/*
 * test_logic_snapshot_raw.cpp — LogicSnapshot raw 存储路径测试
 *
 * 验证 raw 版存储 + P5 diff+ctz 毛刺滤波:
 *   - find_first_different_raw (P5 raw 字节扫描) 与 mipmap 树搜索
 *     get_nxt_edge_self 逐位置结果一致性
 *   - apply_glitch_filter 滤除窄脉冲 / 保留宽脉冲的正确性
 *
 * 依赖链: logicsnapshot + snapshot + diskcache_writer + glitch_filter
 *         + mmap_allocator + leaf_block_pool (header-only)
 * 纯数据层, 无 QWidget 依赖。
 */

#include <QtTest/QtTest>

// ── 标准库头必须在 #define private public 之前 include ──
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <libsigrok/libsigrok.h>   // sr_datafeed_logic / sr_channel / GSList

// ── xlog stub: logicsnapshot.cpp 经 log.h 引用 pxv_log + xlog_* ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

// ── 访问 private 方法: find_first_different_raw / get_nxt_edge_self ──
// 仅对本测试编译单元生效 (include 后立即 undef), 不污染其它代码。
#define private public
#define protected public
#include "pv/data/snapshot/logicsnapshot.h"
#undef private
#undef protected

using namespace pv::data;

namespace {

// 构造 interleaved (LA_SPLIT_DATA, unitsize=1) 采样数据:
//   data[s] bit0 = ch0, bit1 = ch1.
std::vector<uint8_t> build_interleaved(
    size_t n, const std::function<bool(size_t s, int ch)> &bit_fn)
{
    std::vector<uint8_t> d(n);
    for (size_t s = 0; s < n; ++s) {
        uint8_t v = 0;
        if (bit_fn(s, 0)) v |= 0x01;
        if (bit_fn(s, 1)) v |= 0x02;
        d[s] = v;
    }
    return d;
}

struct Fixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    std::vector<uint8_t> data;
    sr_datafeed_logic logic{};

    Fixture(size_t ch_count, size_t total_samples)
        : chs(ch_count), nodes(ch_count)
    {
        for (size_t i = 0; i < ch_count; ++i) {
            chs[i].index = (int)i;
            chs[i].type = SR_CHANNEL_LOGIC;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < ch_count) ? &nodes[i + 1] : nullptr;
        }
        logic.length = 0;
        logic.unitsize = (uint8_t)((ch_count + 7) / 8);
        logic.format = 0;   // LA_SPLIT_DATA (interleaved)
        logic.data = nullptr;
        (void)total_samples;
    }

    void feed(LogicSnapshot &snap, uint64_t total_sample_count)
    {
        // 一次性喂入全部数据 (interleaved)
        sr_datafeed_logic l = logic;
        l.length = (uint64_t)data.size();
        l.data = data.data();
        l.unitsize = 1;
        snap.first_payload(l, total_sample_count, &nodes[0], true);
        snap.append_payload(l);
        snap.capture_ended();
    }
};

} // anonymous namespace

class TestLogicSnapshotRaw : public QObject
{
    Q_OBJECT

private slots:
    // P5 一致性: find_first_different_raw == get_nxt_edge_self (逐位置)
    void test_find_first_different_matches_tree();
    void test_find_first_different_matches_tree_multi_block();
    void test_find_first_different_constant_tail();
    void test_find_first_different_uninstantiated_block();

    // 毛刺滤波正确性
    void test_glitch_filter_removes_narrow_pulses();
    void test_glitch_filter_keeps_wide_pulses();
};

void TestLogicSnapshotRaw::test_find_first_different_matches_tree()
{
    // 单 block (N < 65536): 交替 + 常量混合
    const size_t N = 30000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) {
            if (s < 5000) return false;                 // 常量低
            if (s < 15000) return (s % 3) < 2;          // 交替
            if (s < 22000) return true;                 // 常量高
            return (s % 1000) < 500;                    // 稀疏
        }
        return (s % 128) < 64;                          // ch1 低频
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    const int order = snap.get_ch_order(sig);
    QVERIFY(order >= 0);

    // 逐位置对比 (步长覆盖块内/块尾/常量段)
    for (uint64_t start = 0; start + 1 < N; start += 97) {
        const bool expected = snap.get_sample_self(start, sig);

        uint64_t out_raw = start;
        const bool found_raw =
            snap.find_first_different_raw(order, start, N - 1, expected, out_raw);

        uint64_t idx_tree = start;
        const bool found_tree =
            snap.get_nxt_edge_self(idx_tree, expected, N - 1, 0, sig);

        QCOMPARE(found_raw, found_tree);
        if (found_raw) {
            QCOMPARE(out_raw, idx_tree);
            // 一致性校验: out_raw 处电平确实翻转, 且前一位保持 expected
            QVERIFY(snap.get_sample_self(out_raw, sig) != expected);
            if (out_raw > start)
                QVERIFY(snap.get_sample_self(out_raw - 1, sig) == expected);
        }
    }
}

void TestLogicSnapshotRaw::test_find_first_different_matches_tree_multi_block()
{
    // 跨多个 leaf block + root 边界 (N > 2 * 65536), 覆盖 block 衔接
    const size_t N = 200000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) {
            // 每 4096 样本一段: 交替段 / 常量段 / 稀疏段 轮换
            const size_t seg = (s / 4096) % 3;
            if (seg == 0) return (s % 5) < 3;
            if (seg == 1) return (s / 4096) % 2 == 1;   // 整段常量高/低
            return (s % 512) < 256;                     // 稀疏
        }
        return false;                                   // ch1 常量低
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    const int order = snap.get_ch_order(sig);
    QVERIFY(order >= 0);

    // 覆盖 root 边界附近 (65536 倍数 ± 200) 与全范围抽样
    std::vector<uint64_t> probes;
    for (uint64_t s = 0; s + 1 < N; s += 1021)
        probes.push_back(s);
    for (uint64_t base = 65536; base < N; base += 65536) {
        for (int off = -200; off <= 200; off += 100) {
            if (base + (uint64_t)off + 1 < N)
                probes.push_back(base + (uint64_t)off);
        }
    }

    for (uint64_t start : probes) {
        const bool expected = snap.get_sample_self(start, sig);
        uint64_t out_raw = start;
        const bool found_raw =
            snap.find_first_different_raw(order, start, N - 1, expected, out_raw);
        uint64_t idx_tree = start;
        const bool found_tree =
            snap.get_nxt_edge_self(idx_tree, expected, N - 1, 0, sig);
        QCOMPARE(found_raw, found_tree);
        if (found_raw) {
            QCOMPARE(out_raw, idx_tree);
            QVERIFY(snap.get_sample_self(out_raw, sig) != expected);
            if (out_raw > start)
                QVERIFY(snap.get_sample_self(out_raw - 1, sig) == expected);
        }
    }
}

void TestLogicSnapshotRaw::test_find_first_different_constant_tail()
{
    // 尾部常量段: 搜索应一致地报告"无更多边缘"
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) {
            if (s < 70000) return (s % 2) == 0;         // 高频交替
            return false;                                // 尾部常量低
        }
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    const int order = snap.get_ch_order(sig);
    QVERIFY(order >= 0);

    // 从常量段内部开始: 两者都必须找不到边缘
    const uint64_t start = 80000;
    const bool expected = snap.get_sample_self(start, sig);
    QVERIFY(!expected);   // 常量低段

    uint64_t out_raw = start;
    const bool found_raw =
        snap.find_first_different_raw(order, start, N - 1, expected, out_raw);
    uint64_t idx_tree = start;
    const bool found_tree =
        snap.get_nxt_edge_self(idx_tree, expected, N - 1, 0, sig);

    QVERIFY(!found_raw);
    QVERIFY(!found_tree);

    // 从交替段末尾开始, 搜索应落到常量段边界
    const uint64_t start2 = 69999;
    const bool expected2 = snap.get_sample_self(start2, sig);
    uint64_t out_raw2 = start2;
    const bool found_raw2 =
        snap.find_first_different_raw(order, start2, N - 1, expected2, out_raw2);
    uint64_t idx_tree2 = start2;
    const bool found_tree2 =
        snap.get_nxt_edge_self(idx_tree2, expected2, N - 1, 0, sig);
    QCOMPARE(found_raw2, found_tree2);
    if (found_raw2) {
        QCOMPARE(out_raw2, idx_tree2);
        QVERIFY(out_raw2 == 70000);   // 交替段结束处
    }
}

void TestLogicSnapshotRaw::test_find_first_different_uninstantiated_block()
{
    // 未实例化块 (lbp==nullptr): calc_mipmap 对"整块无跳变"的完整块调用
    // push_to_free_list 释放, 常量值编码在 _ch_data[order][idx0].first bit idx1.
    //
    // 真实块几何: LeafBlockSamples = 2^24 (16M 样本/叶块)。旧版本用例假设
    // 65536 样本/块, 数据全落在块 0 内, nullptr 分支从未执行, 回归失效。
    //
    // 数据布局 (3 个叶块, ~33.6M 样本):
    //   块0 [0, LB):      低电平 + [1000,2000) 高脉冲 → 有跳变, 保持分配
    //   块1 [LB, 2LB):    常量高 (完整块) → 被释放 (lbp==nullptr)
    //   块2 [2LB, N):     部分写入, 常量低
    //
    // 回归: 原实现命中 nullptr 块时 out_pos=start 错误回跳到搜索起点
    // (start 处电平仍 == expected), 毛刺滤波把跳变误判到 start。
    const uint64_t LB = LogicSnapshot::LeafBlockSamples;
    const size_t N = (size_t)(2 * LB + 65536);
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [LB](size_t s, int ch) {
        if (ch != 0) return false;
        if (s < LB) return s >= 1000 && s < 2000;   // 块0: 低 + 窄脉冲
        if (s < 2 * LB) return true;                // 块1: 常量高
        return false;                               // 块2: 常量低
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    const int order = snap.get_ch_order(sig);
    QVERIFY(order >= 0);
    QVERIFY(snap._ch_data[order].size() > 0);
    auto &root = snap._ch_data[order][0];

    // 前提: 块1 (完整常量高) 被释放; 块0 因脉冲有跳变保持分配
    QVERIFY(root.lbp[0] != nullptr);
    QVERIFY(root.lbp[1] == nullptr);

    // A: 块0 内 (脉冲后, 电平=低) 搜索 → 扫完块0 尾部 + 命中 nullptr 块1
    //    起点 LB。回归点: bug 版返回 start=5000。
    {
        uint64_t out = 0;
        const bool f = snap.find_first_different_raw(order, 5000, N - 1, false, out);
        QVERIFY(f);
        QCOMPARE(out, LB);
        // 与 mipmap 树搜索一致
        uint64_t idx_tree = 5000;
        QVERIFY(snap.get_nxt_edge_self(idx_tree, false, N - 1, 0, sig));
        QCOMPARE(idx_tree, LB);
    }

    // B: nullptr 块1 内 (电平=高 == 常量块值) → 跳过块1, 命中块2 起点 2LB
    {
        uint64_t out = 0;
        const bool f = snap.find_first_different_raw(order, LB + 1000, N - 1, true, out);
        QVERIFY(f);
        QCOMPARE(out, 2 * LB);
        uint64_t idx_tree = LB + 1000;
        QVERIFY(snap.get_nxt_edge_self(idx_tree, true, N - 1, 0, sig));
        QCOMPARE(idx_tree, 2 * LB);
    }

    // C: nullptr 块1 内, 常量值 != expected → 返回块内当前位置 (首块时
    //    pos==start, 与 bug 版不可区分, 仅正确性校验)
    {
        uint64_t out = 0;
        const bool f = snap.find_first_different_raw(order, LB + 1000, N - 1, false, out);
        QVERIFY(f);
        QCOMPARE(out, LB + 1000);
    }
}

void TestLogicSnapshotRaw::test_glitch_filter_removes_narrow_pulses()
{
    // ch0: 低电平为主 + 窄高脉冲 (len<=5, 应滤除) + 宽高脉冲 (len>threshold, 保留)
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch != 0) return false;
        if (s >= 1000 && s < 1005) return true;    // 窄脉冲 len=5
        if (s >= 2000 && s < 2002) return true;    // 窄脉冲 len=2
        if (s >= 10000 && s < 12000) return true;  // 宽脉冲 len=2000
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const uint32_t threshold = 20;
    snap.apply_glitch_filter(0, threshold, nullptr, GlitchFilterMode::Both);

    // 窄脉冲被覆盖为 accepted_level=false
    for (uint64_t s = 1000; s < 1005; ++s)
        QVERIFY2(!snap.get_sample(s, 0), "narrow pulse should be filtered");
    for (uint64_t s = 2000; s < 2002; ++s)
        QVERIFY2(!snap.get_sample(s, 0), "narrow pulse should be filtered");

    // 宽脉冲保留 (稳定迁移, 电平为高)
    for (uint64_t s = 10000; s < 12000; ++s)
        QVERIFY2(snap.get_sample(s, 0), "wide pulse should be kept");

    // 滤波后仍可运行渲染级搜索 (不因 mipmap 失效而崩溃)
    uint64_t idx = 0;
    const bool has_edge = snap.get_nxt_edge(idx, snap.get_sample(0, 0),
                                            N - 1, 0, 0);
    Q_UNUSED(has_edge);
}

void TestLogicSnapshotRaw::test_glitch_filter_keeps_wide_pulses()
{
    // 高电平为主 + 窄低脉冲 (len<=5, 应滤除) + 宽低脉冲 (保留)
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch != 0) return false;
        if (s >= 1000 && s < 1005) return false;   // 窄低脉冲
        if (s >= 10000 && s < 12000) return false; // 宽低脉冲
        return true;                               // 默认高
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const uint32_t threshold = 20;
    snap.apply_glitch_filter(0, threshold, nullptr, GlitchFilterMode::Both);

    // 窄低脉冲被覆盖为 accepted_level=true
    for (uint64_t s = 1000; s < 1005; ++s)
        QVERIFY2(snap.get_sample(s, 0), "narrow low pulse should be filtered");

    // 宽低脉冲保留
    for (uint64_t s = 10000; s < 12000; ++s)
        QVERIFY2(!snap.get_sample(s, 0), "wide low pulse should be kept");
}

QTEST_GUILESS_MAIN(TestLogicSnapshotRaw)
#include "test_logic_snapshot_raw.moc"
