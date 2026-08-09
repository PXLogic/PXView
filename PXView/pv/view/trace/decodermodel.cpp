/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2016 DreamSourceLab <support@dreamsourcelab.com>
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

#include <libsigrokdecode.h>

#include <algorithm>

#include "pv/data/decode/annotation.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/rowdata.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/view/trace/decodermodel.h"

using namespace std;

namespace pv {
namespace view {

// Format a duration in seconds to a human-readable string.
static QString formatDuration(double seconds) {
    if (seconds >= 1.0)
        return QString::number(seconds, 'f', 6) + "s";
    if (seconds >= 1e-3)
        return QString::number(seconds * 1e3, 'f', 3) + "ms";
    if (seconds >= 1e-6)
        return QString::number(seconds * 1e6, 'f', 3) + "us";
    if (seconds >= 1e-9)
        return QString::number(seconds * 1e9, 'f', 0) + "ns";
    return QStringLiteral("0s");
}

DecoderModel::DecoderModel(QObject *parent)
    : QAbstractTableModel(parent),
      _decoder_stack(nullptr)
{
}

void DecoderModel::setDecoderStack(pv::data::DecoderStack *decoder_stack)
{
    beginResetModel();
    _decoder_stack = decoder_stack;
    _multi_stack = false;
    _all_stacks.clear();
    _stack_row_offsets.clear();
    _visible_merged.clear();
    _column_map.clear();
    _column_headers.clear();
    endResetModel();
}

// ============ Multi-stack (All) mode ============
// Architecture: NO global merged index.
// - Full list: sequential display via prefix sums (O(stacks))
// - Visible range: lazy per-stack get_visible_range() + small merge
//   (O(visible_count * log), typically < 1000 annotations)

void DecoderModel::setAllStacks(const std::vector<pv::data::DecoderStack *> &stacks)
{
    beginResetModel();
    _multi_stack = true;
    _decoder_stack = nullptr;
    _all_stacks = stacks;
    _visible_start_row = -1;
    _visible_end_row = -1;
    _visible_merged.clear();

    // Compute prefix sums for full-list sequential row mapping.
    _stack_row_offsets.clear();
    uint64_t acc = 0;
    for (auto *s : stacks) {
        _stack_row_offsets.push_back(acc);
        if (s)
            acc += s->list_annotation_size();
    }
    _stack_row_offsets.push_back(acc); // sentinel

    buildColumnMap();
    endResetModel();
}

void DecoderModel::buildColumnMap()
{
    _column_map.clear();
    _column_headers.clear();
    for (int si = 0; si < (int)_all_stacks.size(); si++) {
        auto *s = _all_stacks[si];
        if (!s)
            continue;
        QString prefix;
        auto &decoders = s->stack();
        if (!decoders.empty())
            prefix = QString(decoders.back()->decoder()->name);
        else
            prefix = QString("Dec%1").arg(si);

        // Append custom label to distinguish multiple instances of the same
        // decoder type (e.g., "I2C(CH2.I2C):Address/Data").
        QString custom_label = s->label();
        if (!custom_label.isEmpty())
            prefix += "(" + custom_label + ")";

        int row_count = s->list_rows_size();
        for (int r = 0; r < row_count; r++) {
            QString title;
            if (s->list_row_title(r, title) && !title.isEmpty())
                _column_headers.push_back(prefix + ":" + title);
            else
                _column_headers.push_back(prefix + ":" + QString::number(r));
            _column_map.push_back({si, r});
        }
    }
}

void DecoderModel::buildVisibleMerged(uint64_t start_sample,
                                       uint64_t end_sample)
{
    _visible_merged.clear();
    for (int si = 0; si < (int)_all_stacks.size(); si++) {
        auto *s = _all_stacks[si];
        if (!s)
            continue;
        // Use the first visible decode row for range computation.
        std::map<const pv::data::decode::Row, bool> rows =
            s->get_rows_lshow();
        pv::data::decode::Row target_row;
        bool found = false;
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            if (it->second) {
                target_row = it->first;
                found = true;
                break;
            }
        }
        if (!found)
            continue;
        // Binary search within this stack for the visible range.
        auto range = s->get_visible_range(target_row, start_sample,
                                           end_sample);
        // Collect visible annotations from this stack.
        for (uint64_t r = range.first; r < range.second; r++) {
            pv::data::decode::Annotation ann;
            if (s->list_annotation(&ann, 0, r))
                _visible_merged.push_back({si, r, ann.start_sample()});
        }
    }
    // Sort only the small visible batch (typically < 1000).
    std::sort(_visible_merged.begin(), _visible_merged.end(),
              [](const MergedEntry &a, const MergedEntry &b) {
                  return a.start_sample < b.start_sample;
              });
}

void DecoderModel::mapRowToStack(int model_row,
                                  pv::data::DecoderStack *&stack,
                                  uint64_t &row_in_stack) const
{
    if (_multi_stack) {
        if (!_visible_merged.empty()) {
            // Visible range mode: direct lookup in merged list.
            if (model_row >= 0 && model_row < (int)_visible_merged.size()) {
                const auto &e = _visible_merged[model_row];
                if (e.stack_idx >= 0 && e.stack_idx < (int)_all_stacks.size()) {
                    stack = _all_stacks[e.stack_idx];
                    row_in_stack = e.row_in_stack;
                    return;
                }
            }
            stack = nullptr;
            row_in_stack = 0;
        } else {
            // Full list mode: sequential mapping via prefix sums.
            for (size_t i = 0; i + 1 < _stack_row_offsets.size(); i++) {
                if ((uint64_t)model_row < _stack_row_offsets[i + 1]) {
                    stack = (i < _all_stacks.size()) ? _all_stacks[i] : nullptr;
                    row_in_stack = (uint64_t)model_row - _stack_row_offsets[i];
                    return;
                }
            }
            stack = nullptr;
            row_in_stack = 0;
        }
    } else {
        uint64_t query_row = model_row;
        if (_visible_start_row >= 0)
            query_row = (uint64_t)(_visible_start_row + model_row);
        stack = _decoder_stack;
        row_in_stack = query_row;
    }
}

void DecoderModel::set_visible_range(int64_t start_row, int64_t end_row)
{
    beginResetModel();
    _visible_start_row = start_row;
    _visible_end_row = end_row;
    endResetModel();
}

void DecoderModel::set_visible_range_by_samples(uint64_t start_sample,
                                                 uint64_t end_sample)
{
    if (!_multi_stack) {
        set_visible_range(0, 0);
        return;
    }
    // Build the visible merged list on demand (small, fast).
    beginResetModel();
    buildVisibleMerged(start_sample, end_sample);
    // Use sentinel values to indicate "visible merged mode".
    _visible_start_row = 0;
    _visible_end_row = (int64_t)_visible_merged.size();
    endResetModel();
}

void DecoderModel::clear_visible_range()
{
    beginResetModel();
    _visible_start_row = -1;
    _visible_end_row = -1;
    _visible_merged.clear();
    endResetModel();
}

int DecoderModel::findRowBySample(uint64_t sample) const
{
    if (!_multi_stack)
        return -1;
    if (!_visible_merged.empty()) {
        // Binary search the visible merged list.
        auto it = std::lower_bound(_visible_merged.begin(),
            _visible_merged.end(), sample,
            [](const MergedEntry &e, uint64_t v) {
                return e.start_sample < v;
            });
        int idx = (int)std::distance(_visible_merged.begin(), it);
        if (idx > 0 && idx <= (int)_visible_merged.size()) {
            uint64_t prev_dist = sample - _visible_merged[idx - 1].start_sample;
            uint64_t curr_dist = (idx < (int)_visible_merged.size())
                ? (_visible_merged[idx].start_sample - sample)
                : ~(uint64_t)0;
            if (prev_dist <= curr_dist)
                idx--;
        }
        if (idx >= (int)_visible_merged.size())
            idx = (int)_visible_merged.size() - 1;
        return idx;
    }
    // Full list mode: search each stack, pick closest.
    // (Returns sequential row, not time-sorted.)
    int best_row = -1;
    uint64_t best_dist = ~(uint64_t)0;
    for (size_t si = 0; si < _all_stacks.size(); si++) {
        auto *s = _all_stacks[si];
        if (!s)
            continue;
        std::map<const pv::data::decode::Row, bool> rows = s->get_rows_lshow();
        pv::data::decode::Row target_row;
        bool found = false;
        for (auto it = rows.begin(); it != rows.end(); ++it) {
            if (it->second) { target_row = it->first; found = true; break; }
        }
        if (!found)
            continue;
        uint64_t ann_idx = s->get_annotation_index(target_row, sample);
        pv::data::decode::Annotation ann;
        if (s->list_annotation(&ann, 0, ann_idx)) {
            uint64_t ann_center = (ann.start_sample() + ann.end_sample()) / 2;
            uint64_t dist = (ann_center >= sample)
                ? (ann_center - sample) : (sample - ann_center);
            if (dist < best_dist) {
                best_dist = dist;
                best_row = (int)(_stack_row_offsets[si] + ann_idx);
            }
        }
    }
    return best_row;
}

int DecoderModel::rowCount(const QModelIndex & /* parent */) const
{
    if (_multi_stack) {
        if (!_visible_merged.empty())
            return (int)_visible_merged.size();
        if (_stack_row_offsets.empty())
            return 100;
        return (int)_stack_row_offsets.back();
    }
    if (!_decoder_stack)
        return 100;
    if (_visible_start_row >= 0) {
        const uint64_t full = _decoder_stack->list_annotation_size();
        int64_t end = _visible_end_row;
        if (end > (int64_t)full)
            end = (int64_t)full;
        int64_t count = end - _visible_start_row;
        return count > 0 ? (int)count : 0;
    }
    return _decoder_stack->list_annotation_size();
}

int DecoderModel::columnCount(const QModelIndex & /* parent */) const
{
    if (_multi_stack)
        return PRESET_COLUMN_COUNT + (int)_column_map.size();
    if (_decoder_stack)
        return _decoder_stack->list_rows_size() + PRESET_COLUMN_COUNT;
    else
        return 1 + PRESET_COLUMN_COUNT;
}

QVariant DecoderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (role == Qt::TextAlignmentRole) {
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    else if (role == Qt::DisplayRole) {
        // ===== Multi-stack (All) mode =====
        if (_multi_stack) {
            int64_t idx = index.row();
            // Visible mode: direct lookup in _visible_merged.
            // Full list mode: sequential via prefix sums.
            pv::data::DecoderStack *s = nullptr;
            uint64_t row_in_stack = 0;
            mapRowToStack(index.row(), s, row_in_stack);
            if (!s)
                return QVariant();

            if (index.column() == START_COLUMN_INDEX) {
                pv::data::decode::Annotation ann;
                if (s->list_annotation(&ann, 0, row_in_stack)) {
                    uint64_t sr = s->sample_rate();
                    if (sr > 0)
                        return QString::number((double)ann.start_sample() / (double)sr, 'f', 6) + "s";
                    return QString::number(ann.start_sample());
                }
                return QVariant();
            }
            if (index.column() == DURATION_COLUMN_INDEX) {
                pv::data::decode::Annotation ann;
                if (s->list_annotation(&ann, 0, row_in_stack)) {
                    uint64_t sr = s->sample_rate();
                    if (sr > 0)
                        return formatDuration((double)(ann.end_sample() - ann.start_sample()) / (double)sr);
                    return QString::number(ann.end_sample() - ann.start_sample());
                }
                return QVariant();
            }
            // Data columns: only show data if row's stack matches column's stack.
            int data_col = index.column() - PRESET_COLUMN_COUNT;
            if (data_col >= 0 && data_col < (int)_column_map.size()) {
                const auto &cm = _column_map[data_col];
                // Get the stack_idx for this row.
                int row_stack = -1;
                if (!_visible_merged.empty()) {
                    if (idx >= 0 && idx < (int64_t)_visible_merged.size())
                        row_stack = _visible_merged[idx].stack_idx;
                } else {
                    for (size_t i = 0; i + 1 < _stack_row_offsets.size(); i++) {
                        if ((uint64_t)index.row() < _stack_row_offsets[i + 1]) {
                            row_stack = (int)i;
                            break;
                        }
                    }
                }
                if (cm.first == row_stack) {
                    pv::data::decode::Annotation ann;
                    if (s->list_annotation(&ann, cm.second, row_in_stack)) {
                        if (!ann.annotations().empty())
                            return ann.annotations().at(0);
                    }
                }
            }
            return QVariant();
        }

        // ===== Single-stack mode =====
        if (_decoder_stack) {
            uint64_t query_row = index.row();
            if (_visible_start_row >= 0)
                query_row = (uint64_t)(_visible_start_row + index.row());

            if (index.column() == START_COLUMN_INDEX) {
                pv::data::decode::Annotation ann;
                if (_decoder_stack->list_annotation(&ann, 0, query_row)) {
                    uint64_t sr = _decoder_stack->sample_rate();
                    if (sr > 0)
                        return QString::number((double)ann.start_sample() / (double)sr, 'f', 6) + "s";
                    return QString::number(ann.start_sample());
                }
                return QVariant();
            }
            if (index.column() == DURATION_COLUMN_INDEX) {
                pv::data::decode::Annotation ann;
                if (_decoder_stack->list_annotation(&ann, 0, query_row)) {
                    uint64_t sr = _decoder_stack->sample_rate();
                    if (sr > 0)
                        return formatDuration((double)(ann.end_sample() - ann.start_sample()) / (double)sr);
                    return QString::number(ann.end_sample() - ann.start_sample());
                }
                return QVariant();
            }
            int ann_col = index.column() - PRESET_COLUMN_COUNT;
            pv::data::decode::Annotation ann;
            if (_decoder_stack->list_annotation(&ann, ann_col, query_row)) {
                if (!ann.annotations().empty())
                    return ann.annotations().at(0);
            }
        }
    }
    return QVariant();
}

QVariant DecoderModel::headerData(int section,
                                   Qt::Orientation orientation,
                                   int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Vertical) {
        if (_visible_start_row >= 0)
            return qlonglong((int64_t)section + _visible_start_row);
        return section;
    }

    if (section == START_COLUMN_INDEX)
        return QStringLiteral("Start");
    if (section == DURATION_COLUMN_INDEX)
        return QStringLiteral("Duration");

    if (_multi_stack) {
        int data_col = section - PRESET_COLUMN_COUNT;
        if (data_col >= 0 && data_col < (int)_column_headers.size())
            return _column_headers[data_col];
        return QVariant();
    }

    if (_decoder_stack) {
        QString title;
        int ann_col = section - PRESET_COLUMN_COUNT;
        if (_decoder_stack->list_row_title(ann_col, title))
            return title;
    }
    return QVariant();
}

} // namespace view
} // namespace pv
