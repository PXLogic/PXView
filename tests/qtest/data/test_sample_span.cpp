/*
 * test_sample_span.cpp — P1-c 统一读取抽象（SampleSpan）契约回归测试
 *
 * 锁定的契约（见 pv/data/snapshot/sample_span.h）
 * ------------------------------------------------
 * 1) start_sample 是权威索引：logic 按字节向下取整，返回的 start_sample 可能
 *    比请求的 start 小 0..7。调用方必须按 span.start_sample 索引，否则位偏移
 *    整体错位。本测试显式钉住这个取整行为。
 * 2) contiguous_samples 是硬上界：logic 只保证单个 leaf block 内连续，
 *    dso/analog 保证到平面/块末尾。越过即读越界。
 * 3) 三种布局用同一结构表达：
 *      logic   bits_per_sample=1,  stride=1
 *      dso     bits_per_sample=8,  stride=1
 *      analog  bits_per_sample=32, stride=unit_bytes*channel_num
 * 4) 基类默认实现返回无效 span（既有子类不必立即实现）。
 * 5) 常量块（logic 专有）：lbp == nullptr 时旧 get_samples 返回 thread_local
 *    合成缓冲（全 0x00 / 0xFF）；span 改为 is_constant + constant_bit 显式表达，
 *    readable() 为真、valid() 为假（data == nullptr），取值走 bit_at()。
 *
 * 用 get_sample() / get_samples() 作 ground truth 对拍。
 * 纯数据层，无 QWidget 依赖。依赖链同 test_logic_snapshot_query。
 */

#include <QtTest/QtTest>

#include <cstdint>
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
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/snapshot/analogsnapshot.h"
#undef private
#undef protected

using namespace pv::data;

namespace {

// 基类默认实现：不 override span() 的最小 Snapshot 子类
class BareSnapshot : public Snapshot
{
public:
    BareSnapshot() : Snapshot(1, 0, 0) {}
    void clear() override {}
    void init() override {}
    bool has_data(int) override { return false; }
    int get_block_num() override { return 0; }
    uint64_t get_block_size(int) override { return 0; }
};

// ---- Logic: LA_CROSS_DATA，每 64 样本/通道占 ch*8 字节 ----
struct LogicFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    std::vector<uint8_t> payload;

    LogicFixture(int ch_count, uint64_t bytes, uint64_t seed)
        : chs(ch_count), nodes(ch_count)
    {
        for (int i = 0; i < ch_count; ++i) {
            chs[i].index = i;
            chs[i].type = SR_CHANNEL_LOGIC;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < ch_count) ? &nodes[i + 1] : nullptr;
        }
        const uint64_t chunk = (uint64_t)ch_count * 8;
        bytes -= bytes % chunk;
        payload.resize(bytes);
        std::mt19937_64 rng(seed);
        for (auto &b : payload) b = (uint8_t)(rng() & 0xFF);
    }

    // CROSS 布局下样本 s 在通道 ch 的电平（ground truth）
    bool bit_of(uint64_t s, int ch) const
    {
        const uint64_t chunk_bytes = (uint64_t)chs.size() * 8;
        const uint64_t byte = (s / 64) * chunk_bytes + (uint64_t)ch * 8 +
                              ((s % 64) / 8);
        return (payload[byte] >> ((s % 64) % 8)) & 1u;
    }
};

struct DsoFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    sr_datafeed_dso dso{};

    explicit DsoFixture(const std::vector<int> &idx) : chs(idx.size()), nodes(idx.size())
    {
        for (size_t i = 0; i < chs.size(); ++i) {
            chs[i].index = idx[i];
            chs[i].type = SR_CHANNEL_DSO;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < chs.size()) ? &nodes[i + 1] : nullptr;
        }
    }

    void feed(DsoSnapshot &snap, uint64_t total, const void *data, uint32_t n)
    {
        dso.data = const_cast<void *>(data);
        dso.num_samples = n;
        dso.trig_flag = 0;
        dso.trig_ch = 0;
        dso.en_ch_num = (uint8_t)chs.size();
        dso.sample_bits = 8;
        dso.trig_offset = 0;
        dso.packet_len = 0;
        dso.samplerate_tog = 0;
        snap.first_payload(dso, total, &nodes[0], /*instant=*/true, /*isFile=*/false);
        snap.capture_ended();
    }
};

