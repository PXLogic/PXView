/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef STRING_IDS_H
#define STRING_IDS_H

inline constexpr int STR_PAGE_MSG = 1;
inline constexpr int STR_PAGE_TOOLBAR = 2;
inline constexpr int STR_PAGE_DLG = 3;
inline constexpr int STR_PAGE_DSL = 100;
inline constexpr int STR_PAGE_DECODER = 101;
inline constexpr int STR_PAGE_SIGNAL_PROC = 102;
/* libsigrok 输入/输出模块选项（导入/导出选项对话框里的标签与说明）。
 * 这些字符串由模块自己声明（英文），按选项 id 在 input_output.json 里翻译。 */
inline constexpr int STR_PAGE_INPUT_OUTPUT = 103;

#endif