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

#include "signallistmodel.h"

#include <QString>

namespace pv {
namespace data {

SignalListModel::SignalListModel(QObject *parent)
    : QAbstractListModel(parent) {}

void SignalListModel::set_signal_models(
    std::vector<std::shared_ptr<SignalModel>> *models) {
  beginResetModel();
  _models = models;
  endResetModel();
}

std::shared_ptr<SignalModel> SignalListModel::get_model(int row) const {
  if (!_models || row < 0 || row >= (int)_models->size())
    return nullptr;
  return (*_models)[row];
}

int SignalListModel::rowCount(const QModelIndex &parent) const {
  if (parent.isValid() || !_models)
    return 0;
  return (int)_models->size();
}

int SignalListModel::columnCount(const QModelIndex &parent) const {
  if (parent.isValid())
    return 0;
  return ColCount;
}

QVariant SignalListModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid())
    return {};

  auto model = get_model(index.row());
  if (!model)
    return {};

  // Map column to role when Qt::DisplayRole / Qt::EditRole is used
  int effective_role = role;
  if (role == Qt::DisplayRole || role == Qt::EditRole) {
    switch (index.column()) {
    case ColIndex:   effective_role = IndexRole; break;
    case ColName:    effective_role = NameRole; break;
    case ColType:    effective_role = TypeRole; break;
    case ColEnabled: effective_role = EnabledRole; break;
    case ColColor:   effective_role = ColorRole; break;
    default: return {};
    }
  }

  switch (effective_role) {
  case IndexRole:
    return model->index();
  case NameRole:
    return QString::fromStdString(model->name());
  case TypeRole:
    return model->type();
  case EnabledRole:
    return model->enabled();
  case ColorRole:
    return QString::fromStdString(model->color());
  case VdivRole:
    return model->vdiv();
  case CouplingRole:
    return model->coupling();
  case TrigTypeRole:
    return model->trig_type();
  default:
    return {};
  }
}

bool SignalListModel::setData(const QModelIndex &index, const QVariant &value,
                               int role) {
  if (!index.isValid())
    return false;

  auto model = get_model(index.row());
  if (!model)
    return false;

  int effective_role = role;
  if (role == Qt::EditRole) {
    switch (index.column()) {
    case ColName:    effective_role = NameRole; break;
    case ColEnabled: effective_role = EnabledRole; break;
    case ColColor:   effective_role = ColorRole; break;
    default: return false;
    }
  }

  switch (effective_role) {
  case NameRole:
    model->set_name(value.toString().toStdString());
    break;
  case EnabledRole:
    model->set_enabled(value.toBool());
    break;
  case ColorRole:
    model->set_color(value.toString().toStdString());
    break;
  default:
    return false;
  }

  emit dataChanged(index, index, {role});
  return true;
}

Qt::ItemFlags SignalListModel::flags(const QModelIndex &index) const {
  if (!index.isValid())
    return Qt::NoItemFlags;

  Qt::ItemFlags f = QAbstractListModel::flags(index);

  switch (index.column()) {
  case ColName:
  case ColEnabled:
  case ColColor:
    f |= Qt::ItemIsEditable;
    break;
  default:
    break;
  }
  return f;
}

QVariant SignalListModel::headerData(int section, Qt::Orientation orientation,
                                      int role) const {
  if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
    return {};

  switch (section) {
  case ColIndex:   return tr("#");
  case ColName:    return tr("Name");
  case ColType:    return tr("Type");
  case ColEnabled: return tr("Enabled");
  case ColColor:   return tr("Color");
  default:         return {};
  }
}

void SignalListModel::refresh_row(int row) {
  if (!_models || row < 0 || row >= (int)_models->size())
    return;
  QModelIndex topLeft = index(row, 0);
  QModelIndex bottomRight = index(row, ColCount - 1);
  emit dataChanged(topLeft, bottomRight);
}

} // namespace data
} // namespace pv
