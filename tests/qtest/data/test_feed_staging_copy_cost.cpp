/*
 * test_feed_staging_copy_cost.cpp — P2 决策门禁 + 生效门禁
 *
 * 背景
 * ----
 * 全项目唯一剩余的冗余整包拷贝（改造前）：
 *     PXView/pv/data/snapshot/logicsnapshot_diskcache_writer.cpp
 *         payload.data = std::vector<uint8_t>(data, data + length);
 * 它删不掉的原因是 libsigrok 的 payload 是 borrowed-by-contract：
 * 驱动在 sr_session_send() 返回后立刻 free_transfer()（pxlogic.c:2082）
 * 或 resubmit_transfer()（:2084）复用 USB buffer。
 *
 * 实测结论（决定 P2 方案）
 * ----------------------
 * 每 4MB payload，best-of-5：
 *   memcpy        92 us  ( 8.7%)
 *   分配          827 us (77.9%)
 *   回收/归还 OS  143 us (13.4%)
 *   ------> 瓶颈是「每 payload 一次的 4MB 堆分配」，不是 memcpy。
 * 物理核验：4MB = 1024 页，827us/1024 ≈ 808ns/页，与内核首次触碰新映射页的
 * 缺页+清零成本吻合。
 *
 * 因此 P2 选择 A3（固定槽位池 + 复用），而不是 A1（改 libsigrok 数据包契约）：
 * A1 只能再多拿那 8.7%，却要付 ABI 变更 + 波及 81 个驱动 + 放大悬垂指针风险。
 *
 * 本测试的两个职责
 * ----------------
 * 1) 决策门禁（primitive breakdown）：把分配开销与 memcpy 成本分开量化，
 *    证明"该做 A3 而不是 A1"。
 * 2) 生效门禁（staging copy cost）：对比「改造前的堆分配入队路径」与
 *    「现状 enqueue()（P2 池化实现）」，断言池化确实带来数量级的改善。
 *
 * 测量纪律（改动此测试时必须保持）
 * --------------------------------
 * - 所有写入变体必须用 volatile 稀疏读取消费目标缓冲，否则 -O2 死存储消除
 *   会给出"无穷大吞吐"的假数字。
 * - 堆分配必须用 range 构造函数 vector(data, data+len)；用 vector(n) 会先零填充
 *   （双倍写），得出约 2 倍虚高的分配开销。
 * - 低于计时分辨率的变体必须放大重复次数后折算。
 * - 多轮取最小值抑制调度抖动。
 *
 * 依赖链同 test_logic_snapshot_cross_throughput（纯数据层，无 QWidget）。
 */

#include <QtTest/QtTest>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
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

// `private public` 让测试能直接构造 LogicSnapshotDiskCacheWriter 并读取
// P2 槽位参数。所有标准库头必须在此之前 include。
#define private public
#define protected public
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/snapshot/logicsnapshot_diskcache_writer.h"
#undef private
#undef protected

using namespace pv::data;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int ROUNDS = 5;      // 多轮取最小值
constexpr int C_REPEAT = 500;  // 零拷贝变体放大倍数（低于计时分辨率）

volatile uint64_t g_sink = 0;

// volatile 读取：编译器无法把读取前移到拷贝之前（store-to-load forwarding），
// 因此无法消除 memcpy —— 否则会把变体测成"无穷大吞吐"。
inline void consume(const uint8_t *p, uint64_t bytes)
{
    const volatile uint8_t *vp = p;
    uint64_t h = 0;
    for (uint64_t i = 0; i < bytes; i += 4096)
        h += vp[i];
    g_sink += h;
}

double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double mbps(uint64_t total_bytes, double ms)
{
    return ms > 0.0 ? total_bytes / 1e6 / (ms / 1000.0) : 0.0;
}

// LA_CROSS_DATA 布局：每 64 样本/通道占 channel_num*8 字节
struct CrossFixture {
    std::vector<sr_channel> chs;
    std::vector<GSList> nodes;

    explicit CrossFixture(int ch_count) : chs(ch_count), nodes(ch_count)
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

