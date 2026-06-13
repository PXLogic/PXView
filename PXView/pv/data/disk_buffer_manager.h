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


#ifndef PXVIEW_PV_DATA_DISK_BUFFER_MANAGER_H
#define PXVIEW_PV_DATA_DISK_BUFFER_MANAGER_H

#include "disk_cache_config.h"
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>
#endif

namespace pv {
namespace data {

static const uint64_t LeafBlockSpace = (64 + 64*64 + 64*64*64 + 64*64*64*64) / 8;

enum BlockState : uint32_t
{
    BlockState_Empty = 0,
    BlockState_Valid = 1,
    BlockState_Overwritten = 2
};

struct BlockIndexEntry
{
    uint64_t disk_offset;
    BlockState block_state;
};

struct ChannelIndex
{
    uint64_t block_count;
    std::vector<BlockIndexEntry> entries;
};

class DiskBufferManager
{
public:
    DiskBufferManager();
    ~DiskBufferManager();

    bool open(const DiskCacheConfig &config, int channel_count);
    void close();

    bool write_block(int channel, uint64_t block_index, const void *data, uint64_t size);
    bool read_block(int channel, uint64_t block_index, void *data, uint64_t size);

    bool save_index();
    bool load_index();

    void cleanup();
    void destroy();

    bool check_disk_space(uint64_t required_bytes);

    uint64_t get_disk_offset(int channel, uint64_t block_index);
    void set_channel_block_count(int channel, uint64_t count);

    bool is_open() const { return _is_open; }
    int channel_count() const { return _channel_count; }
    const std::string& cache_path() const { return _cache_path; }

private:
    bool create_channel_file(int channel);
    bool open_channel_file(int channel);
    void close_channel_file(int channel);

    bool write_file(int channel, uint64_t offset, const void *data, uint64_t size);
    bool read_file(int channel, uint64_t offset, void *data, uint64_t size);

    std::string get_channel_filename(int channel);
    std::string get_index_filename();

private:
    bool _is_open;
    int _channel_count;
    std::string _cache_path;
    std::mutex _mutex;

    std::vector<ChannelIndex> _channel_indexes;
    uint64_t _next_disk_offset;

#ifdef _WIN32
    std::vector<HANDLE> _channel_handles;
#else
    std::vector<int> _channel_fds;
#endif
};

} // namespace data
} // namespace pv

#endif
