/*
 * test_analog_snapshot.cpp — AnalogSnapshot 数据层单元测试
 *
 * 验证 AnalogSnapshot 的 first_payload / append_payload 数据写入与读取:
 *   - float 电压数据 (encoding->unitsize=4, is_float) 单通道
 *   - float 多通道 interleaved (meaning->channels == 全部通道)
 *   - demo 式逐通道 partial 写入 (meaning->channels 少于通道总数)
 *   - get_float_min_max / has_float_range 范围跟踪 (float 数据场景)
 *   - get_envelope_section 第一级 mipmap 的 start/scale/length/min/max
 *   - uint16 整数数据 (is_float=false) 与空数据/越界边界
 *
 * 依赖链: analogsnapshot + snapshot + pxvdef (header-only) + libsigrok 头
 *         + common (xlog.h) — 内部 stub xlog。纯数据层, 无 QWidget 依赖。
 * 全部使用公开 API; 头文件提供的 friend class AnalogSnapshotTest::Basic 未用到。
 */

#include <QtTest/QtTest>

// ── 标准库头必须在目标头之前 include ──
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <libsigrok/libsigrok.h>   // sr_datafeed_analog / sr_channel / GSList

// ── xlog stub: analogsnapshot.cpp 经 log.h 引用 pxv_log + xlog_* ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include "pv/data/snapshot/analogsnapshot.h"

using namespace pv::data;

namespace {

// 构造 SR_DF_ANALOG 数据包 + GSList 通道列表。
// channels 指定快照配置的模拟通道 (SR_CHANNEL_ANALOG)。
struct AnalogFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    sr_analog_encoding encoding{};
    sr_analog_meaning meaning{};
    sr_analog_spec spec{};
    sr_datafeed_analog analog{};

