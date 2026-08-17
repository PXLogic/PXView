/*
 * test_logic_snapshot_pattern_search.cpp — LogicSnapshot pattern 搜索子系统
 * 与显示边沿扫描转发路径测试 (B5 扩展)
 *
 * LogicSnapshot 最近拆出两个子系统 (unique_ptr 持有, 公开方法转发):
 *   - pattern_search()           -> _pattern_search (logicsnapshot_pattern_search.*)
 *   - get_display_edges()        -> _edge_scan
 *   - get_nxt_edge()/get_pre_edge() -> _edge_scan   (logicsnapshot_edge_scan.*)
 *
 * 本测试只走 LogicSnapshot 的公开方法, 既测子系统实现又测转发路径。
 *
 * pattern_search 语义 (读实现得出):
 *   std::map<uint16_t,QString> pattern 的 key = channel(sig) index, value 的
 *   首字符是匹配标志:
 *     '0' 电平低匹配; '1' 电平高匹配; 'X'/'x' 通配 (不参与计数);
 *     'R' 上升沿 / 'F' 下降沿 / 'C' 任意变化 (依赖 isNext 方向与前一样本).
 *   未出现在 pattern 中的通道不参与匹配。'X' 或引用无数据通道被忽略;
 *   pattern 为空 / 全部通道被忽略时返回 true 且不修改 index。
 *   isNext == true  : 正向 (index 递增), 命中时 index = 命中样本位置。
 *   isNext == false : 反向 (index 递减), 命中时 index = 命中样本位置 + 1
 *                     (实现里 "index++ // move to prev position")。
 *
 * 正确性: 用镜像实现的暴力扫描 (brute_pattern) 作为 ground truth 对拍, 覆盖
 * 命中/未命中/单通道/多通道/X 通配/方向/边沿 flag。edge 转发路径用与
 * test_logic_snapshot_query 一致的暴力对拍做精简冒烟, 并断言已知边沿位置与
 * get_display_edges 的确定输出。
 *
 * 纯数据层, 无 QWidget 依赖。依赖链同 test_logic_snapshot_query。
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
#include <random>
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
        if (bit_fn(s, 2)) v |= 0x04;
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

// ----------------------------------------------------------------------------
// pattern_search 实现镜像 ground truth。与 pattern_search / pattern_search_self
// 逐行对应 (含: 有限采集窗口钳制; empty / X / 无数据通道 -> true; edge flag
// 首值读取顺序与 index 钳制; 反向命中后 index+1)。
// ----------------------------------------------------------------------------
bool brute_pattern(LogicSnapshot &snap, int64_t start, int64_t end,
                   int64_t &index, const std::map<uint16_t, QString> &pattern,
                   bool isNext)
{
    // ---- pattern_search (finite fast path) 的窗口钳制 ----
    if (start < 0)
        start = 0;
    const int64_t sc = (int64_t)snap.committed_sample_count();
    if (end >= sc)
        end = sc - 1;

    if (pattern.empty())
        return true;

    std::vector<std::pair<uint16_t, char>> items;
    bool bEdgeFlag = false;
    for (const auto &kv : pattern) {
        // 取首字符 (空串 -> '\0', 与实现一致)
        const char flag = kv.second.toStdString().c_str()[0];
        if (flag != 'X' && flag != 'x' && snap.has_data(kv.first)) {
            items.push_back({kv.first, flag});
            if (flag == 'R' || flag == 'F' || flag == 'C')
                bEdgeFlag = true;
        }
    }
    if (items.empty())
        return true;

    const int64_t to = isNext ? end + 1 : start - 1;
    const int64_t step = isNext ? 1 : -1;
    std::vector<char> lst(items.size(), 0);

    int64_t i = index;
    if (bEdgeFlag) {
        for (size_t k = 0; k < items.size(); ++k)
            lst[k] = (char)snap.get_sample((uint64_t)i, items[k].first);
        i += step;
    }
    if (i < start)
        i = start;
    if (i > end)
        i = end;

    while (i != to) {
        int matched = 0;
        for (size_t k = 0; k < items.size(); ++k) {
            const char val = (char)snap.get_sample((uint64_t)i, items[k].first);
            const char fl = items[k].second;
            if (fl == '0')
                matched += !val;
            else if (fl == '1')
                matched += val;
            else if (fl == 'R')
                matched += isNext ? (lst[k] == 0 && val == 1)
                                  : (lst[k] == 1 && val == 0);
            else if (fl == 'F')
                matched += isNext ? (lst[k] == 1 && val == 0)
                                  : (lst[k] == 0 && val == 1);
            else if (fl == 'C')
                matched += (lst[k] != val);
            lst[k] = val;
        }
        if (matched == (int)items.size()) {
            if (!isNext)
                i++;
            index = i;
            return true;
        }
        i += step;
    }
    return false;
}

// 向右暴力找第一个跳变位置 (start 起, 电平 != start 处电平) —— 与 query 测试一致
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

// 向左暴力找上一个跳变位置 (get_pre_edge 契约 = "当前 run 的起点")
bool brute_pre(LogicSnapshot &snap, uint64_t start, int sig, uint64_t *out)
{
    if (start == 0)
        return false;
    const bool v0 = snap.get_sample(start, sig);
    for (uint64_t s = start; s-- > 0;) {
        if (snap.get_sample(s, sig) != v0) {
            *out = s + 1;   // run 起点
            return true;
        }
    }
    return false;
}

} // anonymous namespace

class TestLogicSnapshotPatternSearch : public QObject
{
    Q_OBJECT

private slots:
    // pattern_search: 单通道电平命中 (正向/反向/0/1)
    void test_pattern_search_single_channel_hit();
    // pattern_search: 多通道电平命中
    void test_pattern_search_multi_channel();
    // pattern_search: 未命中 / X 通配 / 空 pattern / 无数据通道
    void test_pattern_search_miss_and_wildcard();
    // pattern_search: 边沿 flag ('R'/'F'/'C') 确定性命中
    void test_pattern_search_edge_flags();
    // pattern_search: 随机对拍暴力镜像 (方向/窗口/pattern 组合)
    void test_pattern_search_vs_brute();
    // edge 转发路径冒烟: get_nxt_edge / get_pre_edge 已知边沿 + 对拍
    void test_edge_forwarding_smoke();
    // edge 转发路径冒烟: get_display_edges 确定输出
    void test_display_edges_forwarding_smoke();
};

void TestLogicSnapshotPatternSearch::test_pattern_search_single_channel_hit()
{
    // ch0: 高 iff s%4 >= 2  -> 电平串 ... 0 0 1 1 0 0 1 1 ...
    // ch1: 高 iff s%3 == 0  -> 1 0 0 1 0 0 ...
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 4) >= 2;
        return (s % 3) == 0;
    });
    LogicSnapshot snap;
    fx.feed(snap, N);

    // {0:"1"} 正向
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)2);   // s=2,3 为高
        idx = 5;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)6);   // s=5 低, 下一个高在 6
    }
    // {0:"0"} 正向: 首样本 s=0 即低
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("0");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)0);
    }
    // {0:"1"} 反向: 从 100 向左, 最近高 = 99 (99%4==3) -> 返回 99+1
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        int64_t idx = 100;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, false));
        QCOMPARE(idx, (int64_t)100);
        // 反向中途: 从 5 向左, 最近高 = 3 -> 返回 4
        idx = 5;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, false));
        QCOMPARE(idx, (int64_t)4);
    }
}

void TestLogicSnapshotPatternSearch::test_pattern_search_multi_channel()
{
    // ch0: 高 iff s%4 >= 2; ch1: 高 iff s 为偶数
    // 两者同时为高 <=> s%4 == 2 -> s = 2,6,10,14,...
    // ch0 高且 ch1 低 <=> s%4 == 3 -> s = 3,7,11,...
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 4) >= 2;
        return (s % 2) == 0;
    });
    LogicSnapshot snap;
    fx.feed(snap, N);

    // {0:"1", 1:"1"} 正向
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        p[1] = QStringLiteral("1");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)2);
        idx = 5;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)6);
    }
    // {0:"1", 1:"1"} 反向: 从 100 向左, 最近双高 = 98 -> 返回 99
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        p[1] = QStringLiteral("1");
        int64_t idx = 100;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, false));
        QCOMPARE(idx, (int64_t)99);
    }
    // {0:"1", 1:"0"} 正向/反向
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        p[1] = QStringLiteral("0");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)3);
        idx = 100;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, false));
        QCOMPARE(idx, (int64_t)100);   // 99: ch0 高 ch1 低 -> 命中, 返回 100
    }
}

void TestLogicSnapshotPatternSearch::test_pattern_search_miss_and_wildcard()
{
    const size_t N = 100000;
    Fixture fx(3, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 4) >= 2;
        if (ch == 1) return (s % 2) == 0;
        return false;   // ch2 恒低
    });
    LogicSnapshot snap;
    fx.feed(snap, N);

    // 窗口内未命中
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        int64_t idx = 1;
        QVERIFY(!snap.pattern_search(1, 1, idx, p, true));   // s=1 低
        QVERIFY(!snap.pattern_search(1, 1, idx, p, false));
    }
    // 恒低通道上搜高 -> 未命中
    {
        std::map<uint16_t, QString> p;
        p[2] = QStringLiteral("1");
        int64_t idx = 0;
        QVERIFY(!snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QVERIFY(!snap.pattern_search(0, (int64_t)N - 1, idx, p, false));
    }
    // 混合: 恒低通道参与后永不同时满足 -> 未命中
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        p[2] = QStringLiteral("1");
        int64_t idx = 0;
        QVERIFY(!snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
    }
    // X 通配: {0:"1", 1:"X"} 等价于 {0:"1"}
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        p[1] = QStringLiteral("X");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)2);
    }
    // 空 pattern / 全 X / 仅无数据通道 -> 返回 true 且 index 不变 (实现行为)
    {
        std::map<uint16_t, QString> p;
        int64_t idx = 42;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)42);

        p[1] = QStringLiteral("X");
        idx = 42;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)42);

        std::map<uint16_t, QString> q;
        q[40] = QStringLiteral("1");   // 仅 3 通道, 40 无数据 -> count==0 -> true
        idx = 7;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, q, true));
        QCOMPARE(idx, (int64_t)7);
    }
    // 窗口起点钳制: end 超过 committed 会被 clamp, 仍能命中
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("1");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N + 1000, idx, p, true));
        QCOMPARE(idx, (int64_t)2);
    }
}

void TestLogicSnapshotPatternSearch::test_pattern_search_edge_flags()
{
    // ch0 = 0 0 1 1 0 0 1 1 ... (s%4 >= 2 -> 高)
    const size_t N = 100000;
    Fixture fx(2, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 4) >= 2;
        return false;
    });
    LogicSnapshot snap;
    fx.feed(snap, N);

    // 上升沿 {0:"R"} 正向: 0->1 跳变在 s=2; 继续正向下一个在 s=6
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("R");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)2);
        idx = 5;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)6);
    }
    // 变化 {0:"C"} 正向: 首次变化在 s=2 (0->1)
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("C");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)2);
    }
    // 下降沿 {0:"F"} 正向: 1->0 在 s=4
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("F");
        int64_t idx = 0;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, true));
        QCOMPARE(idx, (int64_t)4);
    }
    // 反向 {0:"R"}: 从 100 向左遇到 1->0 (正向视角下降沿) 即 "R" 反向命中
    {
        std::map<uint16_t, QString> p;
        p[0] = QStringLiteral("R");
        int64_t idx = 100;
        QVERIFY(snap.pattern_search(0, (int64_t)N - 1, idx, p, false));
        // 100%4=0(低), 99%4=3(高), 98%4=2(高), 97%4=1(低):
        // 反向 lst 语义: 97 处 lst(98)=1 && val(97)=0 -> 命中, 返回 98
        QCOMPARE(idx, (int64_t)98);
    }
}

void TestLogicSnapshotPatternSearch::test_pattern_search_vs_brute()
{
    const size_t N = 120000;
    Fixture fx(3, N);
    fx.data = build_interleaved(N, [](size_t s, int ch) {
        if (ch == 0) return (s % 4) >= 2;
        if (ch == 1) return (s % 2) == 0;
        return (s % 7) < 4;   // ch2 伪随机
    });
    LogicSnapshot snap;
    fx.feed(snap, N);

    const char flags_pool[] = "01RFC";
    std::mt19937 rng(12345);
    for (int iter = 0; iter < 400; ++iter) {
        const int64_t start = (int64_t)(rng() % (N / 2));
        const int64_t end = start + (int64_t)(rng() % (N - (size_t)start));
        const bool isNext = (rng() & 1) != 0;
        const int64_t span = end - start + 1;
        const int64_t idx_in = start + (int64_t)(rng() % (uint64_t)span);

        std::map<uint16_t, QString> pattern;
        const int nch = 1 + (int)(rng() % 3);   // 1..3 个通道
        for (int c = 0; c < nch; ++c) {
            const uint16_t ch = (uint16_t)(rng() % 3);
            const char fl = flags_pool[rng() % 5];
            pattern[ch] = QString(QChar(fl));
        }

        int64_t idx_ref = idx_in;
        const bool ref = brute_pattern(snap, start, end, idx_ref, pattern, isNext);
        int64_t idx_got = idx_in;
        const bool got = snap.pattern_search(start, end, idx_got, pattern, isNext);

        if (got != ref || (got && idx_got != idx_ref)) {
            QFAIL(QString("pattern mismatch iter=%1 start=%2 end=%3 isNext=%4 "
                          "got=%5 idx_got=%6 ref=%7 idx_ref=%8")
                      .arg(iter).arg(start).arg(end).arg((int)isNext)
                      .arg((int)got).arg(idx_got).arg((int)ref).arg(idx_ref)
                      .toUtf8().constData());
        }
    }
}

void TestLogicSnapshotPatternSearch::test_edge_forwarding_smoke()
{
    // ch0 方波: 0-49 低, 50-99 高, 100-149 低, ...  (s%100 < 50 -> 低)
    const size_t N = 1000;
    Fixture fx(1, N);
    fx.data = build_interleaved(N, [](size_t s, int) {
        return (s % 100) < 50;
    });
    LogicSnapshot snap;
    fx.feed(snap, N);
    const int sig = 0;

    // get_nxt_edge: 已知边沿位置
    {
        uint64_t idx = 0;
        QVERIFY(snap.get_nxt_edge(idx, snap.get_sample(0, sig), N - 1, 1.0, sig));
        QCOMPARE(idx, (uint64_t)50);
        idx = 60;
        QVERIFY(snap.get_nxt_edge(idx, snap.get_sample(60, sig), N - 1, 1.0, sig));
        QCOMPARE(idx, (uint64_t)100);
        idx = N - 1;
        QVERIFY(!snap.get_nxt_edge(idx, snap.get_sample(N - 1, sig), N - 1, 1.0, sig));
    }
    // get_pre_edge: 已知 run 起点
    {
        uint64_t idx = 999;
        QVERIFY(snap.get_pre_edge(idx, snap.get_sample(999, sig), 1.0, sig));
        QCOMPARE(idx, (uint64_t)950);
        idx = 80;
        QVERIFY(snap.get_pre_edge(idx, snap.get_sample(80, sig), 1.0, sig));
        QCOMPARE(idx, (uint64_t)50);
        idx = 49;
        QVERIFY(!snap.get_pre_edge(idx, snap.get_sample(49, sig), 1.0, sig));
    }
    // 与暴力对拍 (转发路径一致性, 同 query 测试)
    for (uint64_t start = 0; start + 1 < N; start += 137) {
        uint64_t bp = 0;
        const bool b_exp = brute_nxt(snap, start, N - 1, sig, &bp);
        uint64_t idx = start;
        const bool f = snap.get_nxt_edge(idx, snap.get_sample(start, sig),
                                         N - 1, 1.0, sig);
        QCOMPARE((int)f, (int)b_exp);
        if (b_exp)
            QCOMPARE(idx, bp);
    }
    for (uint64_t start = 1; start < N; start += 113) {
        uint64_t bp = 0;
        const bool b_exp = brute_pre(snap, start, sig, &bp);
        uint64_t idx = start;
        const bool f = snap.get_pre_edge(idx, snap.get_sample(start, sig),
                                         1.0, sig);
        QCOMPARE((int)f, (int)b_exp);
        if (b_exp)
            QCOMPARE(idx, bp);
    }
}

void TestLogicSnapshotPatternSearch::test_display_edges_forwarding_smoke()
{
    // 单一脉冲: 0-199 低, 200-399 高, 之后全低
    const size_t N = 5000;
    Fixture fx(1, N);
    fx.data = build_interleaved(N, [](size_t s, int) {
        return s >= 200 && s < 400;
    });
    LogicSnapshot snap;
    fx.feed(snap, N);
    const uint16_t sig = 0;

    std::vector<std::pair<bool, bool>> edges;
    std::vector<std::pair<uint16_t, bool>> togs;
    const uint16_t width = 200;
    const uint16_t max_togs = 10;
    const bool ret = snap.get_display_edges(edges, togs, 0, N - 1, width,
                                            max_togs, 0.0, 1.0, sig);

    // 返回值 = start_sample = sample(0) = 低 -> false
    QCOMPARE((int)ret, 0);
    // 渲染契约: edges 填满 width, 边界跳变可 push 至 width+1
    QVERIFY(edges.size() >= width);
    QVERIFY(edges.size() <= (size_t)width + 1);
    QCOMPARE(edges.size(), (size_t)201);
    // togs: [(0,低), (200,高), (200,低)]  —— 确定输出
    QCOMPARE(togs.size(), (size_t)3);
    QCOMPARE((int)togs[0].first, 0);
    QCOMPARE((int)togs[0].second, 0);
    QCOMPARE((int)togs[1].first, 200);
    QCOMPARE((int)togs[1].second, 1);
    QCOMPARE((int)togs[2].first, 200);
    QCOMPARE((int)togs[2].second, 0);
    // edges 电平: 前 200 个为低, 第 200 个 (边界跳变) 为高
    for (size_t i = 0; i < 200; ++i) {
        QCOMPARE((int)edges[i].first, 0);
        QCOMPARE((int)edges[i].second, 0);
    }
    QCOMPARE((int)edges[200].first, 1);
    QCOMPARE((int)edges[200].second, 1);
    // 相邻 toggle 电平翻转不变式 (健壮性兜底)
    for (size_t i = 1; i < togs.size(); ++i)
        QVERIFY2(togs[i].second != togs[i - 1].second,
                 "adjacent toggles must flip level");
}

QTEST_GUILESS_MAIN(TestLogicSnapshotPatternSearch)
#include "test_logic_snapshot_pattern_search.moc"
