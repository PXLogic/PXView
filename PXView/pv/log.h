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

#ifndef _PXV_LOG_H_
#define _PXV_LOG_H_

#include <QString>
#include <assert.h>
#include <log/xlog.h>

extern xlog_writer *pxv_log;

void pxv_log_init();
void pxv_log_uninit();

xlog_context *pxv_log_context();
void pxv_log_level(int l);

void pxv_log_enalbe_logfile(bool append);
void pxv_remove_log_file();
void pxv_clear_log_file();
void pxv_set_log_file_enable(bool flag);

QString get_pxv_log_path();

#define LOG_PREFIX ""
#define pxv_err(fmt, args...) xlog_err(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_warn(fmt, args...) xlog_warn(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_info(fmt, args...) xlog_info(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_dbg(fmt, args...) xlog_dbg(pxv_log, LOG_PREFIX fmt, ## args)
#define pxv_detail(fmt, args...) xlog_detail(pxv_log, LOG_PREFIX fmt, ## args)

#endif