    AnalogFixture(const std::vector<int> &ch_indexes, uint8_t unitsize, bool is_float)
        : chs(ch_indexes.size()), nodes(ch_indexes.size())
    {
        for (size_t i = 0; i < chs.size(); ++i) {
            chs[i].index = ch_indexes[i];
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

    // 喂入全通道 interleaved 数据: meaning->channels == 全部通道
    void feed_all_channels(AnalogSnapshot &snap, uint64_t total,
                           const void *data, uint32_t num_samples)
    {
        meaning.channels = &nodes[0];
        analog.data = const_cast<void *>(data);
        analog.num_samples = num_samples;
        snap.first_payload(analog, total, &nodes[0]);
        snap.capture_ended();
    }

    // 喂入部分通道数据 (demo 式逐通道发送): meaning->channels 只含 pkt_indexes
    void feed_partial(AnalogSnapshot &snap, uint64_t total, const void *data,
                      uint32_t num_samples, const std::vector<int> &pkt_indexes)
    {
        std::vector<GSList> mnodes(pkt_indexes.size());
        for (size_t i = 0; i < pkt_indexes.size(); ++i) {
            sr_channel *ch = nullptr;
            for (auto &c : chs) {
                if (c.index == pkt_indexes[i]) {
                    ch = &c;
                    break;
                }
            }
            mnodes[i].data = ch;
            mnodes[i].next = (i + 1 < pkt_indexes.size()) ? &mnodes[i + 1] : nullptr;
        }
        meaning.channels = &mnodes[0];
        analog.data = const_cast<void *>(data);
        analog.num_samples = num_samples;
        snap.first_payload(analog, total, &nodes[0]);
        snap.capture_ended();
    }
};

} // anonymous namespace

class TestAnalogSnapshot : public QObject
{
    Q_OBJECT

private slots:
    // float 单通道 interleaved: 样本数 / float 范围 / get_samples 对拍 / 越界
    void test_float_single_channel();
    // float 双通道 interleaved: 跨通道读取 / get_ch_order / 范围
    void test_float_multi_channel_interleaved();
    // 逐通道 partial 写入 (demo 风格): 单通道数据落入正确槽位
    void test_float_partial_channel();
    // envelope 第一级 mipmap 的 start/scale/length/min/max
    void test_envelope_section();
    // uint16 整数数据 (is_float=false): 不启用 float 范围, 数据按字节对拍
    void test_integer_uint16_interleaved();
    // 空数据 (num_samples=0) 与越界读取边界
    void test_empty_and_out_of_range();
};

void TestAnalogSnapshot::test_float_single_channel()
{
    const uint64_t N = 1024;
    std::vector<float> data(N);
    for (uint64_t i = 0; i < N; ++i)
        data[i] = (float)i - 512.0f;   // -512 .. 511

    AnalogFixture fx({0}, /*unitsize=*/4, /*is_float=*/true);
    AnalogSnapshot snap;
    fx.feed_all_channels(snap, N, data.data(), (uint32_t)N);

    QCOMPARE(snap.get_sample_count(), N);
    QVERIFY(snap.is_float());
    QVERIFY(snap.last_ended());

    float minv = 0, maxv = 0;
    QVERIFY(snap.has_float_range());
    snap.get_float_min_max(minv, maxv);
    QCOMPARE(minv, -512.0f);
    QCOMPARE(maxv, 511.0f);

    // get_samples 返回 interleaved 缓冲, 单通道即原始 float 序列
    for (uint64_t start : std::vector<uint64_t>{0, 1, 100, 511, 1023}) {
        const uint8_t *p = snap.get_samples((int64_t)start);
        QVERIFY(p != nullptr);
        const float *f = reinterpret_cast<const float *>(p);
        const uint64_t remain = N - start;
        for (uint64_t i = 0; i < remain; ++i)
            QCOMPARE(f[i], data[start + i]);
    }

    // 越界 → nullptr
    QVERIFY(snap.get_samples((int64_t)N) == nullptr);
    QVERIFY(snap.get_samples(-1) == nullptr);
}

void TestAnalogSnapshot::test_float_multi_channel_interleaved()
{
    const uint64_t N = 512;
    std::vector<float> ch0(N), ch1(N), interleaved(N * 2);
    for (uint64_t i = 0; i < N; ++i) {
        ch0[i] = (float)i * 2.0f;
        ch1[i] = -(float)i * 3.0f;
        interleaved[i * 2 + 0] = ch0[i];
        interleaved[i * 2 + 1] = ch1[i];
    }

    AnalogFixture fx({0, 1}, /*unitsize=*/4, /*is_float=*/true);
    AnalogSnapshot snap;
    fx.feed_all_channels(snap, N, interleaved.data(), (uint32_t)N);

    QCOMPARE(snap.get_sample_count(), N);
    QVERIFY(snap.is_float());
    QCOMPARE(snap.get_channel_num(), 2u);
    QCOMPARE(snap.get_ch_order(0), 0);
    QCOMPARE(snap.get_ch_order(1), 1);
    QCOMPARE(snap.get_ch_order(7), -1);

    // 全通道 interleaved 的范围 = 所有通道值的 min/max
    float minv = 0, maxv = 0;
    QVERIFY(snap.has_float_range());
    snap.get_float_min_max(minv, maxv);
    QCOMPARE(minv, -1533.0f);   // -(511)*3
    QCOMPARE(maxv, 1022.0f);    // 511*2

    for (uint64_t start : std::vector<uint64_t>{0, 3, 100, 511}) {
        const uint8_t *p = snap.get_samples((int64_t)start);
        QVERIFY(p != nullptr);
        const float *f = reinterpret_cast<const float *>(p);
        for (uint64_t i = 0; i + start < N; ++i) {
            QCOMPARE(f[i * 2 + 0], ch0[start + i]);
            QCOMPARE(f[i * 2 + 1], ch1[start + i]);
        }
    }
}

void TestAnalogSnapshot::test_float_partial_channel()
{
    // 2 通道配置, 仅喂入 ch0 (demo 式逐通道发送)
    const uint64_t N = 512;
    std::vector<float> ch0(N);
    for (uint64_t i = 0; i < N; ++i)
        ch0[i] = (float)i;

    AnalogFixture fx({0, 1}, /*unitsize=*/4, /*is_float=*/true);
    AnalogSnapshot snap;
    fx.feed_partial(snap, N, ch0.data(), (uint32_t)N, {0});

    QCOMPARE(snap.get_sample_count(), N);   // 取所有通道写入偏移的最大值
    QVERIFY(snap.is_float());

    // 每通道独立写入: ch0 数据落在 interleaved 槽位 [i][0]
    const uint8_t *p = snap.get_samples(0);
    QVERIFY(p != nullptr);
    for (uint64_t i = 0; i < N; ++i) {
        const float *slot = reinterpret_cast<const float *>(p + i * 2 * 4);
        QCOMPARE(slot[0], ch0[i]);
    }

    // 逐通道 partial: float 范围仅覆盖已到达通道
    float minv = 0, maxv = 0;
    QVERIFY(snap.has_float_range());
    snap.get_float_min_max(minv, maxv);
    QCOMPARE(minv, 0.0f);
    QCOMPARE(maxv, 511.0f);
}

void TestAnalogSnapshot::test_envelope_section()
{
    // 2 通道配置, 只喂 ch0 → partial 路径更新每通道 offset, envelope 被生成
    const uint64_t N = 1024;
    std::vector<float> ch0(N);
    for (uint64_t i = 0; i < N; ++i)
        ch0[i] = (float)i;   // 单调递增 → 每 16 样本块的 min/max 可精确预测

    AnalogFixture fx({0, 1}, /*unitsize=*/4, /*is_float=*/true);
    AnalogSnapshot snap;
    fx.feed_partial(snap, N, ch0.data(), (uint32_t)N, {0});

    // EnvelopeScaleFactor = 1<<4 = 16
    AnalogSnapshot::EnvelopeSection s;
    snap.get_envelope_section(s, 0, (int64_t)N, 1.0f, /*probe_index=*/0);
    QCOMPARE(s.scale, 16u);              // 1 << (0+1)*4
    QCOMPARE(s.start, 0ull);             // 0 >> 4
    QCOMPARE(s.length, 64ull);           // 1024 >> 4
    QCOMPARE(s.samples_num, 64ull);      // 1024 / 16 个 envelope 样本
    QVERIFY(s.samples != nullptr);
    QCOMPARE(s.samples[0].min, 0.0f);    // 样本 0..15
    QCOMPARE(s.samples[0].max, 15.0f);
    QCOMPARE(s.samples[1].min, 16.0f);   // 样本 16..31
    QCOMPARE(s.samples[1].max, 31.0f);
    QCOMPARE(s.samples[63].min, 1008.0f);
    QCOMPARE(s.samples[63].max, 1023.0f);

    // 非零起点: start=256, count=512。
    // 注意实现契约: s.samples 指向 level 缓冲首地址 (不按 s.start 偏移),
    // 窗口内样本需用 s.samples[s.start + i] 索引。
    AnalogSnapshot::EnvelopeSection s2;
    snap.get_envelope_section(s2, 256, 512, 1.0f, /*probe_index=*/0);
    QCOMPARE(s2.start, 16ull);           // 256 >> 4
    QCOMPARE(s2.scale, 16u);
    QCOMPARE(s2.length, 32ull);          // 512 >> 4
    QCOMPARE(s2.samples_num, 64ull);     // 仍是 level-0 总样本数
    QCOMPARE(s2.samples[s2.start].min, 256.0f);   // 原始样本 256..271
    QCOMPARE(s2.samples[s2.start].max, 271.0f);
    QCOMPARE(s2.samples[s2.start + 1].min, 272.0f);
    QCOMPARE(s2.samples[s2.start + 1].max, 287.0f);
}

void TestAnalogSnapshot::test_integer_uint16_interleaved()
{
    const uint64_t N = 256;
    std::vector<uint16_t> ch0(N), ch1(N);
    std::vector<uint8_t> interleaved(N * 4);   // 2 ch * 2 字节
    for (uint64_t i = 0; i < N; ++i) {
        ch0[i] = (uint16_t)(i * 2);
        ch1[i] = (uint16_t)(i * 3);
        uint8_t *dst = interleaved.data() + i * 4;
        std::memcpy(dst, &ch0[i], 2);
        std::memcpy(dst + 2, &ch1[i], 2);
    }

    AnalogFixture fx({0, 1}, /*unitsize=*/2, /*is_float=*/false);
    AnalogSnapshot snap;
    fx.feed_all_channels(snap, N, interleaved.data(), (uint32_t)N);

    QCOMPARE(snap.get_sample_count(), N);
    QVERIFY(!snap.is_float());
    QVERIFY(!snap.has_float_range());   // 非 float 数据不启用范围跟踪

    const uint8_t *p = snap.get_samples(0);
    QVERIFY(p != nullptr);
    for (uint64_t i = 0; i < N; ++i) {
        uint16_t g0, g1;
        std::memcpy(&g0, p + i * 4, 2);
        std::memcpy(&g1, p + i * 4 + 2, 2);
        QCOMPARE((int)g0, (int)ch0[i]);
        QCOMPARE((int)g1, (int)ch1[i]);
    }
}

void TestAnalogSnapshot::test_empty_and_out_of_range()
{
    // num_samples = 0 → 无数据, 无 float 范围
    {
        AnalogFixture fx({0}, /*unitsize=*/4, /*is_float=*/true);
        AnalogSnapshot snap;
        std::vector<float> none;
        fx.feed_all_channels(snap, 100, none.data(), 0);

        QCOMPARE(snap.get_sample_count(), 0ull);
        QVERIFY(snap.empty());
        QVERIFY(!snap.has_float_range());
        QVERIFY(snap.get_samples(0) == nullptr);
    }

    // 越界读取边界
    {
        const uint64_t N = 64;
        std::vector<float> data(N, 1.0f);
        AnalogFixture fx({0}, /*unitsize=*/4, /*is_float=*/true);
        AnalogSnapshot snap;
        fx.feed_all_channels(snap, N, data.data(), (uint32_t)N);

        QCOMPARE(snap.get_sample_count(), N);
        QVERIFY(snap.get_samples((int64_t)N) == nullptr);
        QVERIFY(snap.get_samples((int64_t)N + 10) == nullptr);
        QVERIFY(snap.get_samples(-1) == nullptr);
    }
}

QTEST_GUILESS_MAIN(TestAnalogSnapshot)
#include "test_analog_snapshot.moc"
