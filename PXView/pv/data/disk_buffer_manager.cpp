/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */


#include <string.h>
#include <stdlib.h>
#include <algorithm>

#include "disk_buffer_manager.h"
#include "../log.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

// Suppress GCC warn_unused_result for read/write/fread calls where
// we intentionally ignore the return value.
#define IGNORE_RESULT(x) do { if (x) {} } while(0)

using namespace std;

namespace pv {
namespace data {

static const char IndexMagic[4] = {'P', 'X', 'D', 'C'};
static const uint32_t IndexVersion = 1;

DiskBufferManager::DiskBufferManager() :
    _is_open(false),
    _channel_count(0),
    _next_disk_offset(0)
{
}

DiskBufferManager::~DiskBufferManager()
{
    close();
}

bool DiskBufferManager::open(const DiskCacheConfig &config, int channel_count)
{
    lock_guard<mutex> lock(_mutex);

    if (_is_open)
        close();

    _cache_path = config.cache_path;
    _channel_count = channel_count;
    _next_disk_offset = 0;

    if (_cache_path.empty()) {
        pxv_err("DiskBufferManager: cache path is empty");
        return false;
    }

    if (_cache_path.back() != '/' && _cache_path.back() != '\\')
        _cache_path += "/";

    MKDIR(_cache_path.c_str());

    _channel_indexes.resize(_channel_count);
    for (int i = 0; i < _channel_count; i++) {
        _channel_indexes[i].block_count = 0;
        _channel_indexes[i].entries.clear();
    }

#ifdef _WIN32
    _channel_handles.resize(_channel_count, INVALID_HANDLE_VALUE);
#else
    _channel_fds.resize(_channel_count, -1);
#endif

    for (int i = 0; i < _channel_count; i++) {
        if (!create_channel_file(i)) {
            pxv_err("DiskBufferManager: failed to create channel file %d", i);
            close();
            return false;
        }
    }

    _is_open = true;
    pxv_info("DiskBufferManager: opened %d channels at %s", _channel_count, _cache_path.c_str());
    return true;
}

void DiskBufferManager::close()
{
    lock_guard<mutex> lock(_mutex);

    if (!_is_open)
        return;

    for (int i = 0; i < _channel_count; i++)
        close_channel_file(i);

#ifdef _WIN32
    _channel_handles.clear();
#else
    _channel_fds.clear();
#endif

    _channel_indexes.clear();
    _is_open = false;
    pxv_info("DiskBufferManager: closed");
}

bool DiskBufferManager::write_block(int channel, uint64_t block_index, const void *data, uint64_t size)
{
    lock_guard<mutex> lock(_mutex);

    if (!_is_open || channel < 0 || channel >= _channel_count)
        return false;

    ChannelIndex &ch_idx = _channel_indexes[channel];

    if (block_index >= ch_idx.block_count) {
        ch_idx.block_count = block_index + 1;
        ch_idx.entries.resize(ch_idx.block_count);
    }

    BlockIndexEntry &entry = ch_idx.entries[block_index];

    if (entry.block_state == BlockState_Valid) {
        entry.block_state = BlockState_Overwritten;
    }

    uint64_t offset = _next_disk_offset;
    _next_disk_offset += size;

    if (!write_file(channel, offset, data, size)) {
        pxv_err("DiskBufferManager: write_block failed ch=%d blk=%llu", channel, (unsigned long long)block_index);
        return false;
    }

    entry.disk_offset = offset;
    entry.block_state = BlockState_Valid;

    return true;
}

bool DiskBufferManager::read_block(int channel, uint64_t block_index, void *data, uint64_t size)
{
    lock_guard<mutex> lock(_mutex);

    if (!_is_open || channel < 0 || channel >= _channel_count)
        return false;

    ChannelIndex &ch_idx = _channel_indexes[channel];

    if (block_index >= ch_idx.block_count)
        return false;

    BlockIndexEntry &entry = ch_idx.entries[block_index];

    if (entry.block_state != BlockState_Valid)
        return false;

    return read_file(channel, entry.disk_offset, data, size);
}

bool DiskBufferManager::save_index()
{
    lock_guard<mutex> lock(_mutex);

    if (!_is_open)
        return false;

    string filename = get_index_filename();

#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename.c_str(), GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        pxv_err("DiskBufferManager: failed to create index file");
        return false;
    }