    static std::vector<uint8_t> make_payload(uint64_t bytes, int ch_count,
                                             uint64_t seed)
    {
        const uint64_t chunk = (uint64_t)ch_count * 8;
        bytes -= bytes % chunk;
        std::vector<uint8_t> d(bytes);
        std::mt19937_64 rng(seed);
        for (auto &b : d) b = (uint8_t)(rng() & 0xFF);
        return d;
    }
};

} // anonymous namespace

class TestFeedStagingCopyCost : public QObject
{
    Q_OBJECT

private slots:
    void test_primitive_breakdown();
    void test_staging_copy_cost();
    void test_full_pipeline_reference();
};

// ---------------------------------------------------------------------------
// 决策门禁：把"纯分配开销"与"冷页/队列驻留代价"分开
//   X 分配+拷贝+消费+立即释放  → 热页（分配器复用），含分配开销
//   Y 预分配槽位+拷贝+消费      → 热页，无分配开销
//   Z 分配+拷贝+消费+全部持有   → 冷页 + 队列驻留
//   Y = memcpy 成本；Z-Y = 分配成本；X-Z = 回收/归还 OS 成本
// ---------------------------------------------------------------------------
void TestFeedStagingCopyCost::test_primitive_breakdown()
{
    const uint64_t PAYLOAD = 4ULL * 1024 * 1024;
    const int N = 48;
    const uint64_t TOTAL = PAYLOAD * N;

    std::vector<uint8_t> payload(PAYLOAD);
    std::mt19937_64 rng(0xBEEF);
    for (auto &b : payload) b = (uint8_t)(rng() & 0xFF);

    double x_ms = 1e18, y_ms = 1e18, z_ms = 1e18;

    for (int r = 0; r < ROUNDS; ++r) {
        // X：逐 payload 堆分配 + 拷贝 + 消费 + 立即释放（热页稳态）
        // 必须用 range 构造函数 —— 与改造前的生产代码完全一致。
        // 注意 std::vector<uint8_t> v(n) 是值初始化（先零填充再拷贝，双倍写），
        // 用它会得出约 2 倍虚高的分配开销。
        {
            const auto t0 = Clock::now();
            for (int i = 0; i < N; ++i) {
                std::vector<uint8_t> v(payload.data(), payload.data() + PAYLOAD);
                consume(v.data(), PAYLOAD);
            }
            x_ms = std::min(x_ms, ms_since(t0));
        }

        // Y：预分配槽位 + 拷贝 + 消费（零分配）
        {
            std::vector<uint8_t> slot(PAYLOAD);
            const auto t0 = Clock::now();
            for (int i = 0; i < N; ++i) {
                std::memcpy(slot.data(), payload.data(), PAYLOAD);
                consume(slot.data(), PAYLOAD);
            }
            y_ms = std::min(y_ms, ms_since(t0));
        }

        // Z：分配 + 拷贝 + 消费 + 全部持有（冷页 + 队列驻留）
        {
            std::vector<std::vector<uint8_t>> keep;
            keep.reserve(N);
            const auto t0 = Clock::now();
            for (int i = 0; i < N; ++i) {
                keep.emplace_back(payload.data(), payload.data() + PAYLOAD);
                consume(keep.back().data(), PAYLOAD);
            }
            z_ms = std::min(z_ms, ms_since(t0));
        }
    }

    qInfo("%s", "");
    qInfo("==== primitive breakdown (%.0f MB, %d rounds, best-of) ====",
          TOTAL / 1e6, ROUNDS);
    qInfo("  X 分配+拷贝+消费+立即释放（热页） : %8.2f ms  %8.0f MB/s",
          x_ms, mbps(TOTAL, x_ms));
    qInfo("  Y 预分配槽位+拷贝+消费（无分配）  : %8.2f ms  %8.0f MB/s",
          y_ms, mbps(TOTAL, y_ms));
    qInfo("  Z 分配+拷贝+消费+全部持有（冷页） : %8.2f ms  %8.0f MB/s",
          z_ms, mbps(TOTAL, z_ms));
    qInfo("  ---- 因果分解（每 4MB payload）----");
    qInfo("  纯 memcpy 成本      (Y)     : %8.0f us/payload  (%.1f%%)",
          y_ms * 1000.0 / N, y_ms / x_ms * 100.0);
    qInfo("  分配成本            (Z-Y)   : %8.0f us/payload  (%.1f%%)",
          (z_ms - y_ms) * 1000.0 / N, (z_ms - y_ms) / x_ms * 100.0);
    qInfo("  回收/归还 OS 成本   (X-Z)   : %8.0f us/payload  (%.1f%%)",
          (x_ms - z_ms) * 1000.0 / N, (x_ms - z_ms) / x_ms * 100.0);
    qInfo("  合计（=改造前 X）           : %8.0f us/payload", x_ms * 1000.0 / N);
    qInfo("  --> A3 池化可消除 (X-Y)/X = %6.1f%%（分配 + 回收，memcpy 保留）",
          (x_ms - y_ms) / x_ms * 100.0);
    qInfo("  --> memcpy 本身仅占 X 的 %6.1f%% —— 瓶颈是分配，不是拷贝",
          y_ms / x_ms * 100.0);
    qInfo("%s", "");

    QVERIFY(x_ms > 0.0 && y_ms > 0.0 && z_ms > 0.0);
    QVERIFY2(y_ms <= x_ms * 1.30,
             qPrintable(QString("pooled (%1 ms) unexpectedly slower than "
                                "heap (%2 ms)").arg(y_ms).arg(x_ms)));
}

