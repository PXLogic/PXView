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

#include "../data/decode/annotation.h"
#include "../data/decode/rowdata.h"
#include "../data/decoderstack.h"
#include "decodermodel.h"

using namespace boost;
using namespace std;

namespace pv {
namespace view {

DecoderModel::DecoderModel(QObject *parent)
    : QAbstractTableModel(parent),
      _decoder_stack(NULL)
{
}

void DecoderModel::setDecoderStack(pv::data::DecoderStack *decoder_stack)
{
    beginResetModel();
    _decoder_stack = decoder_stack;
    endResetModel();
}

void DecoderModel::set_visible_range(int64_t start_row, int64_t end_row)
{
    beginResetModel();
    _visible_start_row = start_row;
    _visible_end_row = end_row;
    endResetModel();
}

void DecoderModel::clear_visible_range()
{
    beginResetModel();
    _visible_start_row = -1;
    _visible_end_row = -1;
    endResetModel();
}

int DecoderModel::rowCount(const QModelIndex & /* parent */) const
{
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
    if (_decoder_stack)
        return _decoder_stack->list_rows_size();
    else
        return 1;
}

QVariant DecoderModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (role == Qt::TextAlignmentRole) {
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    else if (role == Qt::DisplayRole) {
        if (_decoder_stack) {
            uint64_t query_row = index.row();
            if (_visible_start_row >= 0)
                query_row = (uint64_t)(_visible_start_row + index.row());
            pv::data::decode::Annotation ann;
            if (_decoder_stack->list_annotation(&ann, index.column(), query_row)) {
                return ann.annotations().at(0);
            }
        }
    }
    return QVariant();
}

QVariant DecoderModel::headerData(int section,
                                   Qt::Orientation  orientation,
                                   int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Vertical) {
        if (_visible_start_row >= 0)
            return qlonglong((int64_t)section + _visible_start_row);
        return section;
    }

    if (_decoder_stack) {
        QString title;
        if (_decoder_stack->list_row_title(section, title))
            return title;
    }
    return QVariant();
}

} // namespace view
} // namespace pv
