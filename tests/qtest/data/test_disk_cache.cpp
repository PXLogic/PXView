/*
 * test_disk_cache.cpp — data 层 cache 子系统单元测试
 *
 * 覆盖 PXView/pv/data/cache/ 下 5 个类:
 *   - DiskCacheConfig     纯配置结构体: 默认值 / set-get 往返 / calculate() / 拷贝
 *   - MmapAllocator       RAM 模式 (use_disk_file=false): configure 参数校验 /
 *                         get_block_data 基本往返 / block_absolute_slot /
 *                         is_mmap_address / clear() 复位 (不建真实磁盘文件)
 *   - DiskBufferManager   open 生命周期 / write-read 同步往返 / 通道隔离 /
 *                         越界拒绝 / index 序列化往返 / destroy 清理 (临时目录, 用完删除)
 *   - DiskWriteThread     构造/start/stop/查询 + 入队→on_complete 屏障后的写回往返
 *   - DiskReadCache       构造/空查询 / load 命中 / 缺块 / LRU 逐出回调 / clear
 *
 * 原则: 只测确定性、同步完成的部分。真实磁盘 I/O 均为同步 (write_block/read_block/
 * destroy 无后台线程), 可确定性等待完成, 故用临时目录测试并在结尾清理。
 * DiskWriteThread 的 flush() 只等队列出队(非写完成屏障), 因此写回往返依赖
 * on_complete 回调 + 有界等待 (tiny 同步写, 有界超时内必完成)。
 */

// ── 标准库头必须在其它 PXView 头之前 include ──
#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