// ---------------------------------------------------------------------------
// 生效门禁：改造前堆分配路径 vs 现状 enqueue()（P2 池化实现）
// ---------------------------------------------------------------------------
void TestFeedStagingCopyCost::test_staging_copy_cost()
{
    const int CH = 16;
    const uint64_t PAYLOAD = 4ULL * 1024 * 1024;
    // N < P2_MAX_SLOTS(64)：避免池耗尽触发反压等待（worker 未启动时池耗尽是丢弃）。
    const int N = 48;
    const uint64_t TOTAL = PAYLOAD * N;

    std::vector<uint8_t> payload =
        CrossFixture::make_payload(PAYLOAD, CH, 0xFEEDULL);

    double legacy_ms = 1e18, now_ms = 1e18, c_ms = 1e18, no_recycle_ms = 1e18;
    uint64_t now_peak_queue_bytes = 0;
    uint32_t pool_capacity = 0, pool_peak_in_use = 0;

    for (int r = 0; r < ROUNDS; ++r) {
        // ---- 改造前基线（稳态回收）----
        // 等价于改造前 diskcache_writer.cpp:140 的
        //   payload.data = std::vector<uint8_t>(data, data + length);
        // 消费侧对称：取走一个 payload 并析构（vector 把页归还 OS）。
        {
            std::deque<std::vector<uint8_t>> q;
            const auto t0 = Clock::now();
            for (int i = 0; i < N; ++i) {
                q.emplace_back(payload.data(), payload.data() + PAYLOAD);
                consume(q.front().data(), PAYLOAD);   // 反死存储消除
                q.pop_front();                        // 消费者取走 → 析构 → 归还 OS
            }
            legacy_ms = std::min(legacy_ms, ms_since(t0));
        }

        // ---- 现状：真实 enqueue()（P2 槽位池实现，稳态回收）----
        // 消费侧对称：模拟 worker 取走 payload 并归还槽位。
        //
        // ★ 关键：池化的全部收益来自**槽位复用**。若消费者缺席（队列只进不出），
        //   每个 payload 仍要新分配一个 4MB 槽位，收益为 0 —— 本测试第一版正是
        //   这样写的，量出 1.03x 的假"无改善"。下面的 no_recycle_ms 把这个反例
        //   显式量化出来，防止后人再踩。
        {
            LogicSnapshotDiskCacheWriter writer(nullptr);  // enqueue 不解引用 _owner

            auto recycle_one = [&writer]() {
                std::lock_guard<std::mutex> lk(writer._async_mutex);
                if (writer._async_queue.empty()) return;
                auto &f = writer._async_queue.front();
                if (f.slot != nullptr) writer._slot_pool.release(f.slot);
                writer._async_queue.pop();
                writer._async_queue_bytes_size -= f.length;
                writer._async_queue_depth = writer._async_queue.size();
            };

            // 预热：配置槽位池并产生 1 个可复用槽位
            writer.enqueue(payload.data(), PAYLOAD, LA_CROSS_DATA);
            recycle_one();

            const auto t0 = Clock::now();
            for (int i = 0; i < N; ++i) {
                writer.enqueue(payload.data(), PAYLOAD, LA_CROSS_DATA);
                recycle_one();
            }
            now_ms = std::min(now_ms, ms_since(t0));

            {
                std::lock_guard<std::mutex> lk(writer._async_mutex);
                pool_capacity = writer._slot_pool.capacity();
                pool_peak_in_use = writer._slot_pool.peak_in_use();
            }
            writer.drain_and_join();
        }

        // ---- 反例参照：消费者缺席（槽位从不归还）----
        // 预期与"改造前基线"接近 —— 证明收益来自复用而非"用了池"这件事本身。
        {
            LogicSnapshotDiskCacheWriter writer(nullptr);
            const auto t0 = Clock::now();
            for (int i = 0; i < N; ++i)
                writer.enqueue(payload.data(), PAYLOAD, LA_CROSS_DATA);
            no_recycle_ms = std::min(no_recycle_ms, ms_since(t0));
            now_peak_queue_bytes = std::max(now_peak_queue_bytes,
                                            writer.get_async_queue_bytes());
            writer.drain_and_join();
        }

        // ---- 零拷贝（A1 理论下界，只入队指针，不触碰样本字节）----
        {
            std::deque<const uint8_t *> ptr_q;
            const auto t0 = Clock::now();
            for (int k = 0; k < C_REPEAT; ++k)
                for (int i = 0; i < N; ++i)
                    ptr_q.push_back(payload.data());
            c_ms = std::min(c_ms, ms_since(t0) / C_REPEAT);
            uint64_t acc = 0;
            for (const uint8_t *p : ptr_q) acc += (uintptr_t)p;
            g_sink += acc;   // 反死存储消除
        }
    }

    const double legacy_mbps = mbps(TOTAL, legacy_ms);
    const double now_mbps = mbps(TOTAL, now_ms);
    const double speedup = now_ms > 0 ? legacy_ms / now_ms : 0.0;
    const double no_recycle_mbps = mbps(TOTAL, no_recycle_ms);

    qInfo("%s", "");
    qInfo("==== P2 staging copy: 改造前 vs 现状 (%d x %.1f MB = %.0f MB, "
          "best-of-%d, 稳态回收) ====", N, PAYLOAD / 1e6, TOTAL / 1e6, ROUNDS);
    qInfo("  改造前  堆分配+memcpy+消费 : %8.2f ms  %8.0f MB/s  (%.0f us/payload)",
          legacy_ms, legacy_mbps, legacy_ms * 1000.0 / N);
    qInfo("  现状    P2 槽位池+memcpy   : %8.2f ms  %8.0f MB/s  (%.0f us/payload)",
          now_ms, now_mbps, now_ms * 1000.0 / N);
    qInfo("  反例    池化但消费者缺席   : %8.2f ms  %8.0f MB/s  (%.0f us/payload)",
          no_recycle_ms, no_recycle_mbps, no_recycle_ms * 1000.0 / N);
    qInfo("  零拷贝  仅入队指针（下界） : %8.3f us/payload（无内存流量）",
          c_ms * 1000.0 / N);
    qInfo("  --> P2 池化加速比 = %.1fx，每 payload 成本下降 %.1f%%",
          speedup, (1.0 - now_ms / legacy_ms) * 100.0);
    qInfo("  --> 反例对照：消费者缺席时加速比仅 %.2fx —— 收益全部来自槽位复用",
          no_recycle_ms > 0 ? legacy_ms / no_recycle_ms : 0.0);
    qInfo("  --> 槽位池实测: capacity=%u peak_in_use=%u "
          "（上限 %u 槽 x %llu MB = %llu MB）",
          pool_capacity, pool_peak_in_use,
          (unsigned)LogicSnapshotDiskCacheWriter::P2_MAX_SLOTS,
          (unsigned long long)(LogicSnapshotDiskCacheWriter::P2_SLOT_BYTES / (1024 * 1024)),
          (unsigned long long)(LogicSnapshotDiskCacheWriter::ASYNC_HIGH_WATERMARK
                               / (1024 * 1024)));
    qInfo("  --> 峰值额外常驻（实测队列）: %.1f MB（改造前设计上限 1073 MB）",
          now_peak_queue_bytes / 1e6);

    // 场景换算：feed_bytes_per_s = ch * samplerate / 8
    qInfo("%s", "");
    qInfo("  ---- 采集场景换算（CROSS 格式，feed = ch x rate / 8）----");
    struct Scenario { const char *name; int ch; double rate; };
    const Scenario scen[] = {
        {"  8ch @ 1 GS/s  ", 8, 1e9},
        {" 16ch @ 500 MS/s", 16, 5e8},
        {" 32ch @ 250 MS/s", 32, 2.5e8},
        {" 16ch @ 1 GS/s  ", 16, 1e9},
    };
    for (const auto &s : scen) {
        const double feed = s.ch * s.rate / 8.0 / 1e6;   // MB/s
        qInfo("%s = %6.0f MB/s  ->  改造前占单核 %5.1f%%  |  现状 %5.1f%%",
              s.name, feed, feed / legacy_mbps * 100.0, feed / now_mbps * 100.0);
    }
    qInfo("%s", "");

    // ---- 生效门禁 ----
    // 实测池化应带来数量级改善；保守设 2x 门槛（远低于实测，防回归用）。
    QVERIFY2(speedup >= 2.0,
             qPrintable(QString("P2 slot pool regressed: only %1x faster than "
                                "the legacy heap path (%2 ms vs %3 ms)")
                            .arg(speedup, 0, 'f', 2).arg(now_ms).arg(legacy_ms)));
    // 槽位池不得为 N 个 payload 分配超过 max_slots 个槽位
    QVERIFY2(pool_capacity <= LogicSnapshotDiskCacheWriter::P2_MAX_SLOTS,
             qPrintable(QString("slot pool over-allocated: capacity=%1 > max=%2")
                            .arg(pool_capacity)
                            .arg(LogicSnapshotDiskCacheWriter::P2_MAX_SLOTS)));
    QVERIFY(pool_peak_in_use <= (uint32_t)N);
}