struct AnalogFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    sr_analog_encoding encoding{};
    sr_analog_meaning meaning{};
    sr_analog_spec spec{};
    sr_datafeed_analog analog{};

    AnalogFixture(const std::vector<int> &idx, uint8_t unitsize, bool is_float)
        : chs(idx.size()), nodes(idx.size())
    {
        for (size_t i = 0; i < chs.size(); ++i) {
            chs[i].index = idx[i];
            chs[i].type = SR_CHANNEL_ANALOG;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < chs.size()) ? &nodes[i + 1] : nullptr;
        }
        encoding.unitsize = unitsize;
        encoding.is_float = is_float ? TRUE : FALSE;
        encoding.is_signed = FALSE;
        encoding.is_bigendian = FALSE;
        encoding.digits = 0;
        encoding.is_digits_decimal = FALSE;
        meaning.mq = SR_MQ_VOLTAGE;
        meaning.unit = SR_UNIT_VOLT;
        meaning.mqflags = static_cast<sr_mqflag>(0);
        meaning.channels = nullptr;
        analog.data = nullptr;
        analog.num_samples = 0;
        analog.encoding = &encoding;
        analog.meaning = &meaning;
        analog.spec = &spec;
    }

    void feed_all(AnalogSnapshot &snap, uint64_t total, const void *data, uint32_t n)
    {
        meaning.channels = &nodes[0];
        analog.data = const_cast<void *>(data);
        analog.num_samples = n;
        snap.first_payload(analog, total, &nodes[0]);
        snap.capture_ended();
    }
};

} // anonymous namespace

class TestSampleSpan : public QObject
{
    Q_OBJECT

private slots:
    void test_base_default_invalid();
    void test_logic_span_contract();
    void test_logic_constant_block();
    void test_dso_span_contract();
    void test_analog_span_contract();
    void test_analog_value_decode();
};

// ---------------------------------------------------------------------------
// 契约 4：基类默认返回无效 span
// ---------------------------------------------------------------------------
void TestSampleSpan::test_base_default_invalid()
{
    BareSnapshot b;
    const SampleSpan s = b.span(0, 0, 1);
    QVERIFY(!s.valid());
    QVERIFY(s.data == nullptr);
    QCOMPARE(s.contiguous_samples, (uint64_t)0);
    QCOMPARE(s.bytes(), (uint64_t)0);
}

// ---------------------------------------------------------------------------
// 契约 1 + 2：logic 位打包 + start_sample 向下取整 + leaf block 硬上界
// ---------------------------------------------------------------------------
void TestSampleSpan::test_logic_span_contract()
{
    const int CH = 2;
    const uint64_t LEAF = LogicSnapshot::LeafBlockSamples;
    const uint64_t LEAF_BYTES = LEAF / 8;

    LogicFixture fx(CH, 256 * 1024, 0x5A17);
    LogicSnapshot snap;
    sr_datafeed_logic l{};
    l.length = fx.payload.size();
    l.data = fx.payload.data();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    snap.first_payload(l, LEAF, &fx.nodes[0], true);
    snap.capture_ended();

    // ---- 8 对齐起点 ----
    {
        const SampleSpan s = snap.span(0, 0, 1000);
        QVERIFY(s.valid());
        QVERIFY(s.readable());
        QVERIFY(!s.is_constant);
        QCOMPARE(s.start_sample, (uint64_t)0);
        QCOMPARE(s.bits_per_sample, (uint8_t)1);
        QCOMPARE(s.unit_bytes, (uint32_t)1);
        QCOMPARE(s.is_bit_packed(), true);
        QCOMPARE(s.contiguous_samples, LEAF_BYTES * 8);
        QCOMPARE(s.bytes(), LEAF_BYTES);
        // 与 get_sample() 对拍
        for (uint64_t k = 0; k < 512; ++k)
            QCOMPARE(s.bit_at(k), snap.get_sample(k, 0));
    }

    // ---- 非 8 对齐起点：start_sample 必须向下取整 ----
    for (uint64_t off = 1; off < 8; ++off) {
        const SampleSpan s = snap.span(0, off, 100);
        QVERIFY2(s.valid(), qPrintable(QString("span invalid at off=%1").arg(off)));
        QCOMPARE(s.start_sample, (uint64_t)0);          // floor(off/8)*8 == 0
        QCOMPARE(s.contiguous_samples, LEAF_BYTES * 8);
        QCOMPARE(s.bit_at(off), snap.get_sample(off, 0));
    }

    // ---- 跨字节边界 ----
    {
        const SampleSpan s = snap.span(0, 13, 100);
        QVERIFY(s.valid());
        QCOMPARE(s.start_sample, (uint64_t)8);          // floor(13/8)*8 == 8
        QCOMPARE(s.byte_offset_of(13), (uint64_t)0);
        QCOMPARE(s.bit_mask_of(13), (uint8_t)(1u << 5));
        QCOMPARE(s.bit_at(13), snap.get_sample(13, 0));
        QCOMPARE(s.bit_at(20), snap.get_sample(20, 0));
    }

    // ---- 通道 1 必须读到自己的数据（order 映射正确）----
    {
        const SampleSpan s = snap.span(1, 64, 100);
        QVERIFY(s.valid());
        for (uint64_t k = 64; k < 64 + 256; ++k)
            QCOMPARE(s.bit_at(k), fx.bit_of(k, 1));
    }

    // ---- 越界起点返回无效 span ----
    {
        const SampleSpan s = snap.span(0, LEAF * 4, 10);
        QVERIFY(!s.valid());
    }
}

