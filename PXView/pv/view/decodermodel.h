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

#ifndef PXVIEW_PV_VIEW_DECODERMODEL_H
#define PXVIEW_PV_VIEW_DECODERMODEL_H

#include <QAbstractTableModel>
#include <cstdint>
#include <vector>
#include <utility>
#include <QString>

#include "../data/decode/rowdata.h"

namespace pv {
namespace data {
class DecoderStack;
namespace decode {
class Annotation;
class Decoder;
class Row;
} // namespace decode
} // namespace data

namespace view {

// One entry in the time-sorted visible annotation list (multi-stack All mode).
struct MergedEntry {
    int stack_idx;         // which DecoderStack owns this annotation
    uint64_t row_in_stack; // row index within that stack
    uint64_t start_sample; // for time sorting / binary search
};

class DecoderModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    static const int PRESET_COLUMN_COUNT = 2;
    static const int START_COLUMN_INDEX = 0;
    static const int DURATION_COLUMN_INDEX = 1;

    DecoderModel(QObject *parent = 0);

    int rowCount(const QModelIndex & /*parent*/) const;
    int columnCount(const QModelIndex & /*parent*/) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;

    void setDecoderStack(pv::data::DecoderStack *decoder_stack);
    // Multi-stack (All) mode: store stacks + compute prefix sums.
    // Does NOT build a global merged index — O(stacks), instant.
    void setAllStacks(const std::vector<pv::data::DecoderStack *> &stacks);
    inline bool isMultiStackMode() const { return _multi_stack; }
    inline int stackCount() const { return (int)_all_stacks.size(); }

    // Map a model row to the owning decoder stack and row-within-stack.
    void mapRowToStack(int model_row, pv::data::DecoderStack *&stack,
                       uint64_t &row_in_stack) const;

    inline pv::data::DecoderStack *getDecoderStack() { return _decoder_stack; }

    // Visible-range slicing.
    void set_visible_range(int64_t start_row, int64_t end_row);
    // Multi-stack: per-stack get_visible_range() + small merge.
    void set_visible_range_by_samples(uint64_t start_sample,
                                      uint64_t end_sample);
    void clear_visible_range();
    inline int64_t visible_start_row() const { return _visible_start_row; }

    // Binary-search each stack for the annotation closest to `sample`.
    int findRowBySample(uint64_t sample) const;

private:
    pv::data::DecoderStack *_decoder_stack;

    // Multi-stack mode.
    bool _multi_stack = false;
    std::vector<pv::data::DecoderStack *> _all_stacks;
    // Prefix sums for full-list sequential row mapping (O(stacks)).
    std::vector<uint64_t> _stack_row_offsets;
    // Lazy visible-range merged list (only built when viewport changes).
    std::vector<MergedEntry> _visible_merged;
    // Column layout: (stack_idx, decode_row_idx) per data column.
    std::vector<std::pair<int, int>> _column_map;
    std::vector<QString> _column_headers;

    // -1 means "full list" (backward-compatible default).
    int64_t _visible_start_row = -1;
    int64_t _visible_end_row = -1;

    // Build the column map and headers from all stacks' decode rows.
    void buildColumnMap();
    // Build _visible_merged by querying each stack's get_visible_range().
    void buildVisibleMerged(uint64_t start_sample, uint64_t end_sample);
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_DECODERMODEL_H