// ── xlog stub: cache/*.cpp 经 pv/base/log.h 引用 pxv_log + xlog_* ──
#include "log/xlog.h"
xlog_writer *pxv_log = nullptr;
extern "C" {
int xlog_err(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_warn(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_info(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_dbg(xlog_writer *w, const char *, ...) { (void)w; return 0; }
int xlog_detail(xlog_writer *w, const char *, ...) { (void)w; return 0; }
}

#include "pv/data/cache/disk_cache_config.h"
#include "pv/data/cache/mmap_allocator.h"
#include "pv/data/cache/disk_buffer_manager.h"
#include "pv/data/cache/disk_read_cache.h"
#include "pv/data/cache/disk_write_thread.h"

using namespace pv::data;

namespace {

// 生成唯一临时缓存目录 (父目录必须已存在, _mkdir 只建一层)。
std::string make_unique_cache_dir()
{
    static std::atomic<uint64_t> counter{0};
    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    auto dir = (base / ("pxview_test_cache_" + std::to_string(counter.fetch_add(1)))).string();
    std::filesystem::remove_all(dir, ec); // 清掉上次崩溃残留
    return dir;
}

void remove_cache_dir(const std::string &dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// 有界等待: 轮询谓词直至满足或超时 (返回最终谓词结果)。
bool wait_until(const std::function<bool()> &pred, int timeout_ms = 5000)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// 打开一个 DiskBufferManager 到临时目录的 RAII 夹具。
struct CacheFixture {
    std::string dir;
    DiskCacheConfig cfg;
    DiskBufferManager manager;
    bool ok = false;

    explicit CacheFixture(int channels = 1)
        : dir(make_unique_cache_dir())
    {
        cfg.cache_path = dir;
        ok = manager.open(cfg, channels);
    }

    ~CacheFixture()
    {
        if (manager.is_open())
            manager.destroy();
        remove_cache_dir(dir);
    }
};

} // anonymous namespace

class TestDiskCache : public QObject
{
    Q_OBJECT

private slots:
    // ---- DiskCacheConfig ----
    void configDefaults();
    void configSetRoundtrip();
    void configCalculate();
    void configCopy();

    // ---- MmapAllocator (RAM 模式, 不建磁盘文件) ----
    void mmapUnconfiguredState();
    void mmapConfigureRejectsZeroBytes();
    void mmapRamModeBasics();
    void mmapBlockSlotRoundtrip();
    void mmapOutOfBounds();
    void mmapClearResets();
    void mmapDecommitInvalidReturnsFalse();

    // ---- DiskBufferManager ----
    void dbmInitialState();
    void dbmOpenRejectsEmptyPath();
    void dbmWriteReadRoundtrip();
    void dbmChannelIsolation();
    void dbmBoundaryReads();
    void dbmIndexSaveLoad();
    void dbmDestroyCleansFiles();

    // ---- DiskWriteThread ----
    void dwtLifecycle();
    void dwtSubmitFlushRoundtrip();

    // ---- DiskReadCache ----
    void drcLifecycle();
    void drcLoadAndHit();
    void drcLoadMissingBlock();
    void drcEviction();
    void drcClearInvokesCallback();
    void drcSetMaxSize();
};

// ============================== DiskCacheConfig ==============================

void TestDiskCache::configDefaults()
{
    DiskCacheConfig c;
    QCOMPARE(c.enabled, false);
    QVERIFY(c.cache_path.empty());
    QCOMPARE(c.total_cache_depth_gb, uint64_t(16));
    QCOMPARE(c.memory_size_gb, uint64_t(4));
    QCOMPARE(c.disk_size_gb, uint64_t(12));
    QCOMPARE(c.hot_window_blocks, uint64_t(0));
    QCOMPARE(c.read_cache_bytes, uint64_t(256 * 1024 * 1024));
    QCOMPARE(c.write_queue_threshold_warn, uint64_t(64));
    QCOMPARE(c.write_queue_threshold_stop, uint64_t(256));
    QCOMPARE(c.disk_speed_test_bytes, uint64_t(64 * 1024 * 1024));
    QCOMPARE(c.disk_speed_min_mbps, 200.0);
    QCOMPARE(c.disk_space_min_ratio, 0.1);
}

void TestDiskCache::configSetRoundtrip()
{
    DiskCacheConfig c;
    c.enabled = true;
    c.cache_path = "C:/cache";
    c.total_cache_depth_gb = 100;
    c.memory_size_gb = 20;
    c.disk_size_gb = 80;
    c.hot_window_blocks = 5;
    c.read_cache_bytes = 123;
    c.write_queue_threshold_warn = 7;
    c.write_queue_threshold_stop = 99;
    c.disk_speed_test_bytes = 321;
    c.disk_speed_min_mbps = 1.5;
    c.disk_space_min_ratio = 0.5;

    QCOMPARE(c.enabled, true);
    QVERIFY(c.cache_path == std::string("C:/cache"));
    QCOMPARE(c.total_cache_depth_gb, uint64_t(100));
    QCOMPARE(c.memory_size_gb, uint64_t(20));
    QCOMPARE(c.disk_size_gb, uint64_t(80));
    QCOMPARE(c.hot_window_blocks, uint64_t(5));
    QCOMPARE(c.read_cache_bytes, uint64_t(123));
    QCOMPARE(c.write_queue_threshold_warn, uint64_t(7));
    QCOMPARE(c.write_queue_threshold_stop, uint64_t(99));
    QCOMPARE(c.disk_speed_test_bytes, uint64_t(321));
    QCOMPARE(c.disk_speed_min_mbps, 1.5);
    QCOMPARE(c.disk_space_min_ratio, 0.5);
}

void TestDiskCache::configCalculate()
{
    DiskCacheConfig c;
    c.total_cache_depth_gb = 10;
    c.memory_size_gb = 4;
    c.calculate();
    QCOMPARE(c.disk_size_gb, uint64_t(6));

    // total <= memory → disk 为 0
    c.total_cache_depth_gb = 3;
    c.memory_size_gb = 8;
    c.calculate();
    QCOMPARE(c.disk_size_gb, uint64_t(0));

    // total == memory → disk 为 0
    c.total_cache_depth_gb = 8;
    c.memory_size_gb = 8;
    c.calculate();
    QCOMPARE(c.disk_size_gb, uint64_t(0));

    // 默认值往返
    c.total_cache_depth_gb = 16;
    c.memory_size_gb = 4;
    c.calculate();
    QCOMPARE(c.disk_size_gb, uint64_t(12));
}

void TestDiskCache::configCopy()
{
    DiskCacheConfig a;
    a.enabled = true;
    a.cache_path = "/tmp/cache";
    a.total_cache_depth_gb = 32;
    a.memory_size_gb = 8;
    a.disk_size_gb = 24;
    a.disk_speed_min_mbps = 300.0;

    DiskCacheConfig b(a);
    QCOMPARE(b.enabled, true);
    QVERIFY(b.cache_path == std::string("/tmp/cache"));
    QCOMPARE(b.total_cache_depth_gb, uint64_t(32));
    QCOMPARE(b.memory_size_gb, uint64_t(8));
    QCOMPARE(b.disk_size_gb, uint64_t(24));
    QCOMPARE(b.disk_speed_min_mbps, 300.0);
}

// ============================== MmapAllocator ==============================
// 全部使用 RAM 模式 (use_disk_file=false), 不创建真实磁盘文件。

void TestDiskCache::mmapUnconfiguredState()
{
    MmapAllocator a;
    QCOMPARE(a.get_total_bytes(), uint64_t(0));
    QVERIFY(!a.is_mmap_address(reinterpret_cast<void *>(0x1)));
    QVERIFY(a.get_block_data(0, 0, 16, 4096) == nullptr); // 未 configure → base 空
    uint64_t slot = 999;
    QVERIFY(!a.block_absolute_slot(reinterpret_cast<void *>(0x1), 4096, slot));
}

void TestDiskCache::mmapConfigureRejectsZeroBytes()
{
    MmapAllocator a;
    QVERIFY(!a.configure(false, "", 0, 4096, 16, 1)); // total_bytes == 0 → false
    QCOMPARE(a.get_total_bytes(), uint64_t(0));
}

void TestDiskCache::mmapRamModeBasics()
{
    MmapAllocator a;
    QVERIFY(a.configure(false, "", 1024 * 1024, 4096, 256, 1));
    QCOMPARE(a.get_total_bytes(), uint64_t(1024 * 1024));

    void *p = a.get_block_data(0, 0, 256, 4096);
    QVERIFY(p != nullptr);
    QVERIFY(a.is_mmap_address(p));

    // 末尾块
    void *p2 = a.get_block_data(0, 255, 256, 4096);
    QVERIFY(p2 != nullptr);
    QVERIFY((uint8_t *)p2 == (uint8_t *)p + 255 * 4096);

    // 块索引按 max_blocks_per_channel 回绕 → 回到块 0
    QVERIFY(a.get_block_data(0, 256, 256, 4096) == p);

    // max_blocks_per_channel == 0 → nullptr
    QVERIFY(a.get_block_data(0, 0, 0, 4096) == nullptr);

    // RAM 模式可写可读 (返回的内存可直接读写)
    std::memset(p, 0x5A, 64);
    QCOMPARE(((uint8_t *)p)[0], (uint8_t)0x5A);
    QCOMPARE(((uint8_t *)p)[63], (uint8_t)0x5A);

    a.clear();
}

void TestDiskCache::mmapBlockSlotRoundtrip()
{
    MmapAllocator a;
    QVERIFY(a.configure(false, "", 1024 * 1024, 4096, 256, 1));

    uint64_t slot = 0;
    const std::vector<uint64_t> idxs{0, 1, 7, 255};
    for (uint64_t idx : idxs) {
        void *p = a.get_block_data(0, idx, 256, 4096);
        QVERIFY(p != nullptr);
        QVERIFY(a.block_absolute_slot(p, 4096, slot));
        QCOMPARE(slot, idx);
    }

    a.clear();
}

void TestDiskCache::mmapOutOfBounds()
{
    MmapAllocator a;
    QVERIFY(a.configure(false, "", 1024 * 1024, 4096, 256, 1));

    // channel 1 的块 0 起始偏移 = 1*256*4096 = 1MB → 越界 → nullptr
    QVERIFY(a.get_block_data(1, 0, 256, 4096) == nullptr);
    // block_size 过大导致 offset+size 越界 → nullptr
    QVERIFY(a.get_block_data(0, 255, 256, 8192) == nullptr);

    a.clear();
}

void TestDiskCache::mmapClearResets()
{
    MmapAllocator a;
    QVERIFY(a.configure(false, "", 1024 * 1024, 4096, 256, 1));

    void *p = a.get_block_data(0, 0, 256, 4096);
    QVERIFY(p != nullptr);
    QVERIFY(a.is_mmap_address(p));

    a.clear();
    QCOMPARE(a.get_total_bytes(), uint64_t(0));
    QVERIFY(!a.is_mmap_address(p)); // base 已置空 → false
    uint64_t slot = 999;
    QVERIFY(!a.block_absolute_slot(p, 4096, slot)); // base 已置空 → false
}

void TestDiskCache::mmapDecommitInvalidReturnsFalse()
{
    MmapAllocator a;
    // 未 configure / 空指针 → 恒 false (平台无关)
    QVERIFY(!a.decommit_block(nullptr, 4096));

    QVERIFY(a.configure(false, "", 1024 * 1024, 4096, 256, 1));
    void *p = a.get_block_data(0, 0, 256, 4096);
    QVERIFY(p != nullptr);
    // 有效地址的返回值平台相关 (Windows 恒 false, POSIX 视 madvise),
    // 只验证不崩溃。
    (void)a.decommit_block(p, 4096);
    a.clear();
}

// ============================== DiskBufferManager ==============================

void TestDiskCache::dbmInitialState()
{
    DiskBufferManager m;
    QVERIFY(!m.is_open());
    QCOMPARE(m.channel_count(), 0);
    QVERIFY(m.cache_path().empty());

    char d = 0;
    QVERIFY(!m.write_block(0, 0, &d, 1)); // 未 open → false
    QVERIFY(!m.read_block(0, 0, &d, 1));
}

void TestDiskCache::dbmOpenRejectsEmptyPath()
{
    DiskBufferManager m;
    DiskCacheConfig cfg; // cache_path 为空
    QVERIFY(!m.open(cfg, 1));
    QVERIFY(!m.is_open());
    QVERIFY(m.cache_path().empty());
}

void TestDiskCache::dbmWriteReadRoundtrip()
{
    CacheFixture fx(1);
    QVERIFY(fx.ok);
    QVERIFY(fx.manager.is_open());
    QCOMPARE(fx.manager.channel_count(), 1);

    std::vector<uint8_t> data(128);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (uint8_t)(i * 7 + 1);

    QVERIFY(fx.manager.write_block(0, 0, data.data(), data.size()));
    QCOMPARE(fx.manager.get_disk_offset(0, 0), uint64_t(0));

    std::vector<uint8_t> buf(128, 0);
    QVERIFY(fx.manager.read_block(0, 0, buf.data(), buf.size()));
    QVERIFY(buf == data);
}

void TestDiskCache::dbmChannelIsolation()
{
    CacheFixture fx(2);
    QVERIFY(fx.ok);
    QCOMPARE(fx.manager.channel_count(), 2);

    std::vector<uint8_t> a(64, 0x11);
    std::vector<uint8_t> b(64, 0x22);
    QVERIFY(fx.manager.write_block(0, 0, a.data(), a.size()));
    QVERIFY(fx.manager.write_block(1, 0, b.data(), b.size()));

    // 各通道读到各自数据
    std::vector<uint8_t> buf(64, 0);
    QVERIFY(fx.manager.read_block(0, 0, buf.data(), buf.size()));
    QVERIFY(buf == a);
    QVERIFY(fx.manager.read_block(1, 0, buf.data(), buf.size()));
    QVERIFY(buf == b);

    // 偏移按写入顺序分配 (共享 _next_disk_offset)
    QCOMPARE(fx.manager.get_disk_offset(0, 0), uint64_t(0));
    QCOMPARE(fx.manager.get_disk_offset(1, 0), uint64_t(64));
}

void TestDiskCache::dbmBoundaryReads()
{
    CacheFixture fx(1);
    QVERIFY(fx.ok);

    std::vector<uint8_t> d(64, 0x33);
    std::vector<uint8_t> buf(64, 0);

    // 未写任何块 → 读失败
    QVERIFY(!fx.manager.read_block(0, 0, buf.data(), buf.size()));

    QVERIFY(fx.manager.write_block(0, 0, d.data(), d.size()));
    // 通道越界
    QVERIFY(!fx.manager.read_block(1, 0, buf.data(), buf.size()));
    // 块索引越界
    QVERIFY(!fx.manager.read_block(0, 5, buf.data(), buf.size()));
    // 越界 get_disk_offset → 0
    QCOMPARE(fx.manager.get_disk_offset(1, 0), uint64_t(0));
    QCOMPARE(fx.manager.get_disk_offset(0, 5), uint64_t(0));
}

void TestDiskCache::dbmIndexSaveLoad()
{
    const std::string dir = make_unique_cache_dir();
    {
        DiskCacheConfig cfg;
        cfg.cache_path = dir;
        DiskBufferManager m;
        QVERIFY(m.open(cfg, 1));

        std::vector<uint8_t> d(64, 0x44);
        QVERIFY(m.write_block(0, 0, d.data(), d.size()));
        QVERIFY(m.write_block(0, 1, d.data(), d.size()));
        QVERIFY(m.save_index());
        QCOMPARE(m.get_disk_offset(0, 1), uint64_t(64));

        m.close();
        QVERIFY(!m.is_open());

        // 重开 + load_index: 验证 index 序列化 (magic/version/count/offset/state) 往返
        QVERIFY(m.open(cfg, 1));
        QVERIFY(m.load_index());
        QCOMPARE(m.get_disk_offset(0, 0), uint64_t(0));
        QCOMPARE(m.get_disk_offset(0, 1), uint64_t(64));

        m.destroy();
        QVERIFY(!m.is_open());
    }
    remove_cache_dir(dir);
}

void TestDiskCache::dbmDestroyCleansFiles()
{
    const std::string dir = make_unique_cache_dir();
    {
        DiskCacheConfig cfg;
        cfg.cache_path = dir;
        DiskBufferManager m;
        QVERIFY(m.open(cfg, 2));
        QVERIFY(std::filesystem::exists(dir + "/ch_0.bin"));
        QVERIFY(std::filesystem::exists(dir + "/ch_1.bin"));

        m.destroy();
        QVERIFY(!m.is_open());
        QVERIFY(!std::filesystem::exists(dir + "/ch_0.bin"));
        QVERIFY(!std::filesystem::exists(dir + "/ch_1.bin"));
        QVERIFY(!std::filesystem::exists(dir + "/index.bin"));
    }
    remove_cache_dir(dir);
}

// ============================== DiskWriteThread ==============================

void TestDiskCache::dwtLifecycle()
{
    DiskWriteThread wt(nullptr);
    QCOMPARE(wt.queue_depth(), size_t(0));
    QVERIFY(!wt.is_disk_full());
    QCOMPARE(wt.write_speed_mbps(), 0.0);

    QVERIFY(wt.start());
    QVERIFY(wt.start()); // 幂等
    wt.stop();
    wt.stop(); // 幂等, 不崩
}

void TestDiskCache::dwtSubmitFlushRoundtrip()
{
    CacheFixture fx(1);
    QVERIFY(fx.ok);

    DiskWriteThread wt(&fx.manager);
    QVERIFY(wt.start());

    std::vector<uint8_t> data(128, 0xAB);
    std::atomic<bool> done{false};

    WriteTask task;
    task.channel = 0;
    task.block_index = 0;
    task.data_ptr = data.data();
    task.size = data.size();
    task.on_complete = [&done] { done.store(true); };

    wt.submit(task);

    // 注: flush() 只等队列出队(非写完成屏障), 因此用 on_complete + 有界等待
    // 作为写完成屏障。写入是 tiny 同步写, 超时内必然完成。
    QVERIFY(wait_until([&] { return done.load(); }, 5000));
    QCOMPARE(wt.queue_depth(), size_t(0));

    // 从 manager 同步读回, 验证线程确实把数据写入了磁盘缓存
    std::vector<uint8_t> buf(128, 0);
    QVERIFY(fx.manager.read_block(0, 0, buf.data(), buf.size()));
    QVERIFY(buf == data);

    wt.stop();
}

// ============================== DiskReadCache ==============================

void TestDiskCache::drcLifecycle()
{
    DiskReadCache rc(nullptr); // 无设备也可安全构造
    QCOMPARE(rc.max_size(), uint64_t(256 * 1024 * 1024));
    QVERIFY(rc.lookup(0, 0) == nullptr); // 空缓存 miss
    rc.clear();                          // 空 clear 不崩

    rc.set_max_size(1024);
    QCOMPARE(rc.max_size(), uint64_t(1024));
}

void TestDiskCache::drcLoadAndHit()
{
    CacheFixture fx(1);
    QVERIFY(fx.ok);

    std::vector<uint8_t> data(LeafBlockSpace);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = (uint8_t)(i & 0xFF);
    QVERIFY(fx.manager.write_block(0, 0, data.data(), data.size()));

    DiskReadCache rc(&fx.manager);
    QVERIFY(rc.lookup(0, 0) == nullptr); // 未加载 → miss

    void *p = rc.load(0, 0);
    QVERIFY(p != nullptr);
    QCOMPARE(std::memcmp(p, data.data(), data.size()), 0);

    QVERIFY(rc.lookup(0, 0) == p); // 命中, 同一指针
    QVERIFY(rc.load(0, 0) == p);   // 再次 load 命中同一指针
}

void TestDiskCache::drcLoadMissingBlock()
{
    CacheFixture fx(1);
    QVERIFY(fx.ok);

    DiskReadCache rc(&fx.manager);
    QVERIFY(rc.load(0, 0) == nullptr); // 未写入 → read_block false → nullptr
    QVERIFY(rc.load(3, 0) == nullptr); // 通道越界 → nullptr
}

void TestDiskCache::drcEviction()
{
    CacheFixture fx(1);
    QVERIFY(fx.ok);

    std::vector<uint8_t> d0(LeafBlockSpace, 0x11);
    std::vector<uint8_t> d1(LeafBlockSpace, 0x22);
    QVERIFY(fx.manager.write_block(0, 0, d0.data(), d0.size()));
    QVERIFY(fx.manager.write_block(0, 1, d1.data(), d1.size()));

    // evicted 须先于 rc 声明: rc 析构 (clear) 会回调 lambda, evicted 须存活
    int evicted = 0;
    DiskReadCache rc(&fx.manager);
    rc.set_evict_callback([&evicted](int, uint64_t, void *) { ++evicted; });
    rc.set_max_size(LeafBlockSpace); // 只容纳约 1 块

    void *p0 = rc.load(0, 0);
    QVERIFY(p0 != nullptr);
    QCOMPARE(evicted, 0);

    void *p1 = rc.load(0, 1);
    QVERIFY(p1 != nullptr);
    QCOMPARE(evicted, 1); // 块 0 (LRU 尾) 被逐出
    QVERIFY(rc.lookup(0, 0) == nullptr);
    QVERIFY(rc.lookup(0, 1) == p1);
}

void TestDiskCache::drcClearInvokesCallback()
{
    CacheFixture fx(1);
    QVERIFY(fx.ok);

    std::vector<uint8_t> d0(LeafBlockSpace, 0x55);
    QVERIFY(fx.manager.write_block(0, 0, d0.data(), d0.size()));

    DiskReadCache rc(&fx.manager);
    int evicted = 0;
    rc.set_evict_callback([&evicted](int, uint64_t, void *) { ++evicted; });

    QVERIFY(rc.load(0, 0) != nullptr);
    QCOMPARE(evicted, 0);

    rc.clear();
    QCOMPARE(evicted, 1); // clear 时对残留条目回调
    QVERIFY(rc.lookup(0, 0) == nullptr);
}

void TestDiskCache::drcSetMaxSize()
{
    DiskReadCache rc(nullptr);
    QCOMPARE(rc.max_size(), uint64_t(256 * 1024 * 1024));
    rc.set_max_size(1);
    QCOMPARE(rc.max_size(), uint64_t(1));
    rc.set_max_size(0);
    QCOMPARE(rc.max_size(), uint64_t(0));
}

QTEST_GUILESS_MAIN(TestDiskCache)
#include "test_disk_cache.moc"