// ---------------------------------------------------------------------------
// 契约 5：logic 常量块 —— is_constant 显式表达，取代旧合成缓冲
//
// LogicSnapshot 在"该 leaf 内某通道无跳变"时会释放该 leaf block
// （lbp == nullptr），电平编码在 RootNode.first 的第 index1 位。
// 旧 get_samples() 用一个 thread_local 合成缓冲（全 0x00 / 0xFF）假装"有数据"；
// span 改为 is_constant + constant_bit：readable() 为真、valid() 为假、
// data == nullptr，取值一律走 bit_at()。
// ---------------------------------------------------------------------------
void TestSampleSpan::test_logic_constant_block()
{
    const int CH = 2;
    const uint64_t LEAF = LogicSnapshot::LeafBlockSamples;
    const uint64_t LEAF_BYTES = LEAF / 8;

    LogicFixture fx(CH, 256 * 1024, 0x1234);
    LogicSnapshot snap;
    sr_datafeed_logic l{};
    l.length = fx.payload.size();
    l.data = fx.payload.data();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;
    snap.first_payload(l, LEAF, &fx.nodes[0], true);
    snap.capture_ended();

    // ---- 常量 1：手动把通道 0 的 leaf 0 置为"已释放 + 电平 1" ----
    snap._ch_data[0][0].lbp[0] = nullptr;
    snap._ch_data[0][0].first |= (1ULL << 0);

    {
        const SampleSpan s = snap.span(0, 0, 1000);
        QVERIFY2(!s.valid(), "常量块没有真实数据指针 → valid() 必须为假");
        QVERIFY2(s.readable(), "但常量块是可读的 → readable() 必须为真");
        QVERIFY(s.is_constant);
        QVERIFY(s.constant_bit);
        QCOMPARE(s.data, nullptr);
        QCOMPARE(s.constant_fill_byte(), (uint8_t)0xFF);
        QCOMPARE(s.start_sample, (uint64_t)0);
        QCOMPARE(s.contiguous_samples, LEAF_BYTES * 8);

        // 全段任意位置都是 1（bit_at 内建常量分支，绝不碰 data）
        QCOMPARE(s.bit_at(0), true);
        QCOMPARE(s.bit_at(7), true);
        QCOMPARE(s.bit_at(4096), true);
        QCOMPARE(s.bit_at(s.contiguous_samples - 1), true);

        // 与旧 get_samples 的合成缓冲语义一致（全 0xFF）
        uint64_t end = 0;
        const uint8_t *legacy = snap.get_samples(0, end, 0);
        QVERIFY(legacy != nullptr);
        QCOMPARE(legacy[0], (uint8_t)0xFF);
    }

    // ---- 常量 0 ----
    snap._ch_data[0][0].first &= ~(1ULL << 0);
    {
        const SampleSpan s = snap.span(0, 0, 1000);
        QVERIFY(!s.valid());
        QVERIFY(s.readable());
        QVERIFY(s.is_constant);
        QVERIFY(!s.constant_bit);
        QCOMPARE(s.constant_fill_byte(), (uint8_t)0x00);
        QCOMPARE(s.bit_at(0), false);
        QCOMPARE(s.bit_at(12345), false);

        uint64_t end = 0;
        const uint8_t *legacy = snap.get_samples(0, end, 0);
        QVERIFY(legacy != nullptr);
        QCOMPARE(legacy[0], (uint8_t)0x00);
    }

    // ---- 通道 1 未受影响：仍是正常 span ----
    {
        const SampleSpan s = snap.span(1, 0, 1000);
        QVERIFY(s.valid());
        QVERIFY(!s.is_constant);
        QCOMPARE(s.bit_at(64), snap.get_sample(64, 1));
    }

    // ---- 非 8 对齐起点在常量块上也必须正确（start_sample 仍向下取整）----
    snap._ch_data[0][0].lbp[0] = nullptr;
    snap._ch_data[0][0].first |= (1ULL << 0);
    {
        const SampleSpan s = snap.span(0, 13, 100);
        QVERIFY(s.readable());
        QVERIFY(s.is_constant);
        QCOMPARE(s.start_sample, (uint64_t)8);
        QCOMPARE(s.bit_at(13), true);
    }
}

