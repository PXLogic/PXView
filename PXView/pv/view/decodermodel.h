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

// Moved from pv::data to pv::view (purify-architecture-concepts Task 10):
// DecoderModel is a QAbstractTableModel subclass that returns Qt::DisplayRole
// / Qt::AlignLeft etc. — Qt Model/View UI concepts that do not belong in the
// Core data layer. It now lives in the View layer alongside the QTableView
// that consumes it (ProtocolDock). Core (SigSession/SessionDocument/
// SessionSnapshot) no longer holds or exposes a DecoderModel pointer.

#ifndef PXVIEW_PV_VIEW_DECODERMODEL_H
#define PXVIEW_PV_VIEW_DECODERMODEL_H

#include <QAbstractTableModel>
#include <cstdint>

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

class DecoderModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    DecoderModel(QObject *parent = 0);

    int rowCount(const QModelIndex & /*parent*/) const;
    int columnCount(const QModelIndex & /*parent*/) const;
    QVariant data(const QModelIndex &index, int role) const;
    QVariant headerData(int section, Qt::Orientation orientation,int role) const;

    void setDecoderStack(pv::data::DecoderStack *decoder_stack);

    inline  pv::data::DecoderStack* getDecoderStack(){
        return _decoder_stack;
    }

    // Visible-range slicing. When _visible_start_row >= 0, rowCount/data/
    // headerData restrict to [_visible_start_row, _visible_end_row). Call
    // clear_visible_range() to restore full-list behaviour.
    void set_visible_range(int64_t start_row, int64_t end_row);
    void clear_visible_range();
    inline int64_t visible_start_row() const { return _visible_start_row; }

private:
    pv::data::DecoderStack   *_decoder_stack;
    // -1 means "full list" (backward-compatible default).
    int64_t _visible_start_row = -1;
    int64_t _visible_end_row = -1;
};

} // namespace view
} // namespace pv

#endif // PXVIEW_PV_VIEW_DECODERMODEL_H
