/*
 * test_dso_snapshot.cpp — DsoSnapshot 数据层单元测试
 *
 * 验证 DsoSnapshot 的 first_payload / append_payload 数据写入与读取:
 *   - 单通道 8-bit 数据 (non-instant 覆盖式写入)
 *   - 多通道 interleaved 字节 (跨通道 get_samples / get_max_min_value)
 *   - instant 模式多包追加 (采样在通道缓冲中连续拼接)
 *   - threshold / measure_voltage_factor / data_scale / ref_range 存取
 *   - get_envelope_section 第一级 mipmap (EnvelopeScaleFactor = 256)
 *   - 空数据 / 越界读取边界
 *
 * 依赖链: dsosnapshot + snapshot + pxvdef (header-only) + libsigrok 头
 *         + common (xlog.h) — 内部 stub xlog。纯数据层, 无 QWidget 依赖。
 * 全部使用公开 API; 头文件提供的 friend class DsoSnapshotTest::Basic 未用到。
 */

#include <QtTest/QtTest>

// ── 标准库头必须在目标头之前 include ──
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <libsigrok/libsigrok.h>   // sr_datafeed_dso / sr_channel / GSList

// ── xlog stub: dsosnapshot.cpp 经 log.h 引用 pxv_log + xlog_* ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include "pv/data/snapshot/dsosnapshot.h"

using namespace pv::data;

namespace {

// 构造 SR_DF_DSO 数据包 + GSList 通道列表。
// DSO 数据格式: interleaved 字节, 每样本步长 = 通道数。
struct DsoFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    sr_datafeed_dso dso{};

    explicit DsoFixture(const std::vector<int> &ch_indexes)
        : chs(ch_indexes.size()), nodes(ch_indexes.size())
    {
        for (size_t i = 0; i < chs.size(); ++i) {
            chs[i].index = ch_indexes[i];
            chs[i].type = SR_CHANNEL_DSO;
            chs[i].enabled = TRUE;
            chs[i].name = nullptr;
            nodes[i].data = &chs[i];
            nodes[i].next = (i + 1 < chs.size()) ? &nodes[i + 1] : nullptr;
        }
    }

    // 一次性喂入 (first_payload 内部即调用 append_payload)
    void feed(DsoSnapshot &snap, uint64_t total, const void *data,
              uint32_t num_samples, bool instant, bool isFile)
    {
        dso.data = const_cast<void *>(data);
        dso.num_samples = num_samples;
        dso.trig_flag = 0;
        dso.trig_ch = 0;
        dso.en_ch_num = (uint8_t)chs.size();
        dso.sample_bits = 8;
        dso.trig_offset = 0;
        dso.packet_len = 0;
        dso.samplerate_tog = 0;
        snap.first_payload(dso, total, &nodes[0], instant, isFile);
        snap.capture_ended();
    }
};

} // anonymous namespace

class TestDsoSnapshot : public QObject
{
    Q_OBJECT

private slots:
    // 单通道 non-instant: 样本数 / get_samples 对拍 / get_max_min_value / 越界
    void test_single_channel();
    // 双通道 interleaved: 跨通道读取 / has_data / 每通道 max/min
    void test_multi_channel_cross_read();
    // instant 模式多包追加: 采样连续拼接
    void test_instant_append();
    // threshold / measure_voltage_factor / data_scale / ref_range / is_file
    void test_setter_getter_roundtrip();
    // envelope 第一级 mipmap 的 start/scale/length/min/max
    void test_envelope_section();
    // 空数据 (num_samples=0) 与越界读取边界
    void test_empty_boundary();
};

void TestDsoSnapshot::test_single_channel()
{
    const uint64_t N = 256;
    std::vector<uint8_t> data(N);
    for (uint64_t i = 0; i < N; ++i)
        data[i] = (uint8_t)i;

    DsoFixture fx({0});
    DsoSnapshot snap;
    fx.feed(snap, N, data.data(), (uint32_t)N, /*instant=*/false, /*isFile=*/false);

    QCOMPARE(snap.get_sample_count(), N);
    QVERIFY(snap.last_ended());

    const uint8_t *p = snap.get_samples(0, (int64_t)N - 1, /*ch_index=*/0);
    QVERIFY(p != nullptr);
    for (uint64_t i = 0; i < N; ++i)
        QCOMPARE((int)p[i], (int)data[i]);

    // 非零起点: 返回缓冲指向 start 处
    const uint8_t *p2 = snap.get_samples(100, 200, 0);
    QVERIFY(p2 != nullptr);
    QCOMPARE((int)p2[0], 100);

    uint8_t maxv = 0, minv = 0;
    QVERIFY(snap.get_max_min_value(maxv, minv, 0));
    QCOMPARE((int)maxv, 255);
    QCOMPARE((int)minv, 0);

    // 越界 / 非法区间 → nullptr
    QVERIFY(snap.get_samples(0, (int64_t)N, 0) == nullptr);
    QVERIFY(snap.get_samples(-1, 5, 0) == nullptr);
    QVERIFY(snap.get_samples(5, 4, 0) == nullptr);
}

