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

#include "sessiondata.h"

namespace pv {

SessionData::SessionData() {
  _cur_snap_samplerate = 0;
  _cur_samplelimits = 0;
  _trig_pos = 0;
  _logic_backup = nullptr;
  _glitch_filter_active = false;
  _glitch_filter_modes.clear();
  _signal_invert_active = false;
}

void SessionData::clear() {
  logic.clear();
  analog.clear();
  dso.clear();
  _trig_pos = 0;
  if (_logic_backup) {
    delete _logic_backup;
    _logic_backup = nullptr;
  }
  _glitch_filter_active = false;
  // 架构修复：clear() 不清除 thresholds/modes/auto_apply。
  // 这些是用户配置（滤波面板滑块位置），不是数据。
  // 采集开始时 clear() 被调用，如果清除 thresholds 会导致：
  //   1. 面板重新打开后滑块位置丢失（回退到推荐阈值）
  //   2. auto-apply 条件 !thresholds.empty() 不满足，采集后不自动滤波
  // 数据相关的清除由 clear_glitch_filter_state_for_capture() 处理（只清 active 标志）
  _signal_invert_active = false;
  _signal_invert_channels.clear();
}

} // namespace pv
