/*
 * test_demo_1ghz_benchmark.cpp — 定量分析 Demo 设备 1GHz 125ms 采集性能瓶颈
 *
 * 场景量化目标：
 *   - 采样率: 1 GHz (1,000,000,000 Hz)
 *   - 采样时长: 125 ms (0.125 s)
 *   - 目标采样点数: 125,000,000 (1.25 亿样本)
 *   - 数据量: 16 通道 = 250 MB, 32 通道 = 500 MB
 *
 * 五大分解阶段定量测量：
 *   1. 纯波形生成与位重排 (Pattern Generation & pack_8_cross)
 *   2. 格式转置开销 (Fast Path vs Interleaved 转置)
 *   3. LogicSnapshot 内存入库与 Mmap 扩展 (Ingestion & LeafBlock 分配)
 *   4. 采集收尾与 Mipmap 抽样金字塔计算 (capture_ended & calc_mipmap)
 *   5. 端到端全链路汇总与瓶颈归因
 */

#include <QtTest/QtTest>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
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
#include "pv/data/snapshot/logicsnapshot_diskcache_writer.h"
#undef private
#undef protected

using namespace pv::data;
using Clock = std::chrono::steady_clock;

namespace {

constexpr uint64_t TARGET_SAMPLES = 125ULL * 1000 * 1000; // 125 M 样本
constexpr int CH_16 = 16;
constexpr int CH_32 = 32;

// 模拟 demo 驱动每次 tick 生成的批次大小: 8M 样本
constexpr uint64_t TICK_BATCH_SAMPLES = 8ULL * 1000 * 1000;

volatile uint64_t g_benchmark_sink = 0;

inline double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// 模拟 demo/protocol.c 中的 pattern_sigrok 数组
static const uint8_t pattern_sigrok_sample[] = {
    0x4c, 0x92, 0x92, 0x92, 0x64, 0x00, 0x00, 0x00,
    0x82, 0xfe, 0xfe, 0x82, 0x00, 0x00, 0x00, 0x00,
    0x7c, 0x82, 0x82, 0x92, 0x74, 0x00, 0x00, 0x00,
    0xfe, 0x12, 0x12, 0x32, 0xcc, 0x00, 0x00, 0x00,
    0x7c, 0x82, 0x82, 0x82, 0x7c, 0x00, 0x00, 0x00,
    0xfe, 0x10, 0x28, 0x44, 0x82, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xbe, 0xbe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// 模拟 demo/protocol.c 中的 pack_8_cross 内联函数
static inline void sim_pack_8_cross(uint8_t *group, int s8,
    const uint64_t vals[8], const int *enabled_ch, int num_channels)
{
    for (int k = 0; k < num_channels; k++) {
        int ch = enabled_ch[k];
        uint64_t mask = 1ULL << ch;
        group[k * 8 + s8] = (uint8_t)(
            (!!(vals[0] & mask))       |
            (!!(vals[1] & mask)) << 1  |
            (!!(vals[2] & mask)) << 2  |
            (!!(vals[3] & mask)) << 3  |
            (!!(vals[4] & mask)) << 4  |
            (!!(vals[5] & mask)) << 5  |
            (!!(vals[6] & mask)) << 6  |
            (!!(vals[7] & mask)) << 7);
    }
}

// 模拟 demo 驱动生成 Sigrok 模式的 LA_CROSS_DATA 数据块
static void sim_generate_sigrok_cross(uint8_t *dst, uint64_t num_samples,
                                      int num_channels, uint64_t &step)
{
    int enabled_ch[32];
    for (int i = 0; i < num_channels; ++i) enabled_ch[i] = i;

    uint64_t num_groups = num_samples / 64;
    size_t group_size = (size_t)num_channels * 8;
    size_t unitsize = (num_channels + 7) / 8;

    for (uint64_t g = 0; g < num_groups; g++) {
        uint8_t *group = dst + g * group_size;
        for (int s8 = 0; s8 < 8; s8++) {
            uint64_t vals[8];
            for (int b = 0; b < 8; b++) {
                vals[b] = 0;
                for (size_t j = 0; j < unitsize; j++) {
                    uint8_t pat = pattern_sigrok_sample[
                        (step + j) % sizeof(pattern_sigrok_sample)] >> 1;
                    vals[b] |= ((uint64_t)(uint8_t)~pat) << (j * 8);
                }
                step++;
            }
            sim_pack_8_cross(group, s8, vals, enabled_ch, num_channels);
        }
    }
}

// 模拟转置函数：把 Interleaved 数据转为 LA_CROSS_DATA 格式
static void sim_convert_to_cross(const uint8_t *src, uint8_t *dst,
                                 uint64_t num_samples, size_t unitsize, int num_channels)
{
    uint64_t num_groups = num_samples / 64;
    std::memset(dst, 0, num_groups * num_channels * 8);

    for (uint64_t g = 0; g < num_groups; g++) {
        const uint8_t *src_g = src + (g * 64) * unitsize;
        uint8_t *dst_g = dst + g * num_channels * 8;
        for (int ch = 0; ch < num_channels; ch++) {
            size_t bp = (size_t)(ch >> 3);
            uint8_t mask = (uint8_t)(1u << (ch & 7));
            uint8_t *out = dst_g + ch * 8;
            for (uint64_t s8 = 0; s8 < 8; s8++) {
                uint8_t ob = 0;
                const uint8_t *p = src_g + (s8 * 8) * unitsize + bp;
                for (uint64_t b = 0; b < 8; b++) {
                    if (p[b * unitsize] & mask)
                        ob |= (uint8_t)(1u << b);
                }
                out[s8] = ob;
            }
        }
    }
}

struct BenchmarkFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;
    int channel_num;

    explicit BenchmarkFixture(int ch_count)
        : chs(ch_count), nodes(ch_count), channel_num(ch_count)
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

} // namespace

class TestDemo1GHzBenchmark : public QObject
{
    Q_OBJECT

private:
    double cost_pattern_gen_ms = 0.0;
    double cost_interleaved_convert_ms = 0.0;
    double cost_snapshot_ingest_ms = 0.0;
    double cost_capture_ended_ms = 0.0;

private slots:
    void initTestCase();

