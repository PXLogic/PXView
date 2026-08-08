/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
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

#ifndef PXVIEW_PV_DATA_SIGNALLISTMODEL_H
#define PXVIEW_PV_DATA_SIGNALLISTMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <memory>

#include "pv/data/model/signalmodel.h"

namespace pv {
namespace data {

/**
 * @brief QAbstractListModel wrapper for the SignalModel list.
 *
 * Phase 4 (Signal Model/View separation): introduces a standard Qt model
 * as the middleware between the Core layer (which owns SignalModel objects
 * via SigSession::get_signal_models()) and the View layer (which currently
 * accesses signals directly via View::get_own_signals()).
 *
 * The model exposes signal properties (name, type, enabled, color, etc.)
 * as standard Qt model data roles, enabling QTableView/QListView binding
 * and future headless-mode introspection without depending on View widgets.
 *
 * Column roles:
 *   - ColIndex:   channel index (int, read-only)
 *   - ColName:    channel name (QString, editable)
 *   - ColType:    channel type (int, read-only)
 *   - ColEnabled: enabled state (bool, editable)
 *   - ColColor:   color string (QString, editable)
 */
class SignalListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Column {
        ColIndex = 0,
        ColName,
        ColType,
        ColEnabled,
        ColColor,
        ColCount
    };

    enum Role {
        IndexRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        EnabledRole,
        ColorRole,
        VdivRole,
        CouplingRole,
        TrigTypeRole,
    };

    explicit SignalListModel(QObject *parent = nullptr);

    // ---- QAbstractListModel overrides ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

    // ---- Source sync ----
    /// Replace the underlying SignalModel vector. Resets the model.
    void set_signal_models(std::vector<std::shared_ptr<SignalModel>> *models);

    /// Refresh a single row (emit dataChanged).
    void refresh_row(int row);

    /// Get the SignalModel at the given row, or nullptr.
    std::shared_ptr<SignalModel> get_model(int row) const;

private:
    std::vector<std::shared_ptr<SignalModel>> *_models = nullptr;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SIGNALLISTMODEL_H