    DWORD written = 0;
    WriteFile(hFile, IndexMagic, 4, &written, NULL);
    uint32_t version = IndexVersion;
    WriteFile(hFile, &version, sizeof(uint32_t), &written, NULL);
    uint32_t ch_count = (uint32_t)_channel_count;
    WriteFile(hFile, &ch_count, sizeof(uint32_t), &written, NULL);

    for (int i = 0; i < _channel_count; i++) {
        ChannelIndex &ch_idx = _channel_indexes[i];
        uint64_t bc = ch_idx.block_count;
        WriteFile(hFile, &bc, sizeof(uint64_t), &written, NULL);
        for (uint64_t j = 0; j < ch_idx.entries.size(); j++) {
            WriteFile(hFile, &ch_idx.entries[j].disk_offset, sizeof(uint64_t), &written, NULL);
            WriteFile(hFile, &ch_idx.entries[j].block_state, sizeof(uint32_t), &written, NULL);
        }
    }

    CloseHandle(hFile);
#else
    int fd = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pxv_err("DiskBufferManager: failed to create index file");
        return false;
    }

    IGNORE_RESULT(::write(fd, IndexMagic, 4));
    uint32_t version = IndexVersion;
    IGNORE_RESULT(::write(fd, &version, sizeof(uint32_t)));
    uint32_t ch_count = (uint32_t)_channel_count;
    IGNORE_RESULT(::write(fd, &ch_count, sizeof(uint32_t)));

    for (int i = 0; i < _channel_count; i++) {
        ChannelIndex &ch_idx = _channel_indexes[i];
        uint64_t bc = ch_idx.block_count;
        IGNORE_RESULT(::write(fd, &bc, sizeof(uint64_t)));
        for (uint64_t j = 0; j < ch_idx.entries.size(); j++) {
            IGNORE_RESULT(::write(fd, &ch_idx.entries[j].disk_offset, sizeof(uint64_t)));
            IGNORE_RESULT(::write(fd, &ch_idx.entries[j].block_state, sizeof(uint32_t)));
        }
    }

    ::close(fd);
#endif

    pxv_info("DiskBufferManager: index saved");
    return true;
}

bool DiskBufferManager::load_index()
{
    lock_guard<mutex> lock(_mutex);

    if (!_is_open)
        return false;

    string filename = get_index_filename();

#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        pxv_err("DiskBufferManager: index file not found");
        return false;
    }

    DWORD bytesRead = 0;
    char magic[4];
    ReadFile(hFile, magic, 4, &bytesRead, NULL);
    if (memcmp(magic, IndexMagic, 4) != 0) {
        CloseHandle(hFile);
        pxv_err("DiskBufferManager: invalid index magic");
        return false;
    }

    uint32_t version;
    ReadFile(hFile, &version, sizeof(uint32_t), &bytesRead, NULL);
    if (version != IndexVersion) {
        CloseHandle(hFile);
        pxv_err("DiskBufferManager: unsupported index version %u", version);
        return false;
    }

    uint32_t ch_count;
    ReadFile(hFile, &ch_count, sizeof(uint32_t), &bytesRead, NULL);
    if ((int)ch_count != _channel_count) {
        CloseHandle(hFile);
        pxv_err("DiskBufferManager: index channel count mismatch");
        return false;
    }

    _next_disk_offset = 0;
    for (uint32_t i = 0; i < ch_count; i++) {
        ChannelIndex &ch_idx = _channel_indexes[i];
        ReadFile(hFile, &ch_idx.block_count, sizeof(uint64_t), &bytesRead, NULL);
        ch_idx.entries.resize(ch_idx.block_count);
        for (uint64_t j = 0; j < ch_idx.block_count; j++) {
            ReadFile(hFile, &ch_idx.entries[j].disk_offset, sizeof(uint64_t), &bytesRead, NULL);
            ReadFile(hFile, &ch_idx.entries[j].block_state, sizeof(uint32_t), &bytesRead, NULL);
            if (ch_idx.entries[j].disk_offset + LeafBlockSpace > _next_disk_offset)
                _next_disk_offset = ch_idx.entries[j].disk_offset + LeafBlockSpace;
        }
    }

    CloseHandle(hFile);
