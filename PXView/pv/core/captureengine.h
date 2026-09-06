/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_CORE_CAPTUREENGINE_H
#define PXVIEW_PV_CORE_CAPTUREENGINE_H

#include <mutex>

#include "pv/base/log.h"

namespace pv {
namespace data {
class SessionDocument;
}
namespace core {

class CaptureManager;

// Session-Centric 阶段5：采集执行服务。
//
// 把"提交一次采集"从对 CaptureManager 的直接调用中抽象出来：
//   * GUI（MainWindow/采样栏）、pxviewd（headless）、MCP API 全部经
//     SessionService → CaptureEngine::submit() 消费——按 owner ctx（per-tab
//     SessionDocument 指针）寻址，session_id→ctx 的映射由
//     SessionService/DocumentRegistry 完成，engine 只认 ctx。
//   * libsigrok 是单执行流（同一时刻只能有一个 sr_session_run），因此执行
//     策略默认串行；"多硬件同采"作为策略扩展点预留（ExecutionPolicy）。
//   * 数据通路不变：采集线程经 datafeed 写执行缓冲，RevEndPacket 后零拷贝
//     直达 owner ctx 的快照（阶段3a 的引用语义即"直写"）。
//
// 【阶段7b 裁剪决策】SerialQueue 评估后**不启用**：
//   1. 产品约束为单设备采集，"多硬件同采"动机不存在；
//   2. 采集执行本来已运行在 DeviceAgent 的持久 session worker 线程上
//      （V11: _session_thread + thread-default GMainContext）——"自有执
//      行线程"已成立，engine 队列线程是重复机制；
//   3. 异步化需全链路适配（GUI action_run 同步返回值、MCP 应答改等待事
//      件），无对应收益。
//   串行安全由 SerialInline 路径的 _exec_mutex + CaptureManager 的
//   is_working() 断言共同保障（任意时刻至多一次执行在途）。枚举保留
//   SerialQueue 值仅为文档化该扩展点；若未来产品需要多设备同采，启用前
//   须先完成 GUI/MCP 应答的异步化改造。
struct CaptureIntent {
    data::SessionDocument *owner = nullptr; // 数据落点 ctx（per-tab）
    bool instant = false;                   // 即时/单次采集
};

// 执行策略：
//   SerialInline —— 调用线程立即执行（保持 GUI 同步时序，当前默认）。
//   SerialQueue  —— engine 自有线程串行消费提交队列（预留；启用后 GUI 与
//                   pxviewd 的提交即成为异步请求，结果经事件回投）。
enum class ExecutionPolicy { SerialInline, SerialQueue };

class CaptureEngine {
public:
    explicit CaptureEngine(CaptureManager &mgr) : _mgr(mgr) {}

    // 策略切换（默认 SerialInline）。策略为进程级运行时配置。
    void set_policy(ExecutionPolicy p) { _policy = p; }
    ExecutionPolicy policy() const { return _policy; }

    // 提交采集意图。返回是否成功启动。
    // 线程模型：SerialInline 下由调用线程执行（GUI 主线程 / MCP worker），
    // _exec_mutex 保证任意时刻至多一次执行在途（串行语义的最低保障）；
    // CaptureManager 内部的 is_working 断言依旧是权威守卫。
    bool submit(const CaptureIntent &intent);

    // 停止当前采集（若在跑）。
    bool stop();

private:
    CaptureManager &_mgr;
    ExecutionPolicy _policy = ExecutionPolicy::SerialInline;
    std::mutex _exec_mutex;
};

} // namespace core
} // namespace pv

#endif // PXVIEW_PV_CORE_CAPTUREENGINE_H
