#ifndef PXVIEW_PV_DATA_SESSIONDATA_H
#define PXVIEW_PV_DATA_SESSIONDATA_H

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

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <mutex>

#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/base/pxvdef.h" // GlitchFilterMode

namespace pv {

/**
 * SessionData — per-capture-frame snapshot bundle (logic + analog + dso)
 * plus the per-frame glitch-filter / signal-invert state.
 *
 * Extracted from sigsession.h (Task 19 / Phase A) so CaptureManager can
 * hold `SessionData*` members without including sigsession.h (which would
 * create a circular dependency: SigSession owns CaptureManager via
 * unique_ptr, CaptureManager references SessionData).
 *
 * Public members are intentional: the managers (CaptureManager /
 * DataFeedParser / FilterProcessor) read/write the snapshot pointers and
 * the filter state directly. This is a plain data struct, not an
 * encapsulated type.
 */
class SessionData {
public:
  SessionData();
  // Raw pointer getters (pure read — no side effects).
  // Samplerate is injected at construction time in clear(), not on every
  // access. This eliminates the W1 window (clear → set_cur_snap_samplerate
  // gap) where snapshots temporarily had _samplerate=0.
  data::LogicSnapshot *get_logic() { return _logic.get(); }
  data::AnalogSnapshot *get_analog() { return _analog.get(); }
  data::DsoSnapshot *get_dso() { return _dso.get(); }

  // Shared_ptr getters — for zero-copy ownership sharing with SessionDocument.
  // The caller (copy_data_to_document) copies the shared_ptr, incrementing the
  // ref count. When SessionData::clear() resets its shared_ptr, the underlying
  // snapshot stays alive because SessionDocument still holds a reference.
  std::shared_ptr<data::LogicSnapshot> logic_shared() { return _logic; }
  std::shared_ptr<data::AnalogSnapshot> analog_shared() { return _analog; }
  std::shared_ptr<data::DsoSnapshot> dso_shared() { return _dso; }

  /// Reset to fresh snapshot instances, injecting the current samplerate
  /// at construction time. This prevents the _samplerate=0 window that
  /// caused analog/DSO flat-line waveforms.
  void clear();

  uint64_t _cur_snap_samplerate, _cur_samplelimits, _trig_pos;
  // NOTE: the former `_logic_backup` full-snapshot deep copy is gone. Filter
  // and invert undo now lives inside LogicSnapshot as a reversible edit log
  // (LogicSnapshot::revert_all_edits), which costs O(bytes rewritten) instead
  // of a second complete copy of the sample store and never tears down the
  // mmap allocator. See logicsnapshot_glitch_filter.h.
  bool _glitch_filter_active, _signal_invert_active;
  // 采集后自动重新应用滤波。atomic：写入来自 GUI 线程，读取来自数据馈送线程
  // （DataFeedParser 的 auto-apply 判定）——不属于 _filter_state_mutex 保护的
  // 那组"已应用状态"，因为它是一个不随采集重置的用户偏好。
  std::atomic<bool> _glitch_filter_auto_apply{false};
  bool _show_glitch_filter_overlay = true; // 显示波形轨道红色滤波提示叠加层
  // 架构修复：用 channel_index 作 key（消除 View/Core 位置序号错位）
  std::map<int, uint32_t> _glitch_filter_thresholds;
  std::map<int, GlitchFilterMode> _glitch_filter_modes;
  std::vector<bool> _signal_invert_channels;

  // Mutex protects _glitch_filter_active/_thresholds/_modes and
  // _signal_invert_active/_channels from concurrent access by the
  // FilterProcessor worker thread (writer) and main thread (reader).
  //
  // CONTRACT: every read AND write of those five fields must take this lock.
  // Returning a reference into one of the containers escapes the lock and is
  // therefore not allowed — accessors hand back copies (see
  // SigSession::glitch_filter_thresholds). Fields that are cross-thread but
  // not part of this "applied state" set (e.g. _glitch_filter_auto_apply) use
  // std::atomic instead, so nothing silently relies on the lock's protection.
  mutable std::mutex _filter_state_mutex;

private:
  // shared_ptr enables zero-copy ownership sharing with SessionDocument.
  // clear() resets these to fresh instances; if SessionDocument also holds
  // a shared_ptr to the old snapshot, the old data stays alive (ref count > 0).
  std::shared_ptr<data::LogicSnapshot> _logic;
  std::shared_ptr<data::AnalogSnapshot> _analog;
  std::shared_ptr<data::DsoSnapshot> _dso;
};

} // namespace pv

#endif // PXVIEW_PV_DATA_SESSIONDATA_H
