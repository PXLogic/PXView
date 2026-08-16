/*
 * test_logic_snapshot_query.cpp — LogicSnapshot 查询原语对拍测试 (B5)
 *
 * 用 get_sample() 的暴力逐位扫描作为 ground truth, 对拍:
 *   - get_samples()     字节展开正确性 (返回字节逐位 == get_sample)
 *   - get_nxt_edge_self 下一跳变位置 (向右)
 *   - get_pre_edge_self 上一跳变位置 (向左)
 *   - get_display_edges 渲染边 sanity (width/togs 约束 + toggle 电平翻转)
 *
 * 纯数据层, 无 QWidget 依赖。依赖链同 test_logic_snapshot_raw。
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

#include <libsigrok/libsigrok.h>

#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#define private public
#define protected public
#include "pv/data/snapshot/logicsnapshot.h"
#undef private
#undef protected

using namespace pv::data;

namespace {

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
        logic.format = 0;
        logic.data = nullptr;
        (void)total_samples;
    }

    void feed(LogicSnapshot &snap, uint64_t total_sample_count)
    {
        sr_datafeed_logic l = logic;
        l.length = (uint64_t)data.size();
        l.data = data.data();
        l.unitsize = 1;
        snap.first_payload(l, total_sample_count, &nodes[0], true);
        snap.append_payload(l);
        snap.capture_ended();
    }
};

// 向右暴力找第一个跳变位置 (start 起, 电平 != start 处电平)
// 返回 true 并在 *out 给出位置; 无跳变返回 false.
bool brute_nxt(LogicSnapshot &snap, uint64_t start, uint64_t end, int sig,
               uint64_t *out)
{
    const bool v0 = snap.get_sample(start, sig);
    for (uint64_t s = start + 1; s <= end; ++s) {
        if (snap.get_sample(s, sig) != v0) {
            *out = s;
            return true;
        }
    }
    return false;
}

// 向左暴力找上一个跳变位置 (start 起向左, 电平 != start 处电平)
// get_pre_edge 的契约 = "当前 run 的起点" (measure()/edge 导航用):
// 若首个 (向左) 与 v0 不同的样本在 s, 则 run 起点 = s + 1.
bool brute_pre(LogicSnapshot &snap, uint64_t start, int sig, uint64_t *out)
{
    if (start == 0)
        return false;
    const bool v0 = snap.get_sample(start, sig);
    for (uint64_t s = start; s-- > 0;) {
        if (snap.get_sample(s, sig) != v0) {
            *out = s + 1;   // run 起点 (对称于 nxt 返回首个差异样本)
            return true;
        }
    }
    return false;
}

} // anonymous namespace

class TestLogicSnapshotQuery : public QObject
{
    Q_OBJECT

private slots:
    // get_samples 字节展开 == get_sample 逐位
    void test_get_samples_single_block();
    void test_get_samples_multi_block();
    void test_get_samples_constant_block();

    // get_nxt_edge_self 对拍暴力向右
    void test_nxt_edge_vs_brute();
    void test_nxt_edge_vs_brute_multi_block();

    // get_pre_edge_self 对拍暴力向左
    void test_pre_edge_vs_brute();

    // get_display_edges sanity
    void test_display_edges_sanity();
    void test_display_edges_toggle_flips();
};

void TestLogicSnapshotQuery::test_get_samples_single_block()
{
    const size_t N = 30000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 7) < 4;   // 伪随机交替
        return (s % 3) == 0;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;

    // 多个起点, 校验返回字节逐位与 get_sample 一致
    // NOTE: get_samples 返回的指针指向 leaf block 的字节 (start/8), 即 p[0]
    // 从样本 (start & ~7) 开始; 读样本 s 需用绝对字节/位索引
    // (byte=(s/8)-(start/8), bit=s%8), 不能直接用 (s-start) 的相对偏移
    // (start 非 8 对齐时会错位一个字节内偏移).
    for (uint64_t start : std::vector<uint64_t>{0, 1, 7, 1000, 29990}) {
        uint64_t end = N - 1;
        void *lbp = nullptr;
        const uint8_t *p = snap.get_samples(start, end, sig, &lbp);
        QVERIFY2(p != nullptr, "get_samples should return non-null");
        QVERIFY(end >= start);
        const uint64_t start_byte = start >> 3;
        for (uint64_t s = start; s <= end; ++s) {
            size_t byte = (size_t)((s >> 3) - start_byte);
            unsigned bit = (unsigned)(s & 7);
            bool got = (p[byte] >> bit) & 1u;
            bool exp = snap.get_sample(s, sig);
            if (got != exp) {
                QFAIL(QString("byte expansion mismatch at s=%1 got=%2 exp=%3")
                          .arg(s).arg((int)got).arg((int)exp).toUtf8().constData());
            }
        }
    }
}

void TestLogicSnapshotQuery::test_get_samples_multi_block()
{
    const size_t N = 200000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) {
            const size_t seg = (s / 4096) % 3;
            if (seg == 0) return (s % 5) < 3;
            if (seg == 1) return (s / 4096) % 2 == 1;
            return (s % 2) == 0;
        }
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;

    // 覆盖块内/块边界起点
    std::vector<uint64_t> starts{0, 65535, 65536, 65537, 131071, 131072, 199999};
    for (uint64_t start : starts) {
        uint64_t end = N - 1;
        void *lbp = nullptr;
        const uint8_t *p = snap.get_samples(start, end, sig, &lbp);
        QVERIFY2(p != nullptr, "get_samples should return non-null");
        QVERIFY(end >= start);
        const uint64_t start_byte = start >> 3;
        for (uint64_t s = start; s <= end; ++s) {
            size_t byte = (size_t)((s >> 3) - start_byte);
            unsigned bit = (unsigned)(s & 7);
            bool got = (p[byte] >> bit) & 1u;
            bool exp = snap.get_sample(s, sig);
            QCOMPARE((int)got, (int)exp);
        }
    }
}

void TestLogicSnapshotQuery::test_get_samples_constant_block()
{
    // 完整常量块被 calc_mipmap 释放 (lbp==nullptr): get_samples 须返回
    // 填满常量值的合成缓冲, 而非 nullptr。
    const uint64_t LB = LogicSnapshot::LeafBlockSamples;
    const size_t N = (size_t)(2 * LB + 65536);
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [LB](size_t s, int ch) {
        if (ch != 0) return false;
        if (s < LB) return s >= 1000 && s < 2000;  // 块0: 有脉冲
        if (s < 2 * LB) return true;               // 块1: 常量高 (被释放)
        return false;                              // 块2: 常量低
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;
    int order = snap.get_ch_order(sig);
    QVERIFY(order >= 0);
    auto &root = snap._ch_data[order][0];
    QVERIFY(root.lbp[1] == nullptr);   // 块1 已释放

    // 从块1 中部查询 → 合成缓冲全 1
    {
        uint64_t start = LB + 1000, end = N - 1;
        void *lbp = nullptr;
        const uint8_t *p = snap.get_samples(start, end, sig, &lbp);
        QVERIFY2(p != nullptr, "constant block should return synthetic buffer");
        for (uint64_t s = start; s <= std::min(end, 2 * LB - 1); ++s) {
            size_t byte = (size_t)((s - start) / 8);
            unsigned bit = (s - start) % 8;
            QCOMPARE((int)((p[byte] >> bit) & 1u), 1);   // 常量高
        }
    }
}

void TestLogicSnapshotQuery::test_nxt_edge_vs_brute()
{
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) {
            if (s < 20000) return (s % 2) == 0;
            if (s < 40000) return false;
            if (s < 80000) return (s % 1000) < 500;
            return true;
        }
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;
    const int order = snap.get_ch_order(sig);
    QVERIFY(order >= 0);

    for (uint64_t start = 0; start + 1 < N; start += 503) {
        uint64_t brute_pos = 0;
        const bool b_exp = brute_nxt(snap, start, N - 1, sig, &brute_pos);

        uint64_t idx = start;
        const bool f = snap.get_nxt_edge_self(idx, snap.get_sample(start, sig),
                                              N - 1, 0, sig);
        QCOMPARE((int)f, (int)b_exp);
        if (b_exp)
            QCOMPARE(idx, brute_pos);
    }
}

void TestLogicSnapshotQuery::test_nxt_edge_vs_brute_multi_block()
{
    const size_t N = 200000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) {
            const size_t seg = (s / 4096) % 3;
            if (seg == 0) return (s % 5) < 3;
            if (seg == 1) return (s / 4096) % 2 == 1;
            return false;
        }
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;

    std::vector<uint64_t> starts{0, 65530, 65536, 65540, 131071, 131076, 199900};
    for (uint64_t start : starts) {
        uint64_t brute_pos = 0;
        const bool b_exp = brute_nxt(snap, start, N - 1, sig, &brute_pos);
        uint64_t idx = start;
        const bool f = snap.get_nxt_edge_self(idx, snap.get_sample(start, sig),
                                              N - 1, 0, sig);
        QCOMPARE((int)f, (int)b_exp);
        if (b_exp)
            QCOMPARE(idx, brute_pos);
    }
}

void TestLogicSnapshotQuery::test_pre_edge_vs_brute()
{
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) {
            if (s < 20000) return (s % 2) == 0;
            if (s < 40000) return false;
            return (s % 1000) < 500;
        }
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;
    const int order = snap.get_ch_order(sig);
    QVERIFY(order >= 0);

    for (uint64_t start = 1; start < N; start += 499) {
        uint64_t brute_pos = 0;
        const bool b_exp = brute_pre(snap, start, sig, &brute_pos);

        uint64_t idx = start;
        const bool f = snap.get_pre_edge_self(idx, snap.get_sample(start, sig),
                                              1 /*min_length*/, sig);
        QCOMPARE((int)f, (int)b_exp);
        if (b_exp)
            QCOMPARE(idx, brute_pos);
    }
}

