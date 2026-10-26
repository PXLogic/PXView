/*
 * test_logic_snapshot_cross_alignment.cpp — 跨格式逻辑分块对齐回归测试 (T5 / R3)
 *
 * 契约 (spec harden-review-findings T5 / R3):
 *   LogicSnapshot::append_cross_payload() 解析 LA_CROSS_DATA 时按
 *     chunk = 使能通道数(_channel_num) * block_bytes 字节分块，其中
 *     block_bytes 由 sr_datafeed_logic.block_bytes 给出（0 = 缺省 8）。
 *   block_bytes 是"每通道每拍多少字节"：250MHz×64bit 机型一拍 64 样本
 *   = 8 字节，125MHz×128bit 机型一拍 128 样本 = 16 字节。
 *   某 block b、通道 c 的第 j 个 8 字节 u64 位于
 *     buf[(b*N + c)*B + j*8 .. +8)，对应样本 (b*B*8 + j*64 .. +64)。
 *   本测试作为"跨格式契约"单元回归:
 *     - N 通道 (7 / 32) × block_bytes (8 / 16 / 32) 喂入对齐 cross payload
 *       → 采样解析完整、样本数正确;
 *     - 64 样本边界 / 余数(部分块) / 跨多个 block 连续性;
 *     - 不匹配 chunk 尺寸 / 空负载 → 不越界、不崩溃, 按既有语义截断;
 *     - 宽块(block_bytes > 8) 遇到非整块负载 → 显式丢弃而不是静默串通道。
 *
 * 依赖链同 test_logic_snapshot_raw (直接编译 raw 数据层源, 内部 stub xlog):
 *   snapshot / logicsnapshot / diskcache_writer / glitch_filter / mmap_allocator
 *   + leaf_block_pool (header-only) + libsigrok 头 + common (xlog.h)。
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

#include <libsigrok/libsigrok.h>   // sr_datafeed_logic / sr_channel / GSList / LA_CROSS_DATA

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

// ── 访问 private 方法 append_cross_payload / 私有字段 _ch_data ──
// 仅对本测试编译单元生效 (include 后立即 undef), 不污染其它代码。
#define private public
#define protected public
#include "pv/data/snapshot/logicsnapshot.h"
#undef private
#undef protected

using namespace pv::data;

namespace {

// cross 格式的分块粒度: 每通道 64 样本 = 8 字节。
constexpr size_t kSamplesPerGroup = 64;
constexpr size_t kBytesPerChannel = 8; // 64 samples / 8 samples-per-byte

// 构造可辨识的 8 字节字段值: 各字节互异且随 (c, g) 变化,
// 使块内/跨 group 均产生跳变 (tog 置位, 走 leaf 读取路径)。
//   byte j = ((c+1)*7 + (g*5) + (j*3)) % 251 + 1   (1..251, 非零)
static uint8_t field_byte(int c, uint64_t g, int j)
{
    const int v = ((c + 1) * 7 + (int)(g * 5) + j * 3) % 251 + 1;
    return (uint8_t)v;
}

static uint64_t make_word(int c, uint64_t g)
{
    uint64_t w = 0;
    for (int j = 0; j < 8; ++j)
        w |= (uint64_t)field_byte(c, g, j) << (j * 8);
    return w;
}

// 期望的样本位值: 样本 s 位于 group (s/64)、bit (s%64)。
static bool expected_bit(int c, uint64_t s)
{
    const uint64_t g = s / kSamplesPerGroup;
    const unsigned i = (unsigned)(s % kSamplesPerGroup);
    return (make_word(c, g) >> i) & 1u;
}

// 构造 chunk 对齐的 cross 负载:
//   words[(g*N + c)] = make_word(c, g), 每个 word 占 8 字节小序。
std::vector<uint8_t> build_cross(size_t N, size_t G)
{
    std::vector<uint8_t> d(N * G * kBytesPerChannel);
    for (size_t g = 0; g < G; ++g) {
        for (size_t c = 0; c < N; ++c) {
            const uint64_t w = make_word((int)c, (uint64_t)g);
            const size_t off = (g * N + c) * kBytesPerChannel;
            std::memcpy(&d[off], &w, kBytesPerChannel);
        }
    }
    return d;
}

// 构造任意块宽的 cross 负载（B = 每通道每块字节数，W = B/8 个 u64 槽）。
// 布局: [block b][ch c][slot j] → buf[(b*N + c)*B + j*8]。
// 第 (b*W + j) 个全局槽装 make_word(c, b*W + j)，与 build_cross 的
// 样本编号完全一致 —— 所以 expected_bit(c, s) 对任何块宽都成立：
// 每通道样本 s 永远落在全局槽 s/64 的第 s%64 位。
std::vector<uint8_t> build_cross_wide(size_t N, size_t blocks, size_t B)
{
    const size_t W = B / kBytesPerChannel;
    std::vector<uint8_t> d(N * blocks * B);
    for (size_t b = 0; b < blocks; ++b) {
        for (size_t c = 0; c < N; ++c) {
            for (size_t j = 0; j < W; ++j) {
                const uint64_t w = make_word((int)c, (uint64_t)(b * W + j));
                const size_t off = (b * N + c) * B + j * kBytesPerChannel;
                std::memcpy(&d[off], &w, kBytesPerChannel);
            }
        }
    }
    return d;
}

// 构造 N 个使能 logic 通道的 GSList (通道 index == 列表内位置 0..N-1)。
struct CrossFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    size_t N;          // 使能通道数 (_channel_num)
    uint64_t total;    // each-channel sample count 目标

    explicit CrossFixture(size_t ch_count, uint64_t total_samples)
        : chs(ch_count), nodes(ch_count), N(ch_count), total(total_samples)
    {
        for (size_t i = 0; i < ch_count; ++i) {
            chs[i].index = (int)i;
            chs[i].type = SR_CHANNEL_LOGIC;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < ch_count) ? &nodes[i + 1] : nullptr;
        }
    }

    // 用 first_payload 建立 _channel_num / _ch_data / mmap 分配器。
    // (cross 路径不经过 first_payload 的数据拷贝, 只依赖它初始化骨架。)
    void setup(LogicSnapshot &snap)
    {
        std::vector<uint8_t> dummy(1, 0);
        sr_datafeed_logic l{};
        l.length = dummy.size();
        l.unitsize = 1;
        l.format = LA_SPLIT_DATA;
        l.data = dummy.data();
        snap.first_payload(l, total, &nodes[0], true);
    }
};

} // anonymous namespace

class TestLogicSnapshotCrossAlignment : public QObject
{
    Q_OBJECT

private slots:
    // 对齐路径 (N=7): 多 group 连续 + 64 样本边界 + 首位字节正确
    void test_cross_aligned_7ch_multi_group();
    // 不同通道数 (N=32): 分块尺寸变化
    void test_cross_aligned_32ch();
    // 宽块 (16 字节/通道/拍, 125MHz×128bit): N=7
    void test_cross_wide_block_16B_7ch();
    // 宽块 16B + 32 通道
    void test_cross_wide_block_16B_32ch();
    // 宽块 32B (W=4), 非 2 的幂通道数
    void test_cross_wide_block_32B_7ch();
    // 宽块负载非整块 → 显式丢弃, 不得静默串通道
    void test_cross_wide_misaligned_dropped();
    // block_bytes 是栈垃圾 (非法值) → 按缺省 8 解析
    void test_cross_block_bytes_garbage_falls_back_to_8();
    // 走真实链路 (append_payload → 异步 writer → append_cross_payload):
    // block_bytes 必须随队列传递, 否则 worker 侧会拿到栈垃圾
    void test_cross_wide_via_async_writer();
    // 负载小于一个 chunk / 空负载: 直接忽略, 样本数 0
    void test_cross_payload_short_ignored();
    // 负载不匹配 chunk 尺寸 (长度非 N*8 整数倍): 不越界不崩溃, 按语义截断
    void test_cross_payload_misaligned_nocrash();
};

void TestLogicSnapshotCrossAlignment::test_cross_aligned_7ch_multi_group()
{
    const size_t N = 7;
    const size_t G = 4;                 // 4 组 × 64 = 256 样本/通道 (>64, 跨 group)
    const uint64_t total = G * kSamplesPerGroup;

    CrossFixture fx(N, total);
    std::vector<uint8_t> data = build_cross(N, G);

    LogicSnapshot snap;
    fx.setup(snap);

    sr_datafeed_logic l{};
    l.length = data.size();            // G * N * 8 = 224 字节
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    l.data = data.data();
    snap.append_cross_payload(l);
    snap.capture_ended();

    // 采样数正确 (每通道 = total)
    QCOMPARE(snap.committed_sample_count(), total);

    // 每个通道、每个样本的取值都与按 N*8 分块后的期望一致 (含跨 group 连续性)
    for (size_t c = 0; c < N; ++c) {
        for (uint64_t s = 0; s < total; ++s) {
            const bool got = snap.get_sample(s, (int)c);
            const bool exp = expected_bit((int)c, s);
            if (got != exp) {
                QFAIL(QString("cross sample mismatch ch=%1 s=%2 got=%3 exp=%4")
                          .arg(c).arg(s).arg((int)got).arg((int)exp).toUtf8().constData());
            }
        }
    }

    // 首位数据字节合约: 按 chunk 分块后, 通道 c group g 的 8 字节原样落入 leaf block
    // word[g]。即 buf 字节 (g*N+c)*8 起 8 字节 == leaf[..] word g。
    for (size_t c = 0; c < N; ++c) {
        for (size_t g = 0; g < G; ++g) {
            void *lbp = snap._ch_data[c].front().lbp[0];
            QVERIFY2(lbp != nullptr, "leaf block must be allocated (block has transitions)");
            const uint64_t stored =
                ((const uint64_t *)lbp)[g];
            QVERIFY2(stored == make_word((int)c, (uint64_t)g),
                     "cross chunk bytes not bit-copied into per-channel block");
        }
    }
}

void TestLogicSnapshotCrossAlignment::test_cross_aligned_32ch()
{
    const size_t N = 32;
    const size_t G = 2;                 // 2 组 × 64 = 128 样本/通道
    const uint64_t total = G * kSamplesPerGroup;

    CrossFixture fx(N, total);
    std::vector<uint8_t> data = build_cross(N, G);

    LogicSnapshot snap;
    fx.setup(snap);

    sr_datafeed_logic l{};
    l.length = data.size();            // 2 * 32 * 8 = 512 字节 (chunk = 256 字节)
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    l.data = data.data();
    snap.append_cross_payload(l);
    snap.capture_ended();

    QCOMPARE(snap.committed_sample_count(), total);

    for (size_t c = 0; c < N; ++c)
        for (uint64_t s = 0; s < total; ++s)
            QCOMPARE((int)snap.get_sample(s, (int)c),
                     (int)expected_bit((int)c, s));
}

// ── 宽块（block_bytes > 8）公共校验 ──
// 构造 N 通道 / blocks 个交织块 / B 字节每块的 cross 负载，直接喂
// append_cross_payload，逐样本比对并校验首块 word 落位。
static void check_wide_cross(size_t N, size_t blocks, size_t B)
{
    const size_t W = B / kBytesPerChannel;
    const uint64_t total = (uint64_t)blocks * W * kSamplesPerGroup;

    CrossFixture fx(N, total);
    std::vector<uint8_t> data = build_cross_wide(N, blocks, B);

    LogicSnapshot snap;
    fx.setup(snap);

    sr_datafeed_logic l{};
    l.length = data.size();            // blocks * N * B
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    l.block_bytes = (uint16_t)B;
    l.data = data.data();
    snap.append_cross_payload(l);
    snap.capture_ended();

    QCOMPARE(snap.committed_sample_count(), total);

    for (size_t c = 0; c < N; ++c) {
        for (uint64_t s = 0; s < total; ++s) {
            const bool got = snap.get_sample(s, (int)c);
            const bool exp = expected_bit((int)c, s);
            if (got != exp) {
                QFAIL(QString("wide cross mismatch N=%1 B=%2 ch=%3 s=%4 got=%5 exp=%6")
                          .arg(N).arg(B).arg(c).arg(s)
                          .arg((int)got).arg((int)exp)
                          .toUtf8().constData());
            }
        }
    }

    // 首块落位: 通道 c 第 b 块的第 j 槽 == leaf 块 word (b*W + j)
    for (size_t c = 0; c < N; ++c) {
        void *lbp = snap._ch_data[c].front().lbp[0];
        QVERIFY2(lbp != nullptr, "leaf block must be allocated");
        for (size_t b = 0; b < blocks; ++b) {
            for (size_t j = 0; j < W; ++j) {
                const uint64_t stored =
                    ((const uint64_t *)lbp)[b * W + j];
                QVERIFY2(stored == make_word((int)c, (uint64_t)(b * W + j)),
                         "wide cross block not bit-copied into per-channel block");
            }
        }
    }
}

void TestLogicSnapshotCrossAlignment::test_cross_wide_block_16B_7ch()
{
    // 125MHz×128bit: 每通道每拍 16 字节 = 128 样本
    check_wide_cross(7, 3, 16);
}

void TestLogicSnapshotCrossAlignment::test_cross_wide_block_16B_32ch()
{
    check_wide_cross(32, 2, 16);
}

void TestLogicSnapshotCrossAlignment::test_cross_wide_block_32B_7ch()
{
    // W = 4：块内 4 个连续 u64 槽，通道数非 2 的幂
    check_wide_cross(7, 2, 32);
}

void TestLogicSnapshotCrossAlignment::test_cross_wide_misaligned_dropped()
{
    // 宽块没有"跨包续块"：非整块负载必须被整条丢弃（而不是按 8 字节块
    // 解交织，那会静默把通道串位）。契约：committed 不前进、不崩溃。
    const size_t N = 7;
    const size_t B = 16;
    const uint64_t total = 2 * kSamplesPerGroup;

    CrossFixture fx(N, total);
    std::vector<uint8_t> data = build_cross_wide(N, 1, B);
    data.insert(data.end(), kBytesPerChannel, 0xAB);   // 112 + 8 = 120 字节

    LogicSnapshot snap;
    fx.setup(snap);

    sr_datafeed_logic l{};
    l.length = data.size();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    l.block_bytes = (uint16_t)B;
    l.data = data.data();
    snap.append_cross_payload(l);

    QCOMPARE(snap.committed_sample_count(), (uint64_t)0);
}

void TestLogicSnapshotCrossAlignment::test_cross_block_bytes_garbage_falls_back_to_8()
{
    // 驱动侧 struct sr_datafeed_logic 是栈变量，未赋值的 block_bytes 是垃圾
    // 而不是 0。非法值（这里取 5：既不是 8 的倍数，也小于一个 u64 槽）必须
    // 按缺省 8 解析，绝不能当成"半字节块"去移位。
    const size_t N = 7;
    const size_t G = 2;
    const uint64_t total = G * kSamplesPerGroup;

    CrossFixture fx(N, total);
    std::vector<uint8_t> data = build_cross(N, G);

    LogicSnapshot snap;
    fx.setup(snap);

    sr_datafeed_logic l{};
    l.length = data.size();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    l.block_bytes = 5;                 // 垃圾值
    l.data = data.data();
    snap.append_cross_payload(l);
    snap.capture_ended();

    QCOMPARE(snap.committed_sample_count(), total);
    for (size_t c = 0; c < N; ++c)
        for (uint64_t s = 0; s < total; ++s)
            QCOMPARE((int)snap.get_sample(s, (int)c),
                     (int)expected_bit((int)c, s));
}

void TestLogicSnapshotCrossAlignment::test_cross_wide_via_async_writer()
{
    // 真实链路: append_payload 只把字节 + format + block_bytes 塞进队列，
    // worker 线程再重新构造 sr_datafeed_logic 调 append_cross_payload。
    // 漏传 block_bytes 时 worker 侧拿到 0/垃圾 → 按 8 字节块解析 → 通道串位，
    // 本用例会逐样本失配。
    const size_t N = 32;
    const size_t B = 16;
    const size_t blocks = 2;
    const size_t W = B / kBytesPerChannel;
    const uint64_t total = (uint64_t)blocks * W * kSamplesPerGroup;

    CrossFixture fx(N, total);
    std::vector<uint8_t> data = build_cross_wide(N, blocks, B);

    LogicSnapshot snap;
    fx.setup(snap);

    sr_datafeed_logic l{};
    l.length = data.size();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    l.block_bytes = (uint16_t)B;
    l.data = data.data();
    snap.append_payload(l);            // 入队（异步）
    snap.capture_ended();              // 排空 + finalize

    QCOMPARE(snap.committed_sample_count(), total);
    for (size_t c = 0; c < N; ++c)
        for (uint64_t s = 0; s < total; ++s)
            QCOMPARE((int)snap.get_sample(s, (int)c),
                     (int)expected_bit((int)c, s));
}

void TestLogicSnapshotCrossAlignment::test_cross_payload_short_ignored()
{
    const size_t N = 7;
    const uint64_t total = 1 * kSamplesPerGroup;   // 与空负载场景区分

    CrossFixture fx(N, total);
    std::vector<uint8_t> full = build_cross(N, 1);
    LogicSnapshot snap;
    fx.setup(snap);

    // 空负载 (length == 0): 顶层早退, 无任何写入
    {
        sr_datafeed_logic l{};
        l.length = 0;
        l.unitsize = 1;
        l.format = LA_CROSS_DATA;
        l.data = full.data();
        snap.append_cross_payload(l);
        QCOMPARE(snap.committed_sample_count(), (uint64_t)0);
    }

    // 负载小于一个 chunk (chunk_size = N*8 = 56): 不跨 chunk 边界, 同样被忽略
    {
        sr_datafeed_logic l{};
        l.length = kBytesPerChannel;   // 8 字节 (< 56)
        l.unitsize = 1;
        l.format = LA_CROSS_DATA;
        l.data = full.data();
        snap.append_cross_payload(l);
        QCOMPARE(snap.committed_sample_count(), (uint64_t)0);
    }
}

void TestLogicSnapshotCrossAlignment::test_cross_payload_misaligned_nocrash()
{
    // 负载长度不是 N*8 的整数倍 (chunk_size + 8 字节)。append_cross_payload
    // 走原始逐通道 strided 回退路径: 读到 chunk 边界即停 / 按 Scale 截断,
    // 不得越界、不得崩溃, committed_sample_count 保持 64 的整数倍。
    const size_t N = 7;
    const size_t G = 1;
    const uint64_t total = 2 * kSamplesPerGroup;

    CrossFixture fx(N, total);
    std::vector<uint8_t> data = build_cross(N, G);
    // 追加 8 字节 → length = 64, 非 56 的整数倍
    data.insert(data.end(), kBytesPerChannel, 0xAB);

    LogicSnapshot snap;
    fx.setup(snap);

    sr_datafeed_logic l{};
    l.length = data.size();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    l.data = data.data();
    // 正常返回 (探测段内无越界/崩溃)。
    snap.append_cross_payload(l);

    // 契约: 回退路径捕获读到 chunk 边界即停, 不得越界; committed 只能按
    // Scale(64) 粒度推进并受 sample limit 约束, 但不能保证提交了完整 chunk
    // (多出的 8 字节使首 chunk 不完整, 回退路径可能在首个 block 内停手)。
    const uint64_t committed = snap.committed_sample_count();
    QVERIFY2(committed % kSamplesPerGroup == 0,
             "cross parser must only commit whole 64-sample chunks");
    QVERIFY2(committed <= total,
             "cross parser must not overrun the configured sample limit");
}

QTEST_GUILESS_MAIN(TestLogicSnapshotCrossAlignment)
#include "test_logic_snapshot_cross_alignment.moc"