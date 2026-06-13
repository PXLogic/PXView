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


#ifndef PXVIEW_PV_DATA_DISK_CACHE_CONFIG_H
#define PXVIEW_PV_DATA_DISK_CACHE_CONFIG_H

#include <string>
#include <cstdint>

namespace pv {
namespace data {

struct DiskCacheConfig
{
    bool enabled = false;
    std::string cache_path;
    uint64_t total_cache_depth_gb = 16;
    uint64_t memory_size_gb = 4;
    uint64_t disk_size_gb = 12;
    uint64_t hot_window_blocks = 0;
    uint64_t read_cache_bytes = 256 * 1024 * 1024;
    uint64_t write_queue_threshold_warn = 64;
    uint64_t write_queue_threshold_stop = 256;
    uint64_t disk_speed_test_bytes = 64 * 1024 * 1024;
    double disk_speed_min_mbps = 200.0;
    double disk_space_min_ratio = 0.1;

    void calculate()
    {
        disk_size_gb = total_cache_depth_gb > memory_size_gb
            ? total_cache_depth_gb - memory_size_gb : 0;
    }
};

} // namespace data
} // namespace pv

#endif
