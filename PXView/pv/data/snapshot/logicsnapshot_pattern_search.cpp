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

// One-way dependency: this TU includes logicsnapshot.h (transitively via
// logicsnapshot_pattern_search.h) and logicsnapshot_pattern_search.h.
// logicsnapshot.h forward-declares LogicSnapshotPatternSearch and holds it via
// unique_ptr<forward-declared type>; the forwarder lives in logicsnapshot.cpp.

#include <mutex>

#include "pv/data/snapshot/logicsnapshot_pattern_search.h"

using namespace std;

namespace pv {
namespace data {

// ----------------------------------------------------------------------------
// Construction / destruction
// ----------------------------------------------------------------------------

LogicSnapshotPatternSearch::LogicSnapshotPatternSearch(LogicSnapshot *host)
    : _host(host)
{
}

LogicSnapshotPatternSearch::~LogicSnapshotPatternSearch()
{
}

// ----------------------------------------------------------------------------
// pattern_search / pattern_search_self
// ----------------------------------------------------------------------------

bool LogicSnapshotPatternSearch::pattern_search(int64_t start, int64_t end,
                                                int64_t &index,
                                                std::map<uint16_t, QString> &pattern,
                                                bool isNext) {
  // C3 (P9-on-raw): FINITE (non-loop) fast path is lock-free — see
  // committed_sample_count() for the publication protocol. `_loop_offset` is
  // 0 for finite captures, so the search works directly on user coords.
  if (!_host->_is_loop) {
    const uint64_t sample_count = _host->committed_sample_count();
    if (sample_count == 0)
      return false;
    // Clamp the search window to the committed range.
    if (start < 0)
      start = 0;
    if (end >= (int64_t)sample_count)
      end = (int64_t)sample_count - 1;
    return pattern_search_self(start, end, index, pattern, isNext);
  }

  std::lock_guard<std::recursive_mutex> lock(_host->_mutex);

  start += _host->_loop_offset;
  end += _host->_loop_offset;
  index += _host->_loop_offset;
  _host->_ring_sample_count += _host->_loop_offset;

  bool flag = pattern_search_self(start, end, index, pattern, isNext);

  index -= _host->_loop_offset;
  _host->_ring_sample_count -= _host->_loop_offset;
  return flag;
}

bool LogicSnapshotPatternSearch::pattern_search_self(int64_t start, int64_t end,
                                                     int64_t &index,
                                                     std::map<uint16_t, QString> &pattern,
                                                     bool isNext) {
  if (pattern.empty()) {
    return true;
  }

  char flagList[CHANNEL_MAX_COUNT];
  char lstValues[CHANNEL_MAX_COUNT];
  int chanIndexs[CHANNEL_MAX_COUNT];
  int count = 0;
  bool bEdgeFlag = false;

  const int64_t to = isNext ? end + 1 : start - 1;
  const int64_t step = isNext ? 1 : -1;

  for (auto it = pattern.begin(); it != pattern.end(); it++) {
    char flag = *(it->second.toStdString().c_str());
    int channel = it->first;

    if (flag != 'X' && flag != 'x' && _host->has_data(channel)) {
      flagList[count] = flag;
      chanIndexs[count] = channel;
      count++;

      if (flag == 'R' || flag == 'F' || flag == 'C') {
        bEdgeFlag = true;
      }
    }
  }
  if (count == 0) {
    return true;
  }

  // find
  bool ret = false;
  char val = 0;
  int macthed = 0;

  // get first edge values
  if (bEdgeFlag) {
    for (int i = 0; i < count; i++) {
      lstValues[i] = (char)_host->get_sample_self(index, chanIndexs[i]);
    }
    index += step;
  }

  if (index < start) {
    index = start;
  }
  if (index > end) {
    index = end;
  }

  while (index != to) {
    macthed = 0;

    for (int i = 0; i < count; i++) {
      val = (char)_host->get_sample_self(index, chanIndexs[i]);

      if (flagList[i] == '0') {
        macthed += !val;
      } else if (flagList[i] == '1') {
        macthed += val;
      } else if (flagList[i] == 'R') {
        if (isNext)
          macthed += (lstValues[i] == 0 && val == 1);
        else
          macthed += (lstValues[i] == 1 && val == 0);
      } else if (flagList[i] == 'F') {
        if (isNext)
          macthed += (lstValues[i] == 1 && val == 0);
        else
          macthed += (lstValues[i] == 0 && val == 1);
      } else if (flagList[i] == 'C') {
        if (isNext)
          macthed += (lstValues[i] == 0 && val == 1) ||
                     (lstValues[i] == 1 && val == 0);
        else
          macthed += (lstValues[i] == 1 && val == 0) ||
                     (lstValues[i] == 0 && val == 1);
      }
      lstValues[i] = val;
    }

    // matched all
    if (macthed == count) {
      ret = true;
      if (!isNext) {
        index++; // move to prev position
      }
      break;
    }

    index += step;
  }

  return ret;
}

}  // namespace data
}  // namespace pv
