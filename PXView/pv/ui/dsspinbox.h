/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef DSSPINBOX_H
#define DSSPINBOX_H

#include <QSpinBox>
#include <QDoubleSpinBox>

namespace pv {
namespace ui {

/**
 * @brief 自定义 SpinBox，拦截滚轮事件防止滚动页面时意外改变数值
 * 
 * 当鼠标悬停在输入框上并滚动滚轮时：
 * - 滚轮事件被拦截，不会调整数值
 * - 事件会向上传播到父级的滚动容器
 */
class DsSpinBox : public QSpinBox
{
    Q_OBJECT

public:
    explicit DsSpinBox(QWidget *parent = nullptr);
};

/**
 * @brief 自定义 DoubleSpinBox，拦截滚轮事件防止滚动页面时意外改变数值
 */
class DsDoubleSpinBox : public QDoubleSpinBox
{
    Q_OBJECT

public:
    explicit DsDoubleSpinBox(QWidget *parent = nullptr);
};

} // namespace ui
} // namespace pv

#endif // DSSPINBOX_H