void TestDsoSnapshot::test_multi_channel_cross_read()
{
    const uint64_t N = 200;
    std::vector<uint8_t> data(N * 2);   // 2 ch interleaved, 每样本 2 字节
    for (uint64_t i = 0; i < N; ++i) {
        data[i * 2 + 0] = (uint8_t)i;            // ch0 = ramp 0..199
        data[i * 2 + 1] = (uint8_t)(255 - i);    // ch1 = 反向 ramp 255..56
    }

    DsoFixture fx({0, 1});
    DsoSnapshot snap;
    fx.feed(snap, N, data.data(), (uint32_t)N, /*instant=*/false, /*isFile=*/false);

    QCOMPARE(snap.get_sample_count(), N);
    QCOMPARE(snap.get_channel_num(), 2u);
    QVERIFY(snap.has_data(0));
    QVERIFY(snap.has_data(1));
    QVERIFY(!snap.has_data(9));

    const uint8_t *c0 = snap.get_samples(0, (int64_t)N - 1, 0);
    const uint8_t *c1 = snap.get_samples(0, (int64_t)N - 1, 1);
    QVERIFY(c0 != nullptr);
    QVERIFY(c1 != nullptr);
    for (uint64_t i = 0; i < N; ++i) {
        QCOMPARE((int)c0[i], (int)(i & 0xFF));
        QCOMPARE((int)c1[i], (int)(255 - i));
    }

    uint8_t maxv = 0, minv = 0;
    QVERIFY(snap.get_max_min_value(maxv, minv, 0));
    QCOMPARE((int)maxv, 199);
    QCOMPARE((int)minv, 0);
    QVERIFY(snap.get_max_min_value(maxv, minv, 1));
    QCOMPARE((int)maxv, 255);
    QCOMPARE((int)minv, 56);   // 255-199

    // 不存在的通道 → nullptr
    QVERIFY(snap.get_samples(0, (int64_t)N - 1, 9) == nullptr);
}

void TestDsoSnapshot::test_instant_append()
{
    const uint64_t TOTAL = 256;
    DsoFixture fx({0});
    DsoSnapshot snap;

    // 第一包 100 样本 (instant=true)
    std::vector<uint8_t> pkt1(100);
    for (int i = 0; i < 100; ++i)
        pkt1[i] = (uint8_t)i;
    fx.dso.data = pkt1.data();
    fx.dso.num_samples = 100;
    fx.dso.en_ch_num = 1;
    fx.dso.sample_bits = 8;
    fx.dso.samplerate_tog = 0;
    snap.first_payload(fx.dso, TOTAL, &fx.nodes[0], /*instant=*/true, /*isFile=*/false);
    QCOMPARE(snap.get_sample_count(), 100ull);

    // 第二包 50 样本 → 追加到偏移 100
    std::vector<uint8_t> pkt2(50);
    for (int i = 0; i < 50; ++i)
        pkt2[i] = (uint8_t)(200 + i);
    fx.dso.data = pkt2.data();
    fx.dso.num_samples = 50;
    snap.append_payload(fx.dso);
    QCOMPARE(snap.get_sample_count(), 150ull);

    const uint8_t *p = snap.get_samples(0, 149, 0);
    QVERIFY(p != nullptr);
    for (int i = 0; i < 100; ++i)
        QCOMPARE((int)p[i], i);
    for (int i = 0; i < 50; ++i)
        QCOMPARE((int)p[100 + i], 200 + i);

    snap.capture_ended();
}

