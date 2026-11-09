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

#include "pv/utility/path.h"

namespace pv{
namespace path{

    std::string ConvertPath(QString fileName)
    {
        return fileName.toUtf8().toStdString();
    }

    QString GetDirectoryName(QString path)
    {
        const qsizetype lstdex = path.lastIndexOf('/');
        if (lstdex != -1)
        {
            return path.left(lstdex);
        }
        return path;
    }

    std::string ToUnicodePath(QString path)
    {
        // Every C/C++ consumer of these char* paths expects UTF-8 on Windows:
        //   * common/minizip/ioapi.c fopen64_file_func: MultiByteToWideChar(
        //     CP_UTF8, MB_ERR_INVALID_CHARS, ...) + _wfopen. GBK bytes for a
        //     Chinese path are invalid UTF-8, so the conversion returns 0 and
        //     unzOpen64()/zipOpen64() fail outright.
        //   * libsigrok session_file.c: g_file_test() + zip_open() (GLib and
        //     libzip both use UTF-8 filenames on Windows).
        //   * libsigrokdecode: srd_init(), srd_decoder_user_searchpath_add(),
        //     srd_c_decoder_path_add() (GLib).
        //   * common/log/xlog.c: xlog_open_file() below does the same
        //     CP_UTF8 -> _wfopen conversion as minizip.
        //
        // This used to encode with QStringEncoder::System, i.e. the machine's
        // ANSI code page (GBK/CP936 on a Chinese Windows). ConvertPath() was
        // already UTF-8, so the two halves of a save/load round trip disagreed:
        // storing a .pxl into a Chinese directory worked (UTF-8), but loading
        // it back failed, while pure-ASCII paths were unaffected because the
        // two encodings produce identical bytes there.
        //
        // Keep the separate name for source compatibility, but make both
        // functions the single UTF-8 convention.
        return ConvertPath(path);
    }
}
}