#else
    int fd = ::open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        pxv_err("DiskBufferManager: index file not found");
        return false;
    }

    char magic[4];
    IGNORE_RESULT(::read(fd, magic, 4));
    if (memcmp(magic, IndexMagic, 4) != 0) {
        ::close(fd);
        pxv_err("DiskBufferManager: invalid index magic");
        return false;
    }

    uint32_t version;
    IGNORE_RESULT(::read(fd, &version, sizeof(uint32_t)));
    if (version != IndexVersion) {
        ::close(fd);
        pxv_err("DiskBufferManager: unsupported index version %u", version);
        return false;
    }

    uint32_t ch_count;
    IGNORE_RESULT(::read(fd, &ch_count, sizeof(uint32_t)));
    if ((int)ch_count != _channel_count) {
        ::close(fd);
        pxv_err("DiskBufferManager: index channel count mismatch");
        return false;
    }

    _next_disk_offset = 0;
    for (uint32_t i = 0; i < ch_count; i++) {
        ChannelIndex &ch_idx = _channel_indexes[i];
        IGNORE_RESULT(::read(fd, &ch_idx.block_count, sizeof(uint64_t)));
        ch_idx.entries.resize(ch_idx.block_count);
        for (uint64_t j = 0; j < ch_idx.block_count; j++) {
            IGNORE_RESULT(::read(fd, &ch_idx.entries[j].disk_offset, sizeof(uint64_t)));
            IGNORE_RESULT(::read(fd, &ch_idx.entries[j].block_state, sizeof(uint32_t)));
            if (ch_idx.entries[j].disk_offset + LeafBlockSpace > _next_disk_offset)
                _next_disk_offset = ch_idx.entries[j].disk_offset + LeafBlockSpace;
        }
    }

    ::close(fd);
#endif

    pxv_info("DiskBufferManager: index loaded, next_offset=%llu", (unsigned long long)_next_disk_offset);
    return true;
}

void DiskBufferManager::cleanup()
{
    lock_guard<mutex> lock(_mutex);

    for (int i = 0; i < _channel_count; i++)
        close_channel_file(i);

    for (int i = 0; i < _channel_count; i++) {
        string fname = get_channel_filename(i);
#ifdef _WIN32
        DeleteFileA(fname.c_str());
#else
        ::remove(fname.c_str());
#endif
    }

    string idx_name = get_index_filename();
#ifdef _WIN32
    DeleteFileA(idx_name.c_str());
#else
    ::remove(idx_name.c_str());
#endif

    for (int i = 0; i < _channel_count; i++) {
        _channel_indexes[i].block_count = 0;
        _channel_indexes[i].entries.clear();
    }
    _next_disk_offset = 0;

    for (int i = 0; i < _channel_count; i++)
        create_channel_file(i);

    pxv_info("DiskBufferManager: cleanup done");
}

void DiskBufferManager::destroy()
{
    lock_guard<mutex> lock(_mutex);

    for (int i = 0; i < _channel_count; i++)
        close_channel_file(i);

    for (int i = 0; i < _channel_count; i++) {
        string fname = get_channel_filename(i);
#ifdef _WIN32
        DeleteFileA(fname.c_str());
#else
        ::remove(fname.c_str());
#endif
    }

    string idx_name = get_index_filename();
#ifdef _WIN32
    DeleteFileA(idx_name.c_str());
#else
    ::remove(idx_name.c_str());
#endif

    for (int i = 0; i < _channel_count; i++) {
        _channel_indexes[i].block_count = 0;
        _channel_indexes[i].entries.clear();
    }
    _next_disk_offset = 0;

#ifdef _WIN32
    _channel_handles.clear();
#else
    _channel_fds.clear();
#endif
    _channel_indexes.clear();
    _is_open = false;

    pxv_info("DiskBufferManager: destroy done");
}

