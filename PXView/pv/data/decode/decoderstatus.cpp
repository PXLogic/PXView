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

#include "pv/data/decode/decoderstatus.h"

DecoderStatus::DecoderStatus()
{
        m_bNumeric.store(false, std::memory_order_relaxed);
        m_format.store(0, std::memory_order_relaxed);
        sdr_decoder_handle = nullptr;
}

void DecoderStatus::clear()
{
        // 只清去重表（表里只有 weak_ptr，已发布快照持有的文本不受影响），
        // m_format 是用户的视图设置，不随解码轮次重置（与旧行为一致）。
        m_resTable.reset();
        m_bNumeric.store(false, std::memory_order_relaxed);
} 