// ---------------------------------------------------------------------------
// 契约 3（DSO）：按通道平面，stride=1，contiguous 到平面末尾
// ---------------------------------------------------------------------------
void TestSampleSpan::test_dso_span_contract()
{
    const uint32_t N = 1000;
    std::vector<uint8_t> data(N);
    for (uint32_t i = 0; i < N; ++i) data[i] = (uint8_t)(i & 0xFF);

    DsoFixture fx({0});
    DsoSnapshot snap;
    fx.feed(snap, N, data.data(), N);

    const SampleSpan s = snap.span(0, 10, 5);
    QVERIFY(s.valid());
    QCOMPARE(s.start_sample, (uint64_t)10);      // DSO 无需取整
    QCOMPARE(s.unit_bytes, (uint32_t)1);
    QCOMPARE(s.stride, (uint32_t)1);
    QCOMPARE(s.bits_per_sample, (uint8_t)8);
    QCOMPARE(s.is_bit_packed(), false);

    const uint64_t expect_contig = snap.get_sample_count() - 10;
    QCOMPARE(s.contiguous_samples, expect_contig);
    QCOMPARE(s.bytes(), expect_contig);

    // 与旧 get_samples 对拍：同一指针
    const uint8_t *legacy = snap.get_samples(10, 10, 0);
    QVERIFY(legacy != nullptr);
    QCOMPARE(s.data, legacy);

    // 逐样本值一致
    for (uint64_t k = 0; k < 64; ++k)
        QCOMPARE(s.data[s.byte_offset_of(10 + k)], data[10 + k]);

    // 越界
    QVERIFY(!snap.span(0, N + 5, 1).valid());
}

// ---------------------------------------------------------------------------
// 契约 3（Analog）：交织存储，stride = unit_bytes * channel_num
// ---------------------------------------------------------------------------
void TestSampleSpan::test_analog_span_contract()
{
    const uint8_t UNITSIZE = 4;          // float
    const uint32_t N = 100;
    const int CH = 2;
    std::vector<uint8_t> data((size_t)N * UNITSIZE * CH);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i & 0xFF);

    AnalogFixture fx({0, 1}, UNITSIZE, /*is_float=*/true);
    AnalogSnapshot snap;
    fx.feed_all(snap, N, data.data(), N);

    const SampleSpan s = snap.span(0, 10, 5);
    QVERIFY(s.valid());
    QCOMPARE(s.start_sample, (uint64_t)10);
    QCOMPARE(s.unit_bytes, (uint32_t)UNITSIZE);
    QCOMPARE(s.stride, (uint32_t)(UNITSIZE * CH));   // 交织 → 隔一个通道
    QCOMPARE(s.bits_per_sample, (uint8_t)(UNITSIZE * 8));
    QCOMPARE(s.is_bit_packed(), false);

    const uint64_t expect_contig = snap.get_sample_count() - 10;
    QCOMPARE(s.contiguous_samples, expect_contig);

    // 与旧 get_samples 对拍：channel 0（order 0）时 data == 样本组基址
    const uint8_t *legacy = snap.get_samples(10);
    QVERIFY(legacy != nullptr);
    QCOMPARE(s.data, legacy);
    QCOMPARE(s.group_base, legacy);

    // B+C 收口：channel 1（order 1）时 data 必须落在样本组基址 + 1 * unit_bytes，
    // 而 group_base 与 channel 0 相同（整块基址与通道无关）。
    const SampleSpan s1 = snap.span(1, 10, 5);
    QVERIFY(s1.valid());
    QCOMPARE(s1.group_base, legacy);
    QCOMPARE(s1.data, legacy + (uint64_t)UNITSIZE);

    // 交织步长正确：byte_offset_of() 是**相对 data** 的偏移，
    // 而 data 已经指向样本 10，所以 k=0 时偏移为 0。
    const uint64_t base_abs = 10 * (uint64_t)(UNITSIZE * CH);
    for (uint64_t k = 0; k < 16; ++k) {
        const uint64_t rel = s.byte_offset_of(10 + k);
        QCOMPARE(rel, k * (uint64_t)(UNITSIZE * CH));
        const uint64_t abs = base_abs + rel;
        for (uint32_t b = 0; b < UNITSIZE; ++b)
            QCOMPARE(s.data[rel + b], data[abs + b]);
    }

    QVERIFY(!snap.span(0, N + 1, 1).valid());
}

