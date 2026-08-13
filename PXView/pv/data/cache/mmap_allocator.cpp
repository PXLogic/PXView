#include "pv/data/cache/mmap_allocator.h"
#include <QDebug>
#include <algorithm>
#include <thread>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include "pv/base/log.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// ASan poison macros — only active when built with -fsanitize=address.
// Placed here (not in a header) because they are used only in clear().
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

namespace {
// 在 worker 线程路径上避免使用 QDir/QDateTime，改用 std::filesystem + std::chrono
// 生成磁盘缓存文件路径，并保证目录存在。
std::string make_cache_file_path(const QString& disk_dir) {
    namespace fs = std::filesystem;
    fs::path dir_path(disk_dir.toStdString());
    std::error_code ec;
    fs::create_directories(dir_path, ec);  // 不抛异常，已存在则忽略

    // 生成时间戳 yyyyMMddHHmmsszzz
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now_c);
#else
    localtime_r(&now_c, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d%H%M%S");
    oss << std::setfill('0') << std::setw(3) << ms.count();

    fs::path file_path = dir_path / ("pxview_mmap_cache_" + oss.str() + ".dat");
    return file_path.string();
}
} // anonymous namespace

namespace pv {
namespace data {

MmapAllocator::MmapAllocator()
    : _base_ptr(nullptr),
      _total_bytes(0)
#ifdef _WIN32
      , _hMap(nullptr), _hFile(INVALID_HANDLE_VALUE)
#else
      , _fd(-1)
#endif
      , _prefault_running(false), _writer_block_seq(0), _prefault_block_seq(0),
      _decommitted_block_seq(0), _is_loop_mode(false),
      _block_size(0), _max_blocks_per_channel(0), _channel_num(0)
{
}

MmapAllocator::~MmapAllocator() {
clear();
// M3 fix: join the file-delete thread to ensure the cache file is actually
// deleted before the allocator is destroyed. Previously this was detached,
// which could leak cache files if the process exited before the detached
// thread completed.
if (_file_delete_thread.joinable())
_file_delete_thread.join();
}

bool MmapAllocator::configure(bool use_disk_file, const QString& disk_dir, uint64_t total_bytes,
                              uint64_t block_size, uint64_t max_blocks_per_channel, int channel_num) {
    std::lock_guard<std::mutex> lock(_mutex);
    clear();

    if (total_bytes == 0) return false;
    _total_bytes = total_bytes;
    _block_size = block_size;
    _max_blocks_per_channel = max_blocks_per_channel;
    _channel_num = channel_num;

#ifdef _WIN32
    if (use_disk_file && !disk_dir.isEmpty()) {
        _file_path = QString::fromStdString(make_cache_file_path(disk_dir));

        _hFile = CreateFileA(_file_path.toUtf8().constData(),
                             GENERIC_READ | GENERIC_WRITE,
                             0, // No sharing
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);

        if (_hFile == INVALID_HANDLE_VALUE) {
            pxv_err("MmapAllocator: Failed to create disk cache file %s, error %lu",
                    _file_path.toUtf8().constData(), GetLastError());
            return false;
        }
        // Disk-file-backed mapping: commit all pages (file grows on write).
        _hMap = CreateFileMappingA(_hFile,
                                   nullptr,
                                   PAGE_READWRITE,
                                   (DWORD)(_total_bytes >> 32),
                                   (DWORD)(_total_bytes & 0xFFFFFFFF),
                                   nullptr);
    } else {
        _hFile = INVALID_HANDLE_VALUE; // Page file backed
        // CRITICAL: Use SEC_RESERVE for anonymous mappings so Windows only
        // reserves virtual address space WITHOUT charging the system commit
        // limit for the full size. Without SEC_RESERVE, CreateFileMapping
        // charges the entire total_bytes against the commit limit upfront,
        // causing error 1450 for large ring buffers (e.g. 44GB). Pages are
        // committed on demand via VirtualAlloc(MEM_COMMIT) in prefault_worker
        // and get_block_data, matching Linux mmap(MAP_ANONYMOUS) behavior.
        // RLE compression (calc_mipmap frees blocks with no toggles) means
        // actual physical memory usage is far less than total_bytes.
        _hMap = CreateFileMappingA(INVALID_HANDLE_VALUE,
                                   nullptr,
                                   PAGE_READWRITE | SEC_RESERVE,
                                   (DWORD)(_total_bytes >> 32),
                                   (DWORD)(_total_bytes & 0xFFFFFFFF),
                                   nullptr);
    }

    if (!_hMap) {
        pxv_err("MmapAllocator: CreateFileMapping failed, error %lu", GetLastError());
        if (_hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(_hFile);
            _hFile = INVALID_HANDLE_VALUE;
        }
        return false;
    }

    _base_ptr = MapViewOfFile(_hMap, FILE_MAP_ALL_ACCESS, 0, 0, _total_bytes);
    if (!_base_ptr) {
        pxv_err("MmapAllocator: MapViewOfFile failed, error %lu", GetLastError());
        CloseHandle(_hMap);
        _hMap = nullptr;
        if (_hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(_hFile);
            _hFile = INVALID_HANDLE_VALUE;
        }
        return false;
    }
#else
    if (use_disk_file && !disk_dir.isEmpty()) {
        _file_path = QString::fromStdString(make_cache_file_path(disk_dir));
        _fd = open(_file_path.toUtf8().constData(), O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (_fd < 0) {
            pxv_err("MmapAllocator: Failed to open disk cache file");
            return false;
        }
        if (ftruncate(_fd, _total_bytes) < 0) {
            pxv_err("MmapAllocator: ftruncate failed");
            close(_fd);
            _fd = -1;
            return false;
        }
        _base_ptr = mmap(nullptr, _total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
    } else {
        _base_ptr = mmap(nullptr, _total_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    
    if (_base_ptr == MAP_FAILED) {
        pxv_err("MmapAllocator: mmap failed");
        _base_ptr = nullptr;
        if (_fd >= 0) {
            close(_fd);
            _fd = -1;
        }
        return false;
    }
#endif

    pxv_info("MmapAllocator: Configured successfully, %llu bytes mapped at %p",
             (unsigned long long)_total_bytes, _base_ptr);
    start_prefault();
    return true;
}

void* MmapAllocator::get_block_data(int channel, uint64_t block_index, uint64_t max_blocks_per_channel, uint64_t block_size) {
    if (!_base_ptr) return nullptr;
    
    if (max_blocks_per_channel == 0) return nullptr;
    uint64_t wrapped_block_index = block_index % max_blocks_per_channel;
    
    uint64_t global_offset = ((uint64_t)channel * max_blocks_per_channel + wrapped_block_index) * block_size;
    if (global_offset + block_size > _total_bytes) {
        pxv_err("MmapAllocator: Out of bounds access! offset %llu > total %llu", 
                (unsigned long long)(global_offset + block_size), (unsigned long long)_total_bytes);
        return nullptr;
    }
    
    void *ptr = (uint8_t*)_base_ptr + global_offset;

#ifdef _WIN32
    // For SEC_RESERVE mappings (anonymous / page-file-backed), pages are
    // reserved but NOT committed. Commit the block on demand so the caller
    // can safely write to it. VirtualAlloc(MEM_COMMIT) on already-committed
    // pages is a no-op, so this is safe if prefault already committed it.
    // For disk-file-backed mappings (no SEC_RESERVE), pages are already
    // committed by the file mapping, so skip this.
    if (_hFile == INVALID_HANDLE_VALUE && _hMap) {
        void *committed = VirtualAlloc(ptr, block_size, MEM_COMMIT, PAGE_READWRITE);
        if (!committed) {
            // MEM_COMMIT failed: system is out of physical memory / commit
            // limit. Return nullptr so the caller can set _memory_failed
            // and the user gets a dialog instead of a silent crash.
            pxv_err("MmapAllocator: VirtualAlloc(MEM_COMMIT) failed for "
                    "block (ch=%d, idx=%llu), error %lu — system memory exhausted",
                    channel, (unsigned long long)block_index, GetLastError());
            return nullptr;
        }
    }
#endif

    return ptr;
}

bool MmapAllocator::decommit_block(void* ptr, uint64_t size) {
    if (!ptr || !_base_ptr || !is_mmap_address(ptr)) return false;

#ifdef _WIN32
    if (_hFile == INVALID_HANDLE_VALUE && _hMap) {
        // SEC_RESERVE anonymous mapping: decommit physical pages back to OS.
        // The virtual address range stays reserved, so get_block_data can
        // re-commit it later via VirtualAlloc(MEM_COMMIT).
        // VirtualFree(MEM_DECOMMIT) returns pages to the system pool.
        //
        // Return false: section-view pages may retain content after decommit;
        // the caller (push_to_free_list) must keep the written flag so the
        // allocator explicitly memsets on reuse rather than relying on OS
        // zero-fill (which is NOT guaranteed for SEC_RESERVE section views).
        if (!VirtualFree(ptr, size, MEM_DECOMMIT)) {
            pxv_warn("MmapAllocator: VirtualFree(MEM_DECOMMIT) failed, error %lu (non-fatal)",
                     GetLastError());
        }
    } else {
        // Disk-file-backed mapping: VirtualUnlock triggers decommit,
        // dirty page writes back to file, RAM page is reclaimed.
        // Return false: file-backed pages may retain content after decommit.
        (void)VirtualUnlock(ptr, size);
    }
    return false;
#else
    // RAM pages: madvise MADV_DONTNEED releases pages; subsequent reads
    // return zero (anonymous mapping). Return true: the OS guarantees
    // zero-fill, so the caller can safely skip memset on reuse.
    if (madvise(ptr, size, MADV_DONTNEED) != 0) {
        pxv_warn("MmapAllocator: madvise MADV_DONTNEED failed, errno %d (non-fatal)", errno);
    }
    return true;
#endif
}

bool MmapAllocator::block_absolute_slot(void* ptr, uint64_t block_size, uint64_t& slot) const {
    if (!ptr || !_base_ptr || block_size == 0) return false;
    if (!is_mmap_address(ptr)) return false;
    slot = (uint64_t)((uint8_t*)ptr - (uint8_t*)_base_ptr) / block_size;
    return true;
}

void MmapAllocator::clear() {
    // Stop the background prefault thread BEFORE touching _base_ptr —
    // prefault_worker writes to _base_ptr and must not race with unmap.
    stop_prefault();

    // Take the file path into a local variable before clearing the member,
    // so the background delete thread never touches object state.
    const QString file_to_delete = _file_path;

#if defined(__SANITIZE_ADDRESS__)
    // Poison the mmap region before unmapping so ASan can detect
    // use-after-munmap: any access through a cached pointer after clear()
    // will trigger an ASan error instead of silent stale-data reads.
    if (_base_ptr && _total_bytes > 0) {
        ASAN_POISON_MEMORY_REGION(_base_ptr, _total_bytes);
    }
#endif
#ifdef _WIN32
    if (_base_ptr) {
        UnmapViewOfFile(_base_ptr);
        _base_ptr = nullptr;
    }
    if (_hMap) {
        CloseHandle(_hMap);
        _hMap = nullptr;
    }
    if (_hFile != INVALID_HANDLE_VALUE) {
        CloseHandle(_hFile);
        _hFile = INVALID_HANDLE_VALUE;
    }
#else
    if (_base_ptr && _base_ptr != MAP_FAILED) {
        munmap(_base_ptr, _total_bytes);
        _base_ptr = nullptr;
    }
    if (_fd >= 0) {
        close(_fd);
        _fd = -1;
    }
#endif
    _total_bytes = 0;
    _file_path.clear();

    // Delete the on-disk cache file in the background. Large cache files
    // (up to several GB / 16 GB by default) can take seconds to tens of
    // seconds to delete, and clear() may be called from LogicSnapshot
    // while holding its mutex, which would otherwise block feed/decode/UI
    // threads. The handles above are already closed synchronously (fast),
    // so the detached thread only needs to remove the file by path.
    if (!file_to_delete.isEmpty()) {
        // 在主线程把 QString 转换为 std::string，避免 detached 线程触碰任何 Qt API。
        const std::string path_to_delete = file_to_delete.toStdString();
        // M3 fix: join any previous file-delete thread before starting a new one.
        if (_file_delete_thread.joinable())
            _file_delete_thread.join();
        // M3 fix: store the thread instead of detaching so the destructor
        // can join it, preventing cache file leaks on process exit.
        _file_delete_thread = std::thread([path_to_delete]() {
            namespace fs = std::filesystem;
            std::error_code ec;
            // 用带 error_code 的重载避免抛异常。
            if (!fs::exists(path_to_delete, ec)) {
                return;
            }
            if (fs::remove(path_to_delete, ec)) {
                pxv_info("MmapAllocator: Background-deleted cache file %s",
                         path_to_delete.c_str());
            } else {
                pxv_err("MmapAllocator: Failed to background-delete cache file %s (ec=%d: %s)",
                        path_to_delete.c_str(), ec.value(), ec.message().c_str());
            }
        });
    }
}

void MmapAllocator::prefault_worker() {
    pxv_info("MmapAllocator: prefault thread started, total_blocks=%llu, channels=%d, ahead=%llu blocks",
             (unsigned long long)_max_blocks_per_channel, _channel_num,
             (unsigned long long)PREFAULT_AHEAD_BLOCKS);

    uint64_t block_seq = 0;
    while (_prefault_running.load() && block_seq < _max_blocks_per_channel) {
        // Check ahead distance: if we're far enough ahead of the writer, yield
        uint64_t writer_seq = _writer_block_seq.load();
        if (block_seq >= writer_seq && (block_seq - writer_seq) >= PREFAULT_AHEAD_BLOCKS) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Prefault all channels' blocks for this block_seq
        for (int ch = 0; ch < _channel_num; ch++) {
            uint64_t byte_offset = ((uint64_t)ch * _max_blocks_per_channel + block_seq) * _block_size;
            if (byte_offset + _block_size > _total_bytes) break;

            // PulseView-style defensive programming: wrap the page-fault
            // trigger in a SEH/try-catch so that if the mmap mapping has
            // been revoked (e.g. disk file deleted, memory pressure), the
            // thread exits gracefully instead of crashing the process
            // with SIGSEGV. The prefault thread is a background optimization
            // — failure to prefault a block is non-fatal because
            // get_block_data() will VirtualAlloc(MEM_COMMIT) on demand.
            try {
#ifdef _WIN32
            // For SEC_RESERVE anonymous mappings, commit the block before
            // touching any page. For disk-file-backed mappings, pages are
            // already committed — VirtualAlloc(MEM_COMMIT) is a no-op.
            VirtualAlloc((uint8_t*)_base_ptr + byte_offset, _block_size,
                         MEM_COMMIT, PAGE_READWRITE);
#endif

            // Touch each page in the block to trigger the page fault
            for (uint64_t off = 0; off < _block_size; off += PREFAULT_PAGE_SIZE) {
                *(volatile uint8_t*)((uint8_t*)_base_ptr + byte_offset + off) = 0;
            }
            } catch (...) {
                // mmap mapping may have been revoked or the system is out
                // of memory. Log and stop prefaulting — the caller will
                // fall back to on-demand commit via get_block_data().
                pxv_err("MmapAllocator: prefault failed at block_seq=%llu ch=%d "
                        "— mapping may be invalid, stopping prefault thread",
                        (unsigned long long)block_seq, ch);
                _prefault_running.store(false);
                goto prefault_done;
            }
        }

        block_seq++;
        _prefault_block_seq.store(block_seq);
    }

prefault_done:
    _prefault_running.store(false);
    pxv_info("MmapAllocator: prefault thread finished, prefaulted %llu blocks",
             (unsigned long long)_prefault_block_seq.load());
}

void MmapAllocator::decommit_block_seq_all_channels(uint64_t block_seq) {
    for (int ch = 0; ch < _channel_num; ch++) {
        uint64_t byte_offset = ((uint64_t)ch * _max_blocks_per_channel + block_seq) * _block_size;
        if (byte_offset + _block_size > _total_bytes) break;
        decommit_range(byte_offset, byte_offset + _block_size);
    }
}

void MmapAllocator::start_prefault() {
    _prefault_running.store(true);
    _writer_block_seq.store(0);
    _prefault_block_seq.store(0);
    _decommitted_block_seq.store(0);
    _prefault_thread = std::thread(&MmapAllocator::prefault_worker, this);
}

void MmapAllocator::stop_prefault() {
    _prefault_running.store(false);
    if (_prefault_thread.joinable()) {
        _prefault_thread.join();
    }
}

void MmapAllocator::wait_prefault_initial_blocks(uint64_t block_count) {
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        if (_prefault_block_seq.load() >= block_count) {
            break;
        }
        if (!_prefault_running.load()) {
            break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (elapsed > 2000) {
            pxv_warn("MmapAllocator: wait_prefault_initial_blocks(%llu) timeout after 2s, "
                     "prefaulted only %llu blocks",
                     (unsigned long long)block_count,
                     (unsigned long long)_prefault_block_seq.load());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto t1 = std::chrono::steady_clock::now();
    pxv_info("MmapAllocator: wait_prefault_initial_blocks(%llu) done in %lldms, "
             "prefaulted %llu blocks",
             (unsigned long long)block_count,
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
             (unsigned long long)_prefault_block_seq.load());
}

void MmapAllocator::notify_writer_block_seq(uint64_t block_seq) {
    _writer_block_seq.store(block_seq);
}

void MmapAllocator::set_loop_mode(bool is_loop) {
    _is_loop_mode.store(is_loop);
}

void MmapAllocator::decommit_range(uint64_t start_bytes, uint64_t end_bytes) {
    if (!_base_ptr || start_bytes >= end_bytes || end_bytes > _total_bytes) return;

    // Align to page boundaries.
    uint64_t start = (start_bytes + PREFAULT_PAGE_SIZE - 1) & ~(PREFAULT_PAGE_SIZE - 1);
    uint64_t end = end_bytes & ~(PREFAULT_PAGE_SIZE - 1);

    for (uint64_t off = start; off < end; off += PREFAULT_PAGE_SIZE) {
        void* page = (uint8_t*)_base_ptr + off;
#ifdef _WIN32
        if (_hFile == INVALID_HANDLE_VALUE && _hMap) {
            // SEC_RESERVE anonymous mapping: do NOT call VirtualFree(MEM_DECOMMIT)
            // here. Section-view decommit is unreliable for SEC_RESERVE mappings;
            // decommit_block() handles per-block decommit and returns false to
            // signal the caller to memset on reuse. Skipping here avoids the
            // problematic bulk decommit entirely.
        } else {
            // Disk-file-backed mapping: VirtualUnlock triggers decommit.
            (void)VirtualUnlock(page, PREFAULT_PAGE_SIZE);
        }
#else
        if (madvise(page, PREFAULT_PAGE_SIZE, MADV_DONTNEED) != 0) {
            // Non-fatal: ignore madvise failure.
        }
#endif
    }
}

} // namespace data
} // namespace pv