bool DiskBufferManager::check_disk_space(uint64_t required_bytes)
{
#ifdef _WIN32
    ULARGE_INTEGER free_avail, total, total_free;
    if (!GetDiskFreeSpaceExA(_cache_path.c_str(), &free_avail, &total, &total_free))
        return false;
    return free_avail.QuadPart >= required_bytes;
#else
    struct statvfs stat;
    if (statvfs(_cache_path.c_str(), &stat) != 0)
        return false;
    uint64_t free_bytes = (uint64_t)stat.f_bavail * (uint64_t)stat.f_frsize;
    return free_bytes >= required_bytes;
#endif
}

uint64_t DiskBufferManager::get_disk_offset(int channel, uint64_t block_index)
{
    if (channel < 0 || channel >= _channel_count)
        return 0;

    ChannelIndex &ch_idx = _channel_indexes[channel];
    if (block_index >= ch_idx.entries.size())
        return 0;

    return ch_idx.entries[block_index].disk_offset;
}

void DiskBufferManager::set_channel_block_count(int channel, uint64_t count)
{
    if (channel < 0 || channel >= _channel_count)
        return;

    ChannelIndex &ch_idx = _channel_indexes[channel];
    ch_idx.block_count = count;
    ch_idx.entries.resize(count);
}

bool DiskBufferManager::create_channel_file(int channel)
{
    string filename = get_channel_filename(channel);

#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        pxv_err("DiskBufferManager: failed to create file %s", filename.c_str());
        return false;
    }
    _channel_handles[channel] = hFile;
#else
    int fd = ::open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        pxv_err("DiskBufferManager: failed to create file %s", filename.c_str());
        return false;
    }
    _channel_fds[channel] = fd;
#endif

    return true;
}

bool DiskBufferManager::open_channel_file(int channel)
{
    string filename = get_channel_filename(channel);

#ifdef _WIN32
    HANDLE hFile = CreateFileA(filename.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;
    _channel_handles[channel] = hFile;
#else
    int fd = ::open(filename.c_str(), O_RDWR);
    if (fd < 0)
        return false;
    _channel_fds[channel] = fd;
#endif

    return true;
}

void DiskBufferManager::close_channel_file(int channel)
{
#ifdef _WIN32
    if (channel < (int)_channel_handles.size() && _channel_handles[channel] != INVALID_HANDLE_VALUE) {
        CloseHandle(_channel_handles[channel]);
        _channel_handles[channel] = INVALID_HANDLE_VALUE;
    }
#else
    if (channel < (int)_channel_fds.size() && _channel_fds[channel] >= 0) {
        ::close(_channel_fds[channel]);
        _channel_fds[channel] = -1;
    }
#endif
}

bool DiskBufferManager::write_file(int channel, uint64_t offset, const void *data, uint64_t size)
{
#ifdef _WIN32
    HANDLE hFile = _channel_handles[channel];
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);

    DWORD written = 0;
    BOOL result = WriteFile(hFile, data, (DWORD)size, &written, NULL);
    return result && written == (DWORD)size;
#else
    int fd = _channel_fds[channel];
    if (fd < 0)
        return false;

    ssize_t ret = ::pwrite(fd, data, (size_t)size, (off_t)offset);
    return ret == (ssize_t)size;
#endif
}

bool DiskBufferManager::read_file(int channel, uint64_t offset, void *data, uint64_t size)
{
#ifdef _WIN32
    HANDLE hFile = _channel_handles[channel];
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);

    DWORD bytesRead = 0;
    BOOL result = ReadFile(hFile, data, (DWORD)size, &bytesRead, NULL);
    return result && bytesRead == (DWORD)size;
#else
    int fd = _channel_fds[channel];
    if (fd < 0)
        return false;

    ssize_t ret = ::pread(fd, data, (size_t)size, (off_t)offset);
    return ret == (ssize_t)size;
#endif
}

string DiskBufferManager::get_channel_filename(int channel)
{
    return _cache_path + "ch_" + to_string(channel) + ".bin";
}

string DiskBufferManager::get_index_filename()
{
    return _cache_path + "index.bin";
}

} // namespace data
} // namespace pv