    // 阶段 1: 纯波形生成与位重排算力基准 (125M 样本)
    void benchmark_pattern_generation();

    // 阶段 2: 格式转置开销对比 (Fast Path vs 软触发交织转置)
    void benchmark_format_transposition();

    // 阶段 3: LogicSnapshot 内存入库与 Mmap 扩展耗时
    void benchmark_snapshot_ingestion();

    // 阶段 4: 采集收尾与 Mipmap 抽样金字塔计算耗时
    void benchmark_capture_ended_and_mipmap();

    // 阶段 5: 全链路瓶颈定量对比与诊断报告
    void benchmark_bottleneck_diagnosis_report();
};

void TestDemo1GHzBenchmark::initTestCase()
{
    qInfo("================================================================================");
    qInfo("PXView Benchmark: 定量分析 Demo 设备采集 1GHz 125ms (1.25 亿样本) 性能瓶颈");
    qInfo("================================================================================");
}

void TestDemo1GHzBenchmark::benchmark_pattern_generation()
{
    // 测试 125M 样本在 16 通道下的纯生成开销
    // 每次 tick 8M 样本，生成 16 次
    const uint64_t BATCH_SAMPLES = TICK_BATCH_SAMPLES;
    const uint64_t BATCH_BYTES = (BATCH_SAMPLES / 64) * CH_16 * 8; // 8M 样本 16 通道 = 16 MB
    std::vector<uint8_t> batch_buf(BATCH_BYTES);

    uint64_t step = 0;

    // 预热一轮
    sim_generate_sigrok_cross(batch_buf.data(), 1024 * 64, CH_16, step);

    const int iterations = static_cast<int>(TARGET_SAMPLES / BATCH_SAMPLES); // 15 次 8M + 1 次 5M
    const uint64_t tail_samples = TARGET_SAMPLES - (iterations * BATCH_SAMPLES);

    qInfo("\n[阶段 1] 测量 Demo 驱动生成 1.25 亿样本 (Sigrok 模式, 16 通道, 250MB)...");

    auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        sim_generate_sigrok_cross(batch_buf.data(), BATCH_SAMPLES, CH_16, step);
        g_benchmark_sink += batch_buf[0];
    }
    if (tail_samples > 0) {
        sim_generate_sigrok_cross(batch_buf.data(), tail_samples, CH_16, step);
        g_benchmark_sink += batch_buf[0];
    }
    cost_pattern_gen_ms = ms_since(t0);

