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

#include <atomic>

#include "pv/data/decode/annotationrestable.h"

// 解码器状态。Plan D 之后它不再承载"注解文本"（那部分已下沉到
// AnnotationText），只剩两件跨线程共享的标量 + 一个解码线程私有的去重表：
//
//   m_bNumeric  解码线程写（出现过数值）/ GUI 读（是否启用进制选择器）
//   m_format    GUI 写（用户切换进制）/ 渲染与导出线程读
//   m_resTable  只允许解码线程访问（见 AnnotationResTable 的线程契约）
//
// 这两个标量原本是裸 bool/int，属于 scan_cross_thread_flags 会报的跨线程
// 非原子标志，改为原子量。
class DecoderStatus
{
public:
    DecoderStatus();

    void clear();  

public:
    std::atomic<bool>   m_bNumeric; //when decoder get any numerical data,it will be set
    std::atomic<int>    m_format;   //protocol format code (DecoderDataFormat)
    void    *sdr_decoder_handle;
    AnnotationResTable  m_resTable; 
};
