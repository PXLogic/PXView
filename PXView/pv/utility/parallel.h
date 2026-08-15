/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PXVIEW_PV_UTILITY_PARALLEL_H
#define PXVIEW_PV_UTILITY_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pv {
namespace utility {

/* ── ParallelChannels: 轻量"按通道/索引并行"执行器 ──
 *
 * 用途: 采集热路径中每通道完全独立的工作 (如 RLE 编码的 LA_CROSS_DATA
 * 路径) 可并行加速. 但采集路径不能每次 spawn 线程 (创建开销 ~10-50µs
 * 会吃掉收益), 故用持久 worker 线程池:
 *   - 构造时启动 N-1 个 worker (N = min(hw-1, 8), 主线程额外参与).
 *   - run(n, fn): 将 [0,n) 的索引经原子计数器动态分配给 主线程 + workers,
 *     全部完成后返回. 每次调用零线程创建/销毁.
 *   - 串行退化: n <= 1 或无线程时直接串行执行 (同步开销为 0).
 *
 * 线程安全: run() 通过 exec_mtx_ 串行化 — 多调用方并发调用时第二个会等待
 * 第一个完成, 保证内部状态不被并发破坏. fn 引用在 run 返回前一直有效.
 */
class ParallelChannels
{
public:
    ParallelChannels()
        : stop_(false), active_(false), next_(0), n_(0), done_(0)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        const size_t workers = (hw > 2) ? std::min<size_t>(hw - 1, 8) : 2;
        threads_.reserve(workers);
        for (size_t i = 0; i < workers; ++i)
            threads_.emplace_back([this] { worker_main(); });
    }

    ~ParallelChannels()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_work_.notify_all();
        for (auto& t : threads_)
            if (t.joinable())
                t.join();
        threads_.clear();
    }

    ParallelChannels(const ParallelChannels&) = delete;
    ParallelChannels& operator=(const ParallelChannels&) = delete;

    /* 对 [0,n) 的每个 i 调用 fn(i), 全部完成后返回.
     * fn 必须可并发调用 (对每个 i 写独立输出, 只读共享输入). */
    template <typename Fn>
    void run(size_t n, const Fn& fn)
    {
        std::lock_guard<std::mutex> exec(exec_mtx_);

        if (n <= 1 || threads_.empty()) {
            for (size_t i = 0; i < n; ++i)
                fn(i);
            return;
        }

        /* worker 需要类型擦除的任务句柄. 用 run 栈上的 std::function 中转
         * (SBO 小 lambda 零堆分配), run 阻塞等待全部完成, 期间引用有效. */
        const std::function<void(size_t)> f = fn;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fn_ = &f;
            n_ = n;
            next_.store(0, std::memory_order_relaxed);
            done_ = 0;
            active_ = true;
        }
        cv_work_.notify_all();

        /* 主线程参与: 动态领取索引 */
        for (;;) {
            const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= n)
                break;
            fn(i);
        }

        /* 等待所有 worker 完成 */
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_done_.wait(lk, [this] { return done_ >= threads_.size(); });
            active_ = false;
            fn_ = nullptr;
        }
        /* 唤醒等在"本轮结束"状态下的 worker, 使它们进入下一轮等待 */
        cv_work_.notify_all();
    }

    size_t worker_count() const { return threads_.size(); }

private:
    void worker_main()
    {
        for (;;) {
            std::unique_lock<std::mutex> lk(mtx_);
            /* 阶段1: 等待主线程发起本轮任务 */
            cv_work_.wait(lk, [this] { return stop_ || active_; });
            if (stop_)
                return;
            lk.unlock();

            for (;;) {
                const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
                if (i >= n_)
                    break;
                (*fn_)(i);
            }

            lk.lock();
            done_++;
            if (done_ == threads_.size())
                cv_done_.notify_one();
            /* 阶段2: 等待主线程确认本轮结束 (active_ 置 false) —
             * 防止 worker 在 active_ 仍为 true 时忙循环, 并确保在主线程
             * run() 返回 (fn_ 析构) 前不再触碰 fn_. */
            cv_work_.wait(lk, [this] { return stop_ || !active_; });
            if (stop_)
                return;
        }
    }

    std::mutex exec_mtx_;        /* 串行化 run() 调用 */
    std::mutex mtx_;
    std::condition_variable cv_work_, cv_done_;
    bool stop_ = false;          /* guarded by mtx_ */
    bool active_ = false;        /* guarded by mtx_ */
    const std::function<void(size_t)>* fn_ = nullptr; /* guarded by mtx_ */
    std::atomic<size_t> next_{0};
    std::atomic<size_t> n_{0};
    std::atomic<size_t> done_{0};
    std::vector<std::thread> threads_;
};

/* 全局共享的按通道并行池 (懒初始化, 进程生命周期).
 * 采集编码同一时刻通常只有一个 LogicSnapshot 在跑, 共享池避免多实例
 * 各自起线程; 并发调用由 run() 内部 exec_mtx_ 串行化保护. */
inline ParallelChannels& parallel_channels()
{
    static ParallelChannels pool;
    return pool;
}

} // namespace utility
} // namespace pv

#endif // PXVIEW_PV_UTILITY_PARALLEL_H