// ---------------------------------------------------------------------------
// 契约 6：交织布局的样本解码 —— SampleSpan::analog_value_at()
//
// 取值域与 AnalogSignal 渲染、SessionService::export_binary /
// get_analog_samples 一致：float 编码 -> 电压(V)；整数编码 -> 原始整数计数。
//
// 这个用例是 get_analog_samples() 交织索引缺陷的回归钉：
// 旧实现用 pitch = get_scale_factor()（= EnvelopeScaleFactor = 16，是**包络降采样
// 因子**，不是交织步长）+ 未映射的 channel_index + /255.0f，会读到别的通道的字节。
// 正确步长必须是 unit_bytes * channel_num（= span.stride）。
//
// B+C 收口后：通道内偏移由 AnalogSnapshot::span() 自己加进 data，
// analog_value_at() 不再有 ch_off 参数 —— 调用方无法传错。所以本用例改为断言
//   data == group_base + get_ch_order(channel) * unit_bytes
// 并验证解码值。
// ---------------------------------------------------------------------------
void TestSampleSpan::test_analog_value_decode()
{
    // ---- float 编码（unit_bytes=4，3 通道）----
    {
        const uint8_t UNITSIZE = 4;
        const uint32_t N = 64;
        const int CH = 3;

        std::vector<float> expect((size_t)N * CH);
        std::vector<uint8_t> data((size_t)N * CH * UNITSIZE);
        for (uint32_t s = 0; s < N; ++s) {
            for (int c = 0; c < CH; ++c) {
                const float v = 1.25f * (float)(s * CH + c) - 7.5f;
                expect[(size_t)s * CH + c] = v;
                std::memcpy(data.data() + ((size_t)s * CH + c) * UNITSIZE,
                            &v, sizeof(float));
            }
        }

        AnalogFixture fx({0, 1, 2}, UNITSIZE, /*is_float=*/true);
        AnalogSnapshot snap;
        fx.feed_all(snap, N, data.data(), N);

        for (int c = 0; c < CH; ++c) {
            const SampleSpan sp = snap.span((uint32_t)c, 10, 1);
            QVERIFY2(sp.valid(), qPrintable(QString("span invalid ch=%1").arg(c)));
            // 步长必须是交织步长，绝不是 EnvelopeScaleFactor
            QCOMPARE(sp.stride, (uint32_t)(UNITSIZE * CH));
            QVERIFY(sp.stride != (uint32_t)snap.get_scale_factor());

            // B+C 收口：data 必须已指向本通道第一个样本
            const int order = snap.get_ch_order(c);
            QVERIFY(order >= 0);
            QCOMPARE(sp.data, sp.group_base + (uint64_t)order * UNITSIZE);
            // group_base 必须仍是"整块样本组"基址：同一起点的所有通道共享它，
            // 且 order == 0 的通道其 data 就等于 group_base。
            // （注意：不能与测试自己的输入向量比地址 —— 快照有自己的
            //   malloc 缓冲，两者地址无关。）
            const SampleSpan sp_ch0 = snap.span(0, 10, 1);
            QCOMPARE(sp.group_base, sp_ch0.group_base);
            QCOMPARE(sp_ch0.data, sp_ch0.group_base);

            for (uint64_t k = 0; k < 16; ++k) {
                QCOMPARE(sp.analog_value_at(10 + k, true),
                         (double)expect[(size_t)(10 + k) * CH + c]);
            }
        }
    }

    // ---- 整数编码（unit_bytes=2，2 通道，小端拼接）----
    {
        const uint8_t UNITSIZE = 2;
        const uint32_t N = 32;
        const int CH = 2;

        std::vector<uint16_t> expect((size_t)N * CH);
        std::vector<uint8_t> data((size_t)N * CH * UNITSIZE);
        for (uint32_t s = 0; s < N; ++s) {
            for (int c = 0; c < CH; ++c) {
                const uint16_t v = (uint16_t)(s * 100 + c * 7 + 1000);
                expect[(size_t)s * CH + c] = v;
                uint8_t *p = data.data() + ((size_t)s * CH + c) * UNITSIZE;
                p[0] = (uint8_t)(v & 0xFF);
                p[1] = (uint8_t)(v >> 8);
            }
        }

        AnalogFixture fx({0, 1}, UNITSIZE, /*is_float=*/false);
        AnalogSnapshot snap;
        fx.feed_all(snap, N, data.data(), N);

        for (int c = 0; c < CH; ++c) {
            const SampleSpan sp = snap.span((uint32_t)c, 0, 1);
            QVERIFY(sp.valid());
            QCOMPARE(sp.stride, (uint32_t)(UNITSIZE * CH));
            QCOMPARE(sp.data,
                     sp.group_base + (uint64_t)snap.get_ch_order(c) * UNITSIZE);
            for (uint64_t k = 0; k < 8; ++k) {
                QCOMPARE(sp.analog_value_at(k, false),
                         (double)expect[(size_t)k * CH + c]);
            }
        }
    }

    // ---- 非恒等 _ch_index（demo 布局 {8,9}）----
    //
    // 这是"通道索引 vs 内部 order"混用缺陷的回归钉：_ch_index 非恒等时，
    // span() 的 channel 参数必须真正影响结果（data 落在本通道的字节上），
    // 且 get_ch_order() 查不到时要返回空 span 而不是静默给出别通道的数据。
    {
        const uint8_t UNITSIZE = 1;
        const uint32_t N = 32;
        const int CH = 2;
        const int CH_BASE = 8;   // 模拟通道索引 = 逻辑通道数 + i

        std::vector<uint8_t> expect((size_t)N * CH);
        std::vector<uint8_t> data((size_t)N * CH * UNITSIZE);
        for (uint32_t s = 0; s < N; ++s) {
            for (int c = 0; c < CH; ++c) {
                const uint8_t v = (uint8_t)(s * 3 + c * 40);
                expect[(size_t)s * CH + c] = v;
                data[(size_t)s * CH + c] = v;
            }
        }

        AnalogFixture fx({CH_BASE, CH_BASE + 1}, UNITSIZE, /*is_float=*/false);
        AnalogSnapshot snap;
        fx.feed_all(snap, N, data.data(), N);

        QCOMPARE(snap.get_ch_order(CH_BASE), 0);
        QCOMPARE(snap.get_ch_order(CH_BASE + 1), 1);

        for (int c = 0; c < CH; ++c) {
            const SampleSpan sp = snap.span((uint32_t)(CH_BASE + c), 0, 1);
            QVERIFY2(sp.valid(), qPrintable(QString("span invalid ch=%1").arg(CH_BASE + c)));
            QCOMPARE(sp.data, sp.group_base + (uint64_t)c * UNITSIZE);
            for (uint64_t k = 0; k < 8; ++k) {
                QCOMPARE(sp.analog_value_at(k, false),
                         (double)expect[(size_t)k * CH + c]);
            }
        }

        // 内部 order 不是合法通道索引 → 必须返回空 span（这正是 rasterize.cpp
        // 曾经踩中的分支：把 order 当通道索引传进来）
        QVERIFY2(!snap.span(0, 0, 1).valid(),
                 "order 0 is not a valid channel index for _ch_index={8,9}");
        QVERIFY2(!snap.span(1, 0, 1).valid(),
                 "order 1 is not a valid channel index for _ch_index={8,9}");
    }
}

QTEST_GUILESS_MAIN(TestSampleSpan)
#include "test_sample_span.moc"
