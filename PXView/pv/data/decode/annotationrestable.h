/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <QString>

#include "pv/utility/atomic_shared_ptr.h"

#define DECODER_MAX_DATA_BLOCK_LEN 256
#define CONVERT_STR_MAX_LEN 150

// 注意：AnnotationText / AnnotationResTable 与 DecoderStatus 一样是**全局
// 作用域**类型（annotation.h 中的前向声明也在全局作用域），不要给它们套
// namespace —— 否则 DecoderStatus::m_resTable 会因名字不同而变成不完整类型。

// 显示格式的派生文本槽位数：覆盖 DecoderDataFormat 的全部取值
// (hex=0/dec=1/oct=2/bin=3/ascii=4)。这里刻意用整数字面量而不 include
// pv/base/pxvdef.h —— 该头属于 pxview-core，pv/data 是更底层的叶子库，
// 反向依赖会破坏分层（格式取值只在 .cpp 里用 DecoderDataFormat 校验）。
inline constexpr int kAnnotationFormatSlotCount = 8;

// 不可变的注解文本（所有权拆分，Plan D）。
//
// 职责被收窄成一条：承载"一段注解原文 + 其可选的数值原始 hex"。构造后
// _src_lines/_hex/_is_numeric 不再改变，因此可以被多个线程无锁读取、
// 用引用计数跨线程保活（与 AnnotationHeapPtr / AnnotationSegment::heap_ref
// 同一模式）。
//
// 关键变化：显示格式（bin/oct/dec/hex/ascii）是**视图参数**，不再回写进
// 本对象。旧实现把 cur_display_format / cvt_lines 当"读时缓存"写在共享条目
// 上，导致渲染路径读的时候在写共享状态、且跨线程互相覆盖。
// 现在按 format 惰性派生到 _slots[fmt]，槽位写入一次后永不改写，
// 因此已返回的引用在本对象存活期内始终有效。
class AnnotationText {
public:
    AnnotationText(std::vector<QString> src_lines, QString hex, bool is_numeric);
    ~AnnotationText();

    AnnotationText(const AnnotationText &) = delete;
    AnnotationText &operator=(const AnnotationText &) = delete;

    bool is_numeric() const { return _is_numeric; }

    // 返回该 format 下的显示文本。非数值注解没有格式派生，直接返回原文。
    // 返回的引用在本对象存活期内有效（调用方通过 Annotation 持有 shared_ptr）。
    const std::vector<QString> &lines(int fmt) const;

private:
    // 按 format 的派生文本槽位。整体只在数值注解上分配（见 _slots），
    // 让绝大多数非数值注解不为格式缓存付费。
    struct CvtSlots {
        std::mutex mutex;
        pv::atomic_shared_ptr<const std::vector<QString>>
            slot[kAnnotationFormatSlotCount];
    };

    const std::vector<QString> _src_lines;
    const QString _hex;
    const bool _is_numeric;
    // 构造后指针不再改变（const unique_ptr），槽位内容写入一次后不再改写。
    const std::unique_ptr<CvtSlots> _slots;
};

// 解码线程私有的注解文本去重表。
//
// 所有权契约（Plan D）：本表只保存 weak_ptr，**不再是文本的所有者**。
// 文本的生命期由引用计数决定（Annotation 持 shared_ptr<const AnnotationText>），
// 所以 reset() 只清表、不 delete —— 已发布快照里的注解文本继续存活。
// 这消除了旧实现中 reset() 释放条目、而渲染线程仍按 index 取用导致的
// 悬空读取（decode 重启时 DecoderStatus::clear() 会调用 reset()）。
//
// 线程契约：只允许解码线程访问（唯一的写入侧）。读取侧一律走
// shared_ptr<const AnnotationText>，不再通过本表。
class AnnotationResTable {
public:
    // 去重并返回不可变文本对象：key 相同的调用返回同一个对象。
    std::shared_ptr<const AnnotationText> intern(const std::string &key,
                                                 std::vector<QString> src_lines,
                                                 QString hex, bool is_numeric);

    // 清空去重表（只清 weak_ptr，不释放文本）。
    void reset();

    // 表内条目数（去重后的文本数）。仅用于诊断与测试，解码线程侧访问。
    size_t count() const { return m_indexs.size(); }

    static int hexToDecimal(char *hex);
    static void decimalToBinString(unsigned long long num, int bitSize,
                                   char *buffer, int buffer_size);

private:
    std::map<std::string, std::weak_ptr<const AnnotationText>> m_indexs;
};