void TestDsoSnapshot::test_setter_getter_roundtrip()
{
    DsoFixture fx({0});
    DsoSnapshot snap;

    // threshold
    snap.set_threshold(3.5f);
    QVERIFY(qAbs(snap.get_threshold() - 3.5f) < 1e-6f);

    // measure_voltage_factor (双因子槽)
    snap.set_measure_voltage_factor(1234, 0);
    snap.set_measure_voltage_factor(5678, 1);
    QCOMPARE((quint64)snap.get_measure_voltage_factor(0), (quint64)1234);
    QCOMPARE((quint64)snap.get_measure_voltage_factor(1), (quint64)5678);

    // data_scale (双因子槽)
    snap.set_data_scale(2.5f, 0);
    snap.set_data_scale(0.25f, 1);
    QVERIFY(qAbs(snap.get_data_scale(0) - 2.5f) < 1e-6f);
    QVERIFY(qAbs(snap.get_data_scale(1) - 0.25f) < 1e-6f);

    // ref_range 在 append_data 时判定越界
    const uint64_t N = 64;
    std::vector<uint8_t> ok(N, 128);
    std::vector<uint8_t> low(N, 5);
    std::vector<uint8_t> high(N, 250);

    {
        DsoSnapshot s;
        s.set_ref_range(200, 100);
        fx.feed(s, N, ok.data(), (uint32_t)N, false, false);
        QVERIFY(!s.data_is_out_off_range());   // 128 ∈ [100,200]
    }
    {
        DsoSnapshot s;
        s.set_ref_range(200, 100);
        fx.feed(s, N, low.data(), (uint32_t)N, false, false);
        QVERIFY(s.data_is_out_off_range());    // 5 < 100
    }
    {
        DsoSnapshot s;
        s.set_ref_range(200, 100);
        fx.feed(s, N, high.data(), (uint32_t)N, false, false);
        QVERIFY(s.data_is_out_off_range());    // 250 > 200
    }

    // is_file 标记
    {
        DsoFixture fxFile({0});
        DsoSnapshot s;
        std::vector<uint8_t> d(N, 1);
        fxFile.feed(s, N, d.data(), (uint32_t)N, false, /*isFile=*/true);
        QVERIFY(s.is_file());
    }
}

void TestDsoSnapshot::test_envelope_section()
{
    const uint64_t N = 512;
    std::vector<uint8_t> data(N);
    for (uint64_t i = 0; i < N; ++i)
        data[i] = (uint8_t)(i & 0xFF);   // 0..255, 0..255

    DsoFixture fx({0});
    DsoSnapshot snap;
    fx.feed(snap, N, data.data(), (uint32_t)N, /*instant=*/false, /*isFile=*/false);

    snap.enable_envelope(true);   // header 一次性生成 envelope + 开启 _envelope_en

    // EnvelopeScaleFactor = 1<<8 = 256
    DsoSnapshot::EnvelopeSection s;
    snap.get_envelope_section(s, 0, N, 1.0f, /*probe_index=*/0);
    QCOMPARE(s.scale, 256u);                  // 1 << (0+1)*8
    QCOMPARE(s.start, 0ull);                  // (0>>8)<<8
    QCOMPARE(s.length, 2ull);                 // (512>>8)-(0>>8)
    QVERIFY(s.samples != nullptr);
    QCOMPARE((int)s.samples[0].min, 0);       // 原始样本 0..255
    QCOMPARE((int)s.samples[0].max, 255);
    QCOMPARE((int)s.samples[1].min, 0);       // 原始样本 256..511
    QCOMPARE((int)s.samples[1].max, 255);

    DsoSnapshot::EnvelopeSection s2;
    snap.get_envelope_section(s2, 256, N, 1.0f, 0);
    QCOMPARE(s2.start, 256ull);               // (256>>8)<<8
    QCOMPARE(s2.scale, 256u);
    QCOMPARE(s2.length, 1ull);
    QCOMPARE((int)s2.samples[0].min, 0);
    QCOMPARE((int)s2.samples[0].max, 255);
}

void TestDsoSnapshot::test_empty_boundary()
{
    // 从未喂数据: 查询全部返回空
    {
        DsoFixture fx({0});
        DsoSnapshot snap;
        QCOMPARE(snap.get_sample_count(), 0ull);
        QVERIFY(snap.empty());
        uint8_t maxv = 0, minv = 0;
        QVERIFY(!snap.get_max_min_value(maxv, minv, 0));
        QVERIFY(snap.get_samples(0, 0, 0) == nullptr);
    }

    // num_samples=0 的 first_payload: 分配缓冲但不写数据
    {
        DsoFixture fx({0});
        DsoSnapshot snap;
        std::vector<uint8_t> none;
        fx.feed(snap, 128, none.data(), 0, false, false);
        QCOMPARE(snap.get_sample_count(), 0ull);
        QVERIFY(snap.empty());
        uint8_t maxv = 0, minv = 0;
        QVERIFY(!snap.get_max_min_value(maxv, minv, 0));
    }
}

QTEST_GUILESS_MAIN(TestDsoSnapshot)
#include "test_dso_snapshot.moc"