void TestLogicSnapshotQuery::test_display_edges_sanity()
{
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 4) < 2;   // 高频交替
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;

    std::vector<std::pair<bool, bool>> edges;
    std::vector<std::pair<uint16_t, bool>> togs;
    const uint16_t width = 200;
    const uint16_t max_togs = 50;
    const bool ret = snap.get_display_edges(edges, togs, 0, N - 1, width,
                                            max_togs, 0.0, 1.0, (uint16_t)sig);
    QVERIFY(ret);
    // 渲染契约 (logicsignal.cpp assert _cur_pulses.size() >= width): edges 填满
    // width 像素, 边界处 has_edge push 可至 width+1 (记录第 width 像素的跳变)。
    QVERIFY(edges.size() >= width);
    QVERIFY(edges.size() <= (size_t)width + 1);
    QVERIFY(togs.size() <= max_togs);
    // togs 的 edges 索引必须落在合法范围
    for (const auto &t : togs)
        QVERIFY(t.first < (uint16_t)edges.size());
}

void TestLogicSnapshotQuery::test_display_edges_toggle_flips()
{
    // 每个 toggle 记录电平: 相邻 toggle 的 last_sample 必须翻转
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 100) < 50;   // 50/100 占空比方波
        return false;
    });

    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;

    std::vector<std::pair<bool, bool>> edges;
    std::vector<std::pair<uint16_t, bool>> togs;
    const bool ret = snap.get_display_edges(edges, togs, 0, N - 1, 400,
                                            100, 0.0, 1.0, (uint16_t)sig);
    QVERIFY(ret);
    // 方波: 相邻 toggle 电平必须翻转 (至少 2 个 toggle 时)
    for (size_t i = 1; i < togs.size(); ++i)
        QVERIFY2(togs[i].second != togs[i - 1].second,
                 "adjacent toggles must flip level");
}

QTEST_GUILESS_MAIN(TestLogicSnapshotQuery)
#include "test_logic_snapshot_query.moc"