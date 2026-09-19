/*
 * test_mmap_region_lifetime.cpp — P1-b mmap 区域生命周期契约回归测试
 *
 * 锁定的契约
 * ----------
 * P1-b 把 mmap 区域从 LogicSnapshot 的**私有成员**提升为可被读者钉住的
 * **引用计数句柄**（LogicSnapshot::mmap_region()）。核心保证：
 *
 *   1) 读者持有的 shared_ptr 拷贝，会让映射在最后一个读者释放前不被解除
 *      （munmap / UnmapViewOfFile）。
 *   2) 因此 first_payload 在几何变更时可以安全地重建 MmapAllocator ——
 *      旧映射不会在仍有读者的瞬间消失（历史 SIGSEGV 场景：
 *      解码线程的 di->inbuf 悬垂 → term_matches 段错误）。
 *   3) 几何一致时复用同一个 allocator 对象；几何不一致时重建。
 *      这条收紧很重要：free_data() 在迭代器活跃时会提前返回（不 reset），
 *      此时若通道数/深度已变，旧代码会带旧几何进复用分支，
 *      allocate_block() 按错误的 _max_blocks_per_channel 计算 mmap 槽位。
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
    std::vector<uint8_t> payload;

    Fixture(int ch_count, uint64_t payload_bytes, uint64_t seed)
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
        payload_bytes -= payload_bytes % chunk;
        payload.resize(payload_bytes);
        std::mt19937_64 rng(seed);
        for (auto &b : payload) b = (uint8_t)(rng() & 0xFF);
    }

    sr_datafeed_logic logic()
    {
        sr_datafeed_logic l{};
        l.length = payload.size();
        l.data = payload.data();
        l.unitsize = 1;
        l.format = LA_CROSS_DATA;
        return l;
    }
};

// want_blocks = (total_sample_count / LeafBlockSamples) + 16
// total_bytes = want_blocks * LeafBlockSpace * channel_num
uint64_t expect_bytes(uint64_t total_samples, uint64_t ch)
{
    const uint64_t blocks =
        (total_samples / LogicSnapshot::LeafBlockSamples) + 16;
    return blocks * LogicSnapshot::LeafBlockSpace * ch;
}

} // anonymous namespace

class TestMmapRegionLifetime : public QObject
{
    Q_OBJECT

private slots:
    void test_region_exposed_and_refcounted();
    void test_pin_survives_recreate();
    void test_same_geometry_reuses_region();
};

// ---------------------------------------------------------------------------
// 契约 1：句柄被暴露，且引用计数确实随读者增减
// ---------------------------------------------------------------------------
void TestMmapRegionLifetime::test_region_exposed_and_refcounted()
{
    Fixture fx(4, 64 * 1024, 0xA11C);
    LogicSnapshot snap;
    sr_datafeed_logic l = fx.logic();
    snap.first_payload(l, 1000000ULL, &fx.nodes[0], true);
    snap.capture_ended();

    auto r = snap.mmap_region();
    QVERIFY2(r != nullptr, "mmap_region() must expose the configured allocator");
    QCOMPARE(r->get_total_bytes(), expect_bytes(1000000ULL, 4));

    // 读者持有拷贝期间，快照侧仍然看得到同一个对象
    QCOMPARE(snap.mmap_region().get(), r.get());
    QVERIFY(snap.mmap_region().use_count() >= 2);

    // 释放读者引用后回到 1（只剩快照自己）
    const long before = snap.mmap_region().use_count();
    QVERIFY(before >= 2);
    r.reset();
    QCOMPARE(snap.mmap_region().use_count(), before - 1);
}

// ---------------------------------------------------------------------------
// 契约 2：几何变更时重建 allocator，但被钉住的旧映射不会消失
//         （这正是历史 SIGSEGV 场景的机制性修复）
// ---------------------------------------------------------------------------
void TestMmapRegionLifetime::test_pin_survives_recreate()
{
    Fixture fx4(4, 64 * 1024, 0xB0B0);
    Fixture fx2(2, 64 * 1024, 0xC0C0);

    LogicSnapshot snap;
    sr_datafeed_logic l4 = fx4.logic();
    snap.first_payload(l4, 1000000ULL, &fx4.nodes[0], true);
    snap.capture_ended();

    // 模拟解码线程：钉住本代区域
    auto pinned = snap.mmap_region();
    QVERIFY(pinned != nullptr);
    const uint64_t old_bytes = pinned->get_total_bytes();
    QCOMPARE(old_bytes, expect_bytes(1000000ULL, 4));

    // 几何变更（通道数 4 -> 2，同时改变 total_sample_count 以触发重建路径）
    sr_datafeed_logic l2 = fx2.logic();
    snap.first_payload(l2, 2000000ULL, &fx2.nodes[0], true);
    snap.capture_ended();

    auto fresh = snap.mmap_region();
    QVERIFY2(fresh != nullptr, "must have a region after geometry change");

    // 关键断言 1：确实重建了（不是拿旧几何的 allocator 复用）
    QVERIFY2(fresh.get() != pinned.get(),
             "geometry change must create a NEW MmapAllocator, not reuse the "
             "old geometry's slot mapping");

    // 关键断言 2：被钉住的旧映射仍然存活且内容可读（未被 munmap）
    QCOMPARE(pinned->get_total_bytes(), old_bytes);
    QCOMPARE(fresh->get_total_bytes(), expect_bytes(2000000ULL, 2));
    QVERIFY2(pinned->get_total_bytes() != fresh->get_total_bytes(),
             "old and new regions must have different geometry");

    // 释放钉子后，旧映射才真正可被回收
    pinned.reset();
    QVERIFY(snap.mmap_region() != nullptr);
}

// ---------------------------------------------------------------------------
// 契约 3：几何一致时必须复用同一个 allocator 对象（repeat 模式的省内存前提）
// ---------------------------------------------------------------------------
void TestMmapRegionLifetime::test_same_geometry_reuses_region()
{
    Fixture fx(8, 64 * 1024, 0xD00D);
    LogicSnapshot snap;
    sr_datafeed_logic l = fx.logic();

    snap.first_payload(l, 1000000ULL, &fx.nodes[0], true);
    snap.capture_ended();
    auto r1 = snap.mmap_region();
    QVERIFY(r1 != nullptr);

    // 同一几何再来一帧：total_sample_count / channel_num 都不变
    snap.first_payload(l, 1000000ULL, &fx.nodes[0], true);
    snap.capture_ended();
    auto r2 = snap.mmap_region();
    QVERIFY(r2 != nullptr);

    QVERIFY2(r2.get() == r1.get(),
             "identical geometry must reuse the existing MmapAllocator "
             "(repeat-mode memory saving)");
    QCOMPARE(r2->get_total_bytes(), expect_bytes(1000000ULL, 8));
}

QTEST_GUILESS_MAIN(TestMmapRegionLifetime)
#include "test_mmap_region_lifetime.moc"
