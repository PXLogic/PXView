/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_SESSION_DEVICEMANAGER_H
#define PXVIEW_PV_SESSION_DEVICEMANAGER_H

#include <map>
#include <vector>
#include <QString>
#include "pv/base/pxvdef.h"  // ds_device_handle / NULL_HANDLE

struct sr_dev_inst;

namespace pv {

// Session-Centric 阶段4：设备注册表——从 DeviceAgent 提取的"设备身份"职责。
//
// 唯一职责：驱动扫描缓存、文件（虚拟）设备注册/注销、handle 发放与双向
// 查询。本类不持有"当前活跃设备"概念——那是 DeviceAgent（执行上下文）/
// 未来 CaptureEngine 的职责。所有 handle 的发放与查询必须经由本类，
// 消费方（get_device_list/set_default_device/下拉框）不得自创 handle。
class DeviceManager {
public:
    DeviceManager() = default;
    ~DeviceManager() = default;

    // 非拷贝/非移动：DeviceAgent 按成员持有。
    DeviceManager(const DeviceManager &) = delete;
    DeviceManager &operator=(const DeviceManager &) = delete;

    // --- 扫描缓存（硬件/demo 等驱动扫描结果） ---
    void set_scanned_devices(const std::vector<struct sr_dev_inst *> &sdis)
    {
        _scanned_sdi = sdis;
    }
    const std::vector<struct sr_dev_inst *> &scanned_sdi() const
    {
        return _scanned_sdi;
    }

    // --- 文件（虚拟）设备注册表 ---
    // 注册文件设备并发放稳定 handle：handle = scanned_count + 单调递增序号
    // （关闭不回收）。注册表查找与 _file_sdi 数组位置解耦——此前位置式
    // handle 在 set_device()->release() 擦除活跃 sdi 后错位，导致关闭文件
    // 重开时设备列表选中越界/选错设备。
    ds_device_handle register_file_device(struct sr_dev_inst *sdi);

    // 按 handle 注销文件设备。free_sdi 语义（见 DeviceAgent::remove_device）：
    // 仅当该 sdi 不是当前活跃设备时才在注册表内释放，否则由 release() 负责。
    bool unregister_file_device(ds_device_handle handle, bool free_sdi);

    // release(destroy_file_device=true) 路径：按 sdi 摘除注册并释放。
    bool detach_sdi(struct sr_dev_inst *sdi, bool free_sdi);

    // --- 双向查询（唯一查询点） ---
    struct sr_dev_inst *find_sdi_by_handle(ds_device_handle handle) const;
    ds_device_handle handle_of_sdi(struct sr_dev_inst *sdi) const;

    // 文件设备列表（get_device_list 拼接 all_sdi 用）。
    std::vector<struct sr_dev_inst *> &file_devices() { return _file_sdi; }
    const std::vector<struct sr_dev_inst *> &file_devices() const
    {
        return _file_sdi;
    }

private:
    std::vector<struct sr_dev_inst *> _scanned_sdi;
    std::vector<struct sr_dev_inst *> _file_sdi; // 仅列表用途
    // 稳定 handle 注册表（handle → sdi），权威查询源。
    std::map<ds_device_handle, struct sr_dev_inst *> _file_handles;
    ds_device_handle _next_file_handle = 0; // 单调递增，不复用
};

} // namespace pv

#endif // PXVIEW_PV_SESSION_DEVICEMANAGER_H
