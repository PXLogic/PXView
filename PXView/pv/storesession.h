/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2014 Joel Holdsworth <joel@airwebreathe.org.uk>
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

#ifndef PXVIEW_PV_STORESESSION_H
#define PXVIEW_PV_STORESESSION_H

#include <stdint.h>
#include <string>
#include <thread>  
#include <QObject>
#include <libsigrok.h> 

#include "interface/icallbacks.h"

#include "ZipMaker.h"

namespace pv {

class SigSession;

namespace data {
class Snapshot;
class LogicSnapshot;
class AnalogSnapshot;
class DsoSnapshot;
}

namespace dock {
class ProtocolDock;
}

class StoreSession : public QObject
{
	Q_OBJECT
  
public:
    StoreSession(SigSession *session);

	~StoreSession();
    SigSession* session();
    void get_progress(uint64_t *writed, uint64_t *total);
	const QString& error();
    bool save_start();
    bool export_start();
	void wait();
	void cancel();

private:
    void save_proc(pv::data::Snapshot *snapshot);
    void save_logic(pv::data::LogicSnapshot *logic_snapshot);
    void save_analog(pv::data::AnalogSnapshot *analog_snapshot);
    void save_dso(pv::data::DsoSnapshot *dso_snapshot);
    bool meta_gen(data::Snapshot *snapshot, std::string &str);
    void export_proc(pv::data::Snapshot *snapshot);
    void export_exec(pv::data::Snapshot *snapshot);
    bool decoders_gen(std::string &str);
 

public:    
    bool gen_decoders_json(QJsonArray &array);
    bool load_decoders(dock::ProtocolDock *widget, QJsonArray &dec_array);
    QString MakeSaveFile(bool bDlg);
    QString MakeExportFile(bool bDlg);

    inline QString GetFileName(){
        return _file_name;
    }

    inline void SetFileName(const QString &name){
        _file_name = name;
    }

    bool IsLogicDataType();

    inline void SetDataRange(uint64_t start_index, uint64_t end_index){
        _start_index = start_index;
        _end_index = end_index;
    }

    inline void set_export_channels(const std::vector<int32_t>& channels) {
        _export_channels = channels;
    }

    inline void set_export_channel_type(int type) {
        _export_channel_type = type;
    }

    inline bool is_busy(){
        return _is_busy;
    }

    inline void set_analog_downsample_ratio(uint64_t ratio){
        _analog_downsample_ratio = ratio;
    }

    inline uint64_t get_analog_downsample_ratio() const {
        return _analog_downsample_ratio;
    }

    inline void set_iso8601_timestamp(bool enabled){
        _iso8601_timestamp = enabled;
    }

    inline bool get_iso8601_timestamp() const {
        return _iso8601_timestamp;
    }

private:
    QList<QString> getSuportedExportFormats();
    double get_integer(GVariant * var);
    void MakeChunkName(char *chunk_name, int chunk_num, int index, int type, int version);

signals:
	void progress_updated();

public:
   ISessionDataGetter   *_sessionDataGetter;

private:
    QString         _file_name;
    QString         _suffix;
    SigSession      *_session;
	std::thread     _thread;
    const struct sr_output_module* _outModule;
 
	uint64_t        _units_stored;
	uint64_t        _unit_count;
    bool            _has_error;
	QString         _error;
    volatile bool   _canceled;
    ZipMaker        m_zipDoc;  
    uint64_t        _start_index;
    uint64_t        _end_index;
    volatile bool   _is_busy;
    uint64_t        _analog_downsample_ratio = 1;
    bool            _iso8601_timestamp = false;
    std::vector<int32_t> _export_channels;
    int             _export_channel_type = -1;
};

} // pv

#endif // PXVIEW_PV_STORESESSION_H
