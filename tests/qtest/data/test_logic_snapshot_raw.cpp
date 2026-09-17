/*
 * test_logic_snapshot_raw.cpp — LogicSnapshot raw 存储路径测试
 *
 * 验证 raw 版存储 + P5 diff+ctz 毛刺滤波:
 *   - find_first_different_raw (P5 raw 字节扫描) 与 mipmap 树搜索
 *     get_nxt_edge 逐位置结果一致性
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
#include <shared_mutex>
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

// ── 访问 private 方法: find_first_different_raw / get_nxt_edge ──
// 仅对本测试编译单元生效 (include 后立即 undef), 不污染其它代码。
#define private public
#define protected public
#include "pv/data/snapshot/logicsnapshot.h"
// Also pull in the glitch-filter subsystem while privates are exposed: the
// dense-train regression test asserts on the edit RECORD COUNT (that is the
// mechanism the field bug broke), and _edits is private. logicsnapshot.h only
// forward-declares the class, so without this include it stays incomplete.
#include "pv/data/snapshot/logicsnapshot_glitch_filter.h"
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
    // P5 一致性: find_first_different_raw == get_nxt_edge (逐位置)
    void test_find_first_different_matches_tree();
    void test_find_first_different_matches_tree_multi_block();
    void test_find_first_different_constant_tail();
    void test_find_first_different_uninstantiated_block();

    // 毛刺滤波正确性
    void test_glitch_filter_removes_narrow_pulses();
    void test_glitch_filter_keeps_wide_pulses();

    // 可逆编辑日志 (取代 _logic_backup 全量备份的撤销机制)
    // 注意: 槽名不得以 "_data" 结尾 —— QtTest 把它当作数据提供者
    // (foo_data() 约定)，会静默地从不执行。
    void test_revert_all_edits_restores_capture_samples();
    void test_revert_all_edits_invert_round_trip();
    void test_dense_glitch_train_merges_edit_records();
    // 渐进刷新: 每提交一批就地编辑, 必须对外发出一次"可见数据变了"通知
    void test_batch_callback_publishes_each_committed_batch();
    // 撤销(clear/undo)同样跑在 worker 线程, 大文件撤销期间靠进度通知显示进展
    void test_revert_publishes_progress();

    // 编辑可见性守卫成本 (seqlock 快路径 vs 无条件 shared_lock)
    void test_edit_guard_cost_ratio();
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
            snap.get_nxt_edge(idx_tree, expected, N - 1, 0, sig);

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
            snap.get_nxt_edge(idx_tree, expected, N - 1, 0, sig);
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
        snap.get_nxt_edge(idx_tree, expected, N - 1, 0, sig);

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
        snap.get_nxt_edge(idx_tree2, expected2, N - 1, 0, sig);
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
        QVERIFY(snap.get_nxt_edge(idx_tree, false, N - 1, 0, sig));
        QCOMPARE(idx_tree, LB);
    }

    // B: nullptr 块1 内 (电平=高 == 常量块值) → 跳过块1, 命中块2 起点 2LB
    {
        uint64_t out = 0;
        const bool f = snap.find_first_different_raw(order, LB + 1000, N - 1, true, out);
        QVERIFY(f);
        QCOMPARE(out, 2 * LB);
        uint64_t idx_tree = LB + 1000;
        QVERIFY(snap.get_nxt_edge(idx_tree, true, N - 1, 0, sig));
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

// ────────────────────────────────────────────────────────────────────────────
// 可逆编辑日志
// ────────────────────────────────────────────────────────────────────────────

void TestLogicSnapshotRaw::test_revert_all_edits_restores_capture_samples()
{
    // 数据设计要点:
    //   - 1 采样宽的窄高脉冲每 20 采样一个 → N/20 = 100000 个毛刺,
    //     超过 apply_batch 的 65536 条刷写阈值, 因此同一个叶子块会在
    //     **多个 batch** 里被反复改写。这正好覆盖"反序回放"的关键路径:
    //     每笔记录保存的是它自己那次写之前的字节, 只有倒序回放才能回到
    //     采集原始态 (正序回放会停在中间某一版)。
    const size_t N = 2000000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch != 0) return false;
        return (s % 20) == 0;      // 1-sample 窄高脉冲
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    QVERIFY(snap.get_sample(0, sig));

    // 记录采集原始数据
    std::vector<uint8_t> original(N);
    for (size_t s = 0; s < N; ++s)
        original[s] = snap.get_sample(s, sig) ? 1 : 0;

    QVERIFY(!snap.has_filter_edits());

    // threshold=2 > 脉冲宽度 1 → 全部判为毛刺并抹平
    snap.apply_glitch_filter(sig, 2, nullptr, GlitchFilterMode::Both);

    QVERIFY2(!snap.edit_log_overflowed(),
             "edit log budget must not be hit for 100k single-sample glitches");
    QVERIFY2(snap.has_filter_edits(), "a successful pass must be reversible");

    // 滤波确实改动了数据 (若这一条不成立, 下面的还原断言就是同义反复)
    size_t changed = 0;
    for (size_t s = 0; s < N; ++s) {
        if ((snap.get_sample(s, sig) ? 1 : 0) != original[s])
            ++changed;
    }
    // 反同义反复护栏: 若一趟滤波没改动任何数据, 下面的还原断言就是空话。
    // 不写成精确的 N/20: 采样 0 处的高电平是状态机的**初始基准**而非毛刺,
    // 它不会被抹平 (相差恰好 1), 把这种实现细节写进断言会让测试变脆。
    QVERIFY2(changed > 0,
             "the pass must actually modify data, otherwise the restore "
             "assertion below is vacuous");
    QVERIFY2(changed >= N / 20 - 2,
             "essentially every 1-sample glitch should have been flattened");

    // 撤销 → 必须逐位回到采集原始态
    snap.revert_all_edits();
    QVERIFY2(!snap.has_filter_edits(), "revert must consume the edit log");

    size_t mismatches = 0;
    size_t first_mismatch = 0;
    for (size_t s = 0; s < N; ++s) {
        if ((snap.get_sample(s, sig) ? 1 : 0) != original[s]) {
            if (mismatches == 0)
                first_mismatch = s;
            ++mismatches;
        }
    }
    QVERIFY2(mismatches == 0,
             qPrintable(QString("revert_all_edits must restore all %1 samples "
                                "bit-exactly (first mismatch at %2, total %3)")
                            .arg((qulonglong)N)
                            .arg((qulonglong)first_mismatch)
                            .arg((qulonglong)mismatches)));

    // 撤销是幂等的, 且还原后仍可正常渲染级搜索 (mipmap 已被重建)
    snap.revert_all_edits();
    uint64_t idx = 0;
    const bool has_edge = snap.get_nxt_edge(idx, snap.get_sample(0, sig), N - 1, 0, sig);
    Q_UNUSED(has_edge);

    // 还原后可再次滤波 (证明编辑日志被正确复用, 而非一次性)
    snap.apply_glitch_filter(sig, 2, nullptr, GlitchFilterMode::Both);
    QVERIFY(snap.has_filter_edits());
    snap.revert_all_edits();
    for (size_t s = 0; s < N; s += 997)
        QCOMPARE(snap.get_sample(s, sig) ? 1 : 0, (int)original[s]);
}

void TestLogicSnapshotRaw::test_batch_callback_publishes_each_committed_batch()
{
    // 渐进刷新契约: 每提交一批就地编辑, 数据层必须对外通知一次"可见数据变了"。
    //
    // 界面侧完全依赖这条通知把信号 pixmap 置脏。整趟滤波期间采集已停止(没有
    // 数据包驱动 Viewport::feed_in_*)、解码被暂停、DataUpdated 只在整趟结束时
    // 广播一次 —— 缺少每批通知时, 渲染会一直 blit 缓存里那份"滤波前"的
    // pixmap, 表现为"波形只有滚动视图才刷新"(滚动改变 scale/offset 才强制重建
    // pixmap)。这正是现场日志里"整趟滤波期间 GUI 侧一条活动都没有"的根因。
    //
    // 数据规模与 test_revert_all_edits_restores_capture_samples 同构:
    // N/20 = 100000 个毛刺 > apply_batch 的 65536 阈值, 必然跨多个 batch。
    const size_t N = 2000000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch != 0) return false;
        return (s % 20) == 0;      // 1-sample 窄高脉冲
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    uint64_t calls = 0;
    snap.apply_glitch_filter(sig, 2, nullptr, GlitchFilterMode::Both, nullptr,
                             [&calls]() {
                                 // 注意: 回调在独占写事务内被调用, 里面**不能**
                                 // 读快照 —— EditReadGuard 在同一线程上会自死锁
                                 // (std::shared_mutex 不可重入)。
                                 ++calls;
                             });

    QVERIFY2(calls >= 2,
             qPrintable(QString("a multi-batch pass must publish once per "
                                "committed batch (got %1) — otherwise the GUI "
                                "shows the pre-filter pixmap for the whole pass")
                            .arg((qulonglong)calls)));
    QVERIFY(snap.has_filter_edits());
    snap.revert_all_edits();

    // 单批(小数据)也必须通知一次: 扫尾那次 apply_batch() 同样是一次提交。
    // 用与 test_glitch_filter_removes_narrow_pulses 相同的构造, 保证确有改动。
    const size_t M = 100000;
    Fixture fx2(2, M);
    fx2.data = build_interleaved(M, [](size_t s, int ch) {
        if (ch != 0) return false;
        if (s >= 1000 && s < 1005) return true;    // 窄脉冲 len=5
        if (s >= 2000 && s < 2002) return true;    // 窄脉冲 len=2
        if (s >= 10000 && s < 12000) return true;  // 宽脉冲 len=2000 (保留)
        return false;
    });
    LogicSnapshot snap2;
    fx2.feed(snap2, M);
    uint64_t one_batch = 0;
    snap2.apply_glitch_filter(0, 20, nullptr, GlitchFilterMode::Both, nullptr,
                              [&one_batch]() { ++one_batch; });
    QCOMPARE(one_batch, Q_UINT64_C(1));
    snap2.revert_all_edits();

    // 没有提交任何批次就不得通知: threshold==0 立即返回, 不应触发界面重绘。
    uint64_t noop = 0;
    snap2.apply_glitch_filter(0, 0, nullptr, GlitchFilterMode::Both, nullptr,
                              [&noop]() { ++noop; });
    QCOMPARE(noop, Q_UINT64_C(0));
}

void TestLogicSnapshotRaw::test_revert_publishes_progress()
{
    // 撤销(点"清除滤波"/Ctrl+Z)现在整个跑在滤波 worker 线程上, 因为大文件上
    // 它要重放 ~一份通道数据并重建每个被触及块的 mipmap, 在 GUI 线程上做就是
    // "点一下卡几秒"。撤销期间界面唯一的进展依据就是这条进度通知。
    //
    // 契约: 只要撤销真的做了事, 就必须至少发一次通知; 没有编辑时是空操作,
    // 不能发通知(否则会白白触发一次重绘)。
    const size_t N = 2000000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch != 0) return false;
        return (s % 20) == 0;      // 1-sample 窄高脉冲
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    snap.apply_glitch_filter(sig, 2, nullptr, GlitchFilterMode::Both);
    QVERIFY(snap.has_filter_edits());

    uint64_t calls = 0;
    snap.revert_all_edits([&calls]() {
        // 与 batch_callback 同样的约束: 通知在独占写事务内发出, 回调里读快照
        // 会在同一线程上取 EditReadGuard 而自死锁。
        ++calls;
    });
    QVERIFY2(calls >= 1,
             "an undo that performs work must publish at least one progress "
             "notice, otherwise the GUI shows a frozen window for its duration");
    QVERIFY2(!snap.has_filter_edits(), "revert must consume the edit log");

    // 空操作(已无可撤销的编辑)不应发通知。
    uint64_t noop = 0;
    snap.revert_all_edits([&noop]() { ++noop; });
    QCOMPARE(noop, Q_UINT64_C(0));
}

void TestLogicSnapshotRaw::test_revert_all_edits_invert_round_trip()
{
    // 反相靠 XOR 自逆撤销, 由 _inverted_orders 记住"当前仍是反相态"。
    // 这里覆盖一次反相+撤销的往返, 以及"反相后再撤销"不会二次翻转
    // (即撤销是幂等的, 不会把已还原的数据又反相回去)。
    const size_t N = 200000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        return ch == 0 ? (s % 100) < 40 : false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    std::vector<uint8_t> original(N);
    for (size_t s = 0; s < N; ++s)
        original[s] = snap.get_sample(s, sig) ? 1 : 0;

    snap.invert_channel(sig);

    size_t inverted = 0;
    for (size_t s = 0; s < N; ++s) {
        if ((snap.get_sample(s, sig) ? 1 : 0) == original[s])
            ++inverted;
    }
    QVERIFY2(inverted == 0, "invert_channel must flip every sample");

    snap.revert_all_edits();
    QVERIFY(!snap.has_filter_edits());

    for (size_t s = 0; s < N; ++s)
        QCOMPARE(snap.get_sample(s, sig) ? 1 : 0, (int)original[s]);

    // 幂等: 再撤销一次不得把数据翻回去
    snap.revert_all_edits();
    for (size_t s = 0; s < N; s += 7)
        QCOMPARE(snap.get_sample(s, sig) ? 1 : 0, (int)original[s]);
}

void TestLogicSnapshotRaw::test_dense_glitch_train_merges_edit_records()
{
    // 现场 bug 的回归: "大文件滤完波之后没有更新"。编辑日志原先按"每个写入段
    // 一条记录"记账，于是毛刺挨得很近的信号会退化成"每个毛刺一条记录"；而预算
    // 用的是**记录条数**上限，2.5 G 采样、每 10 采样一个 5 采样窄脉冲的采集在
    // 8,060,928 条记录处撞上限，而有效载荷只有 11.6 MB（记录自身开销约 900 MB）
    // —— 整趟中止并回滚，用户看到的就是"大文件没变、小文件正常"。
    //
    // 这里不追求规模（复现 800 万毛刺需要 8000 万采样），而是直接断言**机制**：
    // 相邻写入段必须合并，因此记录数随"被触及的块数"增长，而不是随毛刺数增长。
    const size_t N = 4 * 1024 * 1024;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch != 0) return false;
        return (s % 10) >= 5;      // 每 10 采样一个 5 采样窄脉冲
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    std::vector<uint8_t> original(N);
    for (size_t s = 0; s < N; ++s)
        original[s] = snap.get_sample(s, sig) ? 1 : 0;

    snap.apply_glitch_filter(sig, 8, nullptr, GlitchFilterMode::Both);

    QVERIFY2(!snap.edit_log_overflowed(),
             "a dense but in-budget pass must not abort");
    QVERIFY(snap.has_filter_edits());

    const size_t records = snap._glitch_filter->_edits.size();
    QVERIFY2(records < 200,
             qPrintable(QString("nearby write-runs must merge into a few records "
                                "per block; got %1 for a %2-sample dense train "
                                "(one record per glitch would be ~%3)")
                            .arg((qulonglong)records)
                            .arg((qulonglong)N)
                            .arg((qulonglong)(N / 10))));

    // 另一半 bug 是"静默"：这趟必须真的改了数据，且必须逐位可还原。
    size_t changed = 0;
    for (size_t s = 0; s < N; ++s)
        if ((snap.get_sample(s, sig) ? 1 : 0) != original[s]) ++changed;
    QVERIFY2(changed >= N / 2 - 16,
             "every narrow high pulse should have been flattened");

    qInfo("dense-train edit log: %zu record(s) for ~%zu glitches (merging must "
          "keep this in the tens, not one per glitch)",
          records, N / 10);

    snap.revert_all_edits();
    size_t mismatches = 0;
    size_t first_bad = SIZE_MAX;
    for (size_t s = 0; s < N; ++s) {
        if ((snap.get_sample(s, sig) ? 1 : 0) != original[s]) {
            if (first_bad == SIZE_MAX) first_bad = s;
            ++mismatches;
        }
    }
    // The revert must also re-establish BLOCK-LEVEL state, not just bytes:
    // flattening every pulse drives the block to a constant level, at which
    // point calc_mipmap() collapses it to the RLE representation and releases
    // the storage (able_free sessions). A revert that only wrote bytes back
    // through a now-null pointer silently did nothing — the assertion below
    // covers it, and the block must be non-null with its mipmap rebuilt.
    {
        const int o = snap.get_ch_order(sig);
        auto &rn = snap._ch_data[o][0];
        QVERIFY2(rn.lbp[0] != nullptr,
                 "a block released by the RLE collapse must be re-materialised "
                 "when the edit is reverted");
        QVERIFY2((rn.tog & 1ULL) != 0,
                 "the restored block has transitions, so its tog bit must be set "
                 "or readers will fall back to the constant representation");
    }

    QVERIFY2(mismatches == 0,
             qPrintable(QString("merged records must still revert bit-exactly; "
                                "%1 mismatches, first at sample %2 "
                                "(original=%3 after-revert=%4)")
                            .arg((qulonglong)mismatches)
                            .arg((qulonglong)(first_bad == SIZE_MAX ? 0 : first_bad))
                            .arg(first_bad == SIZE_MAX ? -1 : (int)original[first_bad])
                            .arg(first_bad == SIZE_MAX
                                     ? -1
                                     : (int)(snap.get_sample(first_bad, sig) ? 1 : 0))));
    qInfo("dense train: records=%zu glitches~=%zu", records, N / 10);
}

void TestLogicSnapshotRaw::test_edit_guard_cost_ratio()
{
    // 为什么需要这条测试: 编辑可见性最初用"无条件 shared_lock"实现, 在数据
    // 查询热路径上实测明显变慢; 改成 seqlock 快路径后无编辑批次时只剩两次
    // acquire 载入。跨会话计时不可比 (同一台机器不同时段能差 1.8x), 所以这里
    // 在**同一进程内交替**测两种写法, 各取多轮最小值 (最小值最能代表无干扰
    // 状态), 比较的是比值而非绝对时间。
    const size_t N = 4 * 1024 * 1024;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        return ch == 0 ? (s % 50) < 25 : false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);

    const int sig = 0;
    const int iters = 200000;
    double best_seqlock = 1e30;
    double best_lock = 1e30;

    for (int round = 0; round < 7; ++round) {
        {
            const auto t0 = std::chrono::steady_clock::now();
            uint64_t sink = 0;
            for (int i = 0; i < iters; ++i)
                sink += snap.get_sample((uint64_t)(i * 37) % N, sig) ? 1u : 0u;
            const auto t1 = std::chrono::steady_clock::now();
            Q_UNUSED(sink);
            best_seqlock = std::min(
                best_seqlock,
                std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
        }
        {
            const auto t0 = std::chrono::steady_clock::now();
            uint64_t sink = 0;
            for (int i = 0; i < iters; ++i) {
                std::shared_lock<std::shared_mutex> g(snap._edit_visibility);
                sink += snap.get_sample_self((uint64_t)(i * 37) % N, sig) ? 1u : 0u;
            }
            const auto t1 = std::chrono::steady_clock::now();
            Q_UNUSED(sink);
            best_lock = std::min(
                best_lock,
                std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
        }
    }

    qInfo("edit-visibility guard, best-of-7 rounds: "
          "seqlock=%.1f ns/call, shared_lock=%.1f ns/call, ratio=%.2fx",
          best_seqlock, best_lock, best_lock / best_seqlock);

    // 结构性要求 (不是性能门禁): 无编辑批次在跑时, 快路径只是两次原子读,
    // 不允许比"取/放一次共享锁"更贵。留 1.25x 余量以免负载抖动误判。
    QVERIFY2(best_seqlock <= best_lock * 1.25,
             qPrintable(QString("seqlock fast path (%1 ns) must not cost more "
                                "than the shared lock (%2 ns)")
                            .arg(best_seqlock, 0, 'f', 1)
                            .arg(best_lock, 0, 'f', 1)));
}

QTEST_GUILESS_MAIN(TestLogicSnapshotRaw)
#include "test_logic_snapshot_raw.moc"
