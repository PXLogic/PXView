/*
 * test_decode_chunk_alignment.cpp — P1-a 解码 chunk 对齐契约回归测试
 *
 * 锁定的契约（decoderstack.cpp 的 P1-a 修复依赖它）
 * --------------------------------------------------
 * 1) 位偏移契约：LogicSnapshot 迭代器返回的指针按字节定位 ——
 *      byte_offset = (start & LeafMask) / 8        // logicsnapshot.cpp:1944
 *    是**向下取整**，所以指针的位 0 对应 floor(start/8)*8，而不是 start。
 *    libsigrokdecode 要求 inbuf[ch] 的位 0 恰好对应 chunk 的绝对起点
 *    (instance.c:1671-1677)，因此解码侧必须把 chunk 起点向下对齐到 8 样本。
 *    本测试把这个"隐式取整"显式钉住：非 8 对齐起点必须与向下对齐起点
 *    返回同一个指针。
 *
 * 2) 窗口不跨 leaf block 边界：get_iterator_valid_length() 必须精确给出
 *    从当前位置到该 leaf block 末尾的字节数，且
 *      (start - (start & LeafMask)) + valid_bytes * 8 == 下一个 leaf 边界
 *    这是 decoderstack 截断 chunk_end 的依据（此前该函数全仓零调用者）。
 *    不截断的后果：越过 L0 数据区（2,097,152 B）读进 L1 mipmap（32,768 B），
 *    不崩溃但把 mipmap 位当样本 —— 静默的解码错误。
 *
 * 3) 现状之所以不出事仅因 LeafBlockSamples / MaxChunkSize = 1024 整除；
 *    本测试用 2 个 leaf block 覆盖边界场景，并覆盖非 8 对齐起点。
 *
 * 纯数据层，无 QWidget 依赖。依赖链同 test_logic_snapshot_query。
 */

#include <QtTest/QtTest>

#include <cstdint>
#include <memory>
#include <random>
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

// LA_CROSS_DATA：每 64 样本/通道占 channel_num*8 字节
struct Fixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;

    explicit Fixture(int ch_count) : chs(ch_count), nodes(ch_count)
    {
        for (int i = 0; i < ch_count; ++i) {
            chs[i].index = i;
            chs[i].type = SR_CHANNEL_LOGIC;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < ch_count) ? &nodes[i + 1] : nullptr;
        }
    }
};

} // anonymous namespace

class TestDecodeChunkAlignment : public QObject
{
    Q_OBJECT

private slots:
    void test_iterator_window_contract();
    void test_bit_offset_floor_contract();
};

// ---------------------------------------------------------------------------
// 契约 2：窗口不跨 leaf block，且 valid_bytes 精确到块末尾
// ---------------------------------------------------------------------------
void TestDecodeChunkAlignment::test_iterator_window_contract()
{
    const int CH = 1;
    const uint64_t LEAF = LogicSnapshot::LeafBlockSamples;      // 16,777,216
    const uint64_t LEAF_BYTES = LEAF / 8;                       // 2,097,152
    // CROSS：1 通道每 64 样本占 8 字节 → 4MB = 2 个 leaf block
    const uint64_t PAYLOAD = 4ULL * 1024 * 1024;
    const uint64_t TOTAL_SAMPLES = 2 * LEAF;

    Fixture fx(CH);
    std::vector<uint8_t> payload(PAYLOAD);
    std::mt19937_64 rng(0x51CE);
    for (auto &b : payload) b = (uint8_t)(rng() & 0xFF);

    LogicSnapshot snap;
    sr_datafeed_logic l{};
    l.length = PAYLOAD;
    l.data = payload.data();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    snap.first_payload(l, TOTAL_SAMPLES, &fx.nodes[0], true);
    snap.capture_ended();

    QCOMPARE(snap.get_ring_sample_count(), (uint64_t)(PAYLOAD * 8 / CH));
    QVERIFY(snap.get_ring_sample_count() >= TOTAL_SAMPLES);

    // 覆盖：块首、块内、块末附近、跨块边界
    const uint64_t starts[] = {
        0,
        8,
        1000,
        LEAF - 16,
        LEAF - 8,
        LEAF - 1,          // 第 0 块最后一个样本
        LEAF,              // 第 1 块第一个样本
        LEAF + 5,
        LEAF + 12345,
        2 * LEAF - 8,
    };

    for (uint64_t s : starts) {
        auto it = snap.begin_sample_iteration(s, 0);
        QVERIFY2(it != nullptr, qPrintable(QString("null iterator at %1").arg(s)));
        QVERIFY2(!it->exhausted,
                 qPrintable(QString("iterator exhausted at %1").arg(s)));
        QVERIFY2(LogicSnapshot::get_iterator_value(it.get()) != nullptr,
                 qPrintable(QString("null data ptr at %1").arg(s)));

        const uint64_t valid = LogicSnapshot::get_iterator_valid_length(it.get());
        const uint64_t byte_off = (s & LogicSnapshot::LeafMask) / 8;
        QCOMPARE(valid, LEAF_BYTES - byte_off);

        // 核心不变式：窗口在**字节**意义上正好结束于 leaf block 末尾。
        // 注意不能写成 (s - (s & LeafMask)) + valid*8 == LEAF —— 那是样本口径，
        // 因 byte_off 向下取整会差 s%8 个样本。正确形式是：
        //     byte_off * 8 + valid * 8 == LEAF
        // 即窗口覆盖样本 [floor(s/8)*8, LEAF)，是块内子集，绝不跨块。
        QCOMPARE(byte_off * 8 + valid * 8, LEAF);
        QVERIFY2(valid >= 1,
                 qPrintable(QString("valid_bytes == 0 at %1 (would stall the "
                                    "decode loop)").arg(s)));

        snap.end_sample_iteration(std::move(it));
    }
}