// ---------------------------------------------------------------------------
// 全链路参考（含异步 worker 转置）
// ---------------------------------------------------------------------------
void TestFeedStagingCopyCost::test_full_pipeline_reference()
{
    const int CH = 16;
    const uint64_t PAYLOAD = 4ULL * 1024 * 1024;
    const int N = 48;
    const uint64_t TOTAL_SAMPLES = 256ULL * 1024 * 1024;
    const uint64_t TOTAL = PAYLOAD * N;

    CrossFixture fx(CH);
    std::vector<uint8_t> payload =
        CrossFixture::make_payload(PAYLOAD, CH, 0xCAFEULL);

    LogicSnapshot snap;
    sr_datafeed_logic l{};
    l.length = PAYLOAD;
    l.data = payload.data();
    l.unitsize = 1;
    l.format = LA_CROSS_DATA;

    const auto t0 = Clock::now();
    snap.first_payload(l, TOTAL_SAMPLES, &fx.nodes[0], true);
    for (int i = 0; i < N; ++i)
        snap.append_payload(l);
    snap.capture_ended();
    const double d_ms = ms_since(t0);
    const double d_mbps = mbps(TOTAL, d_ms);

    qInfo("==== D 全链路（first_payload + %d×append + capture_ended, %dch）====",
          N, CH);
    qInfo("  端到端 : %8.2f ms  %8.0f MB/s（feed 与 worker 并发）", d_ms, d_mbps);
    qInfo("  注：端到端含转置 worker；feed 线程串行成本见上一个用例");
    qInfo("%s", "");

    QVERIFY(d_mbps > 0.0);
}

QTEST_GUILESS_MAIN(TestFeedStagingCopyCost)
#include "test_feed_staging_copy_cost.moc"