    const double mb_total = (TARGET_SAMPLES * 2) / (1024.0 * 1024.0);
    const double throughput_mbps = (mb_total / (cost_pattern_gen_ms / 1000.0));
    const double msamples_per_sec = (TARGET_SAMPLES / 1e6) / (cost_pattern_gen_ms / 1000.0);

    qInfo("  -> 生成 125M 样本耗时: %.2f ms (%.2f 秒)", cost_pattern_gen_ms, cost_pattern_gen_ms / 1000.0);
    qInfo("  -> 纯生成吞吐: %.1f MB/s (%.1f M Samples/s)", throughput_mbps, msamples_per_sec);
}

void TestDemo1GHzBenchmark::benchmark_format_transposition()
{
    // 测试如果在触发等待状态下走 Standard Path，把 Interleaved 数据转为 CROSS 数据的开销
    const uint64_t SAMPLES_BENCH = 8ULL * 1024 * 1024; // 8M 样本切片进行测试然后折算
    const size_t unitsize = (CH_16 + 7) / 8; // 2 字节/样本
    std::vector<uint8_t> interleaved(SAMPLES_BENCH * unitsize, 0x5A);
    std::vector<uint8_t> cross((SAMPLES_BENCH / 64) * CH_16 * 8);

    auto t0 = Clock::now();
    sim_convert_to_cross(interleaved.data(), cross.data(), SAMPLES_BENCH, unitsize, CH_16);
    double single_batch_ms = ms_since(t0);
    g_benchmark_sink += cross[0];

    // 折算到 125M 样本
    cost_interleaved_convert_ms = single_batch_ms * (static_cast<double>(TARGET_SAMPLES) / SAMPLES_BENCH);

    qInfo("\n[阶段 2] 格式转置开销分析 (Interleaved -> LA_CROSS_DATA 位矩阵转置)...");
    qInfo("  -> 8M 样本转置耗时: %.2f ms", single_batch_ms);
    qInfo("  -> 125M 样本若走交织转置额外耗时: %.2f ms (%.2f 秒)",
          cost_interleaved_convert_ms, cost_interleaved_convert_ms / 1000.0);
}