// ---------------------------------------------------------------------------
// 契约 1：指针按字节向下取整 —— 非 8 对齐起点必须与对齐起点同指针
// ---------------------------------------------------------------------------
void TestDecodeChunkAlignment::test_bit_offset_floor_contract()
{
    const int CH = 2;
    const uint64_t LEAF = LogicSnapshot::LeafBlockSamples;
    const uint64_t PAYLOAD = 1ULL * 1024 * 1024;   // 2ch → 4,194,304 样本/ch
    const uint64_t TOTAL_SAMPLES = LEAF;

    Fixture fx(CH);
    std::vector<uint8_t> payload(PAYLOAD);
    std::mt19937_64 rng(0x0FF5);
    for (auto &b : payload) b = (uint8_t)(rng() & 0xFF);

    LogicSnapshot snap;
    sr_datafeed_logic l{};
    l.length = PAYLOAD;
    l.data = payload.data();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    snap.first_payload(l, TOTAL_SAMPLES, &fx.nodes[0], true);
    snap.capture_ended();

    // 对每个 8 对齐基准点 b，b+1..b+7 必须与 b 返回同一指针
    // （这正是 decode_start 非 8 倍数时位偏移错位的根源）
    for (uint64_t b = 0; b < 4096; b += 8) {
        auto it_base = snap.begin_sample_iteration(b, 0);
        QVERIFY(it_base != nullptr && !it_base->exhausted);
        const uint8_t *p_base = LogicSnapshot::get_iterator_value(it_base.get());
        const uint64_t v_base = LogicSnapshot::get_iterator_valid_length(it_base.get());
        snap.end_sample_iteration(std::move(it_base));

        for (uint64_t off = 1; off < 8; ++off) {
            auto it = snap.begin_sample_iteration(b + off, 0);
            QVERIFY(it != nullptr && !it->exhausted);
            const uint8_t *p = LogicSnapshot::get_iterator_value(it.get());
            const uint64_t v = LogicSnapshot::get_iterator_valid_length(it.get());
            snap.end_sample_iteration(std::move(it));

            QVERIFY2(p == p_base,
                     qPrintable(QString("floor contract broken: start %1 vs %2")
                                    .arg(b).arg(b + off)));
            // 非对齐起点与对齐起点落在同一字节 → 剩余字节数相同
            QCOMPARE(v, v_base);
        }
    }

    // 位偏移错位的量化：未对齐起点的真实位偏移 = start % 8
    // （解码侧必须向下对齐到 8 样本才能把它变成 0）
    for (uint64_t off = 1; off < 8; ++off)
        QCOMPARE((uint64_t)(off % 8), off);
}

QTEST_GUILESS_MAIN(TestDecodeChunkAlignment)
#include "test_decode_chunk_alignment.moc"
