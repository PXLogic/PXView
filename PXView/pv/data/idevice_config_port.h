/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2026 PXView contributors
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

#ifndef PXVIEW_PV_DATA_IDEVICE_CONFIG_PORT_H
#define PXVIEW_PV_DATA_IDEVICE_CONFIG_PORT_H

#include <QString>
#include <cstdint>

#include <libsigrok/libsigrok.h>

namespace pv {
namespace data {

/**
 * IDeviceConfigPort — 设备配置写/读端口（依赖倒置）。
 *
 * SignalModel / SignalConfigStore（pxview-data）此前通过
 * SigSession::get_device()（inline，返回具体 DeviceAgent*）反向依赖
 * pxview-session + deviceagent.cpp + libsigrok，导致隔离单测时链接墙
 * （signalmodel.cpp 引用了 DeviceAgent::set_config_* 符号，即使运行时
 * null 守卫，符号仍编译进 TU）。本接口只暴露 data 层真正需要的那一小片
 * 设备配置方法，由 DeviceAgent（session 层）反向实现。data 层从此只依赖
 * 自身层的抽象，写回语义完全不变，Fake 实现仅几十行即可驱动单测。
 *
 * 方法签名必须与 DeviceAgent 精确一致（override 在 DeviceAgent 上声明）。
 * 只添加 SignalModel / SignalConfigStore 实际调用的方法——窄接口原则，
 * 避免把整个 DeviceAgent 虚化（过度设计）。
 */
class IDeviceConfigPort {
public:
    virtual ~IDeviceConfigPort() = default;

    // --- Instance / mode ---
    virtual bool have_instance() const = 0;
    virtual int get_work_mode() = 0;
    virtual void set_work_mode(int mode) = 0;
    virtual bool is_demo() const = 0;
    virtual QString get_demo_operation_mode() = 0;

    // --- Channel enumeration / enable ---
    virtual GSList *get_channels() = 0;
    virtual bool enable_probe(const sr_channel *probe, bool enable) = 0;

    // --- Typed config get/set (subset used by data layer) ---
    virtual bool get_config_string(int key, QString &value,
                                   const sr_channel *ch = nullptr,
                                   const sr_channel_group *cg = nullptr) = 0;
    virtual bool set_config_string(int key, const char *value,
                                   const sr_channel *ch = nullptr,
                                   const sr_channel_group *cg = nullptr) = 0;
    virtual bool get_config_bool(int key, bool &value,
                                 const sr_channel *ch = nullptr,
                                 const sr_channel_group *cg = nullptr) = 0;
    virtual bool set_config_bool(int key, bool value,
                                 const sr_channel *ch = nullptr,
                                 const sr_channel_group *cg = nullptr) = 0;
    virtual bool set_config_uint16(int key, int value,
                                   const sr_channel *ch = nullptr,
                                   const sr_channel_group *cg = nullptr) = 0;
    virtual bool set_config_int32(int key, int value,
                                  const sr_channel *ch = nullptr,
                                  const sr_channel_group *cg = nullptr) = 0;
    virtual bool set_config_uint64(int key, uint64_t value,
                                   const sr_channel *ch = nullptr,
                                   const sr_channel_group *cg = nullptr) = 0;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_IDEVICE_CONFIG_PORT_H