void TestDemo1GHzBenchmark::benchmark_snapshot_ingestion()
{
    // 测试将 125M 样本 (16 通道 250MB) 喂入 LogicSnapshot 的吞吐与入库耗时
    BenchmarkFixture fx(CH_16);
    const uint64_t PAYLOAD_BYTES = (TICK_BATCH_SAMPLES / 64) * CH_16 * 8; // 16 MB payload
    std::vector<uint8_t> payload_data(PAYLOAD_BYTES, 0xAA);

    LogicSnapshot snap;
    sr_datafeed_logic l{};
    l.length = PAYLOAD_BYTES;
    l.data = payload_data.data();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;

    qInfo("\n[阶段 3] 测量 LogicSnapshot 内存入库 (16 个 16MB Payload, 共 250MB)...");

    auto t_start = Clock::now();
    snap.first_payload(l, TARGET_SAMPLES, &fx.nodes[0], true);

    const int total_batches = static_cast<int>(TARGET_SAMPLES / TICK_BATCH_SAMPLES);
    for (int i = 1; i < total_batches; ++i) {
        snap.append_payload(l);
    }

    // 等待异步写入队列彻底完成落库
    snap._disk_cache_writer->drain_queue_for_capture_end();
    cost_snapshot_ingest_ms = ms_since(t_start);

    const double mb_total = (TARGET_SAMPLES * 2) / (1024.0 * 1024.0);
    const double ingest_mbps = (mb_total / (cost_snapshot_ingest_ms / 1000.0));

    qInfo("  -> 125M 样本入库与 Mmap 扩展总耗时: %.2f ms (%.2f 秒)",
          cost_snapshot_ingest_ms, cost_snapshot_ingest_ms / 1000.0);
    qInfo("  -> 入库吞吐: %.1f MB/s", ingest_mbps);

    // 阶段 4 紧接着在这同一个 snapshot 上测量 capture_ended 耗时
    auto t_end = Clock::now();
    snap.capture_ended();
    cost_capture_ended_ms = ms_since(t_end);

    qInfo("\n[阶段 4] 测量采集收尾与 Mipmap 金字塔计算 (125M 样本)...");
    qInfo("  -> capture_ended (含 64x Mipmap 边沿扫描) 耗时: %.2f ms (%.2f 秒)",
          cost_capture_ended_ms, cost_capture_ended_ms / 1000.0);
}

void TestDemo1GHzBenchmark::benchmark_capture_ended_and_mipmap()
{
    // 已在 benchmark_snapshot_ingestion 中测量完成
    QVERIFY(cost_capture_ended_ms >= 0.0);
}

void TestDemo1GHzBenchmark::benchmark_bottleneck_diagnosis_report()
{
    // 计算 Demo 定时器调度等待下限
    // Demo 定时器每 25ms 唤醒一次，上限 8M 样本/tick。125M 样本至少需要 16 次定时器唤醒:
    const double min_timer_sleep_ms = 16.0 * 25.0; // 400 ms 理论时钟

    const double total_pipeline_ms = cost_pattern_gen_ms + cost_snapshot_ingest_ms + cost_capture_ended_ms;

    qInfo("\n================================================================================");
    qInfo("                Demo 设备 1GHz 125ms 采集性能瓶颈诊断汇总");
    qInfo("================================================================================");
    qInfo("1. Demo 驱动纯软件波形生成 (pack_8_cross) : %8.2f ms (%5.1f%%)",
          cost_pattern_gen_ms, (cost_pattern_gen_ms / total_pipeline_ms) * 100.0);
    qInfo("2. LogicSnapshot 内存入库与 Mmap 扩展     : %8.2f ms (%5.1f%%)",
          cost_snapshot_ingest_ms, (cost_snapshot_ingest_ms / total_pipeline_ms) * 100.0);
    qInfo("3. 采集收尾与 64x Mipmap 金字塔计算        : %8.2f ms (%5.1f%%)",
          cost_capture_ended_ms, (cost_capture_ended_ms / total_pipeline_ms) * 100.0);
    qInfo("--------------------------------------------------------------------------------");
    qInfo("核心计算总耗时 (CPU 密集部分)             : %8.2f ms (%.2f 秒)",
          total_pipeline_ms, total_pipeline_ms / 1000.0);
    qInfo("附: 软触发交织转置开销 (若触发未命中时)    : %8.2f ms (%.2f 秒)",
          cost_interleaved_convert_ms, cost_interleaved_convert_ms / 1000.0);
    qInfo("================================================================================\n");

    QVERIFY(total_pipeline_ms > 0.0);
}

QTEST_MAIN(TestDemo1GHzBenchmark)
#include "test_demo_1ghz_benchmark.moc"
