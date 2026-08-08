/*
 * This file is part of the PulseView project.
 * PXView is based on PulseView.
 * 
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2014 DreamSourceLab <support@dreamsourcelab.com>
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

#ifndef PXVIEW_PV_DATA_DECODERSTACK_H
#define PXVIEW_PV_DATA_DECODERSTACK_H

#include <libsigrokdecode.h>
#include <list>
#include <atomic>
#include <optional>
#include <QObject>
#include <QString>
#include <mutex> 

#include "decode/row.h" 
#include "../data/signaldata.h"
#include "decode/decoderstatus.h"
 

namespace DecoderStackTest {
class TwoDecoderStack;
}
 
namespace pv {

class SigSession;

namespace view {
class LogicSignal;
}

namespace data {

class LogicSnapshot;
class SessionDocument;

namespace decode {
class Annotation;
class Decoder;
class RowData;
}

class DecoderStack;

struct decode_task_status
{
    std::atomic<bool> _bStop{false};
    DecoderStack *_decoder;
};

 //a torotocol have a DecoderStack, destroy by DecodeTrace
class DecoderStack : public QObject, public SignalData
{
	Q_OBJECT

private:
	static const double DecodeMargin;
	static const double DecodeThreshold;
	static const int64_t DecodeChunkLength;
	static const unsigned int DecodeNotifyPeriod;
    static const uint64_t MaxChunkSize = 1024 * 16;

public:
    enum decode_state {
        Stopped,
        Running
    };

public:
   	DecoderStack(pv::SigSession *_session,
		const srd_decoder *const decoder, DecoderStatus *decoder_status);

public:

	virtual ~DecoderStack();

    inline std::list<decode::Decoder*>& stack(){
        return _stack;
    }

    const char* get_root_decoder_id();

	void add_sub_decoder(decode::Decoder *decoder);
    void remove_sub_decoder(decode::Decoder *decoder);
    void remove_decoder_by_handel(const srd_decoder *dec);
    
    void build_row();

	int64_t samples_decoded();

	/**
	 * Extracts sorted annotations between two period into a vector.
	 */
	void get_annotation_subset(
		std::vector<pv::data::decode::Annotation*> &dest,
		const decode::Row &row, uint64_t start_sample,
		uint64_t end_sample);

    decode::RowData* get_row_data(const decode::Row &row);

    uint64_t get_annotation_index(
        const decode::Row &row, uint64_t start_sample);
    // Returns the half-open [start_idx, end_idx) annotation index range that
    // overlaps [start_sample, end_sample] for the given row. Used by
    // ProtocolDock to filter its list to the visible viewport portion.
    std::pair<size_t, size_t> get_visible_range(
        const decode::Row &row, uint64_t start_sample, uint64_t end_sample);
    uint64_t get_max_annotation(const decode::Row &row);
    uint64_t get_min_annotation(const decode::Row &row); // except instant(end=start) annotation

    std::map<const decode::Row, bool> get_rows_gshow();
    std::map<const decode::Row, bool> get_rows_lshow();
    void set_rows_gshow(const decode::Row row, bool show);
    void set_rows_lshow(const decode::Row row, bool show);
    bool has_annotations(const decode::Row &row);
    uint64_t list_annotation_size();
    uint64_t list_annotation_size(uint16_t row_index);


    bool list_annotation(decode::Annotation *ann,
                        uint16_t row_index, uint64_t col_index);


    bool list_row_title(int row, QString &title);
	 
	void clear();
    void init();
	uint64_t get_max_sample_count();

    inline bool IsRunning(){
        return _decode_state == Running;
    }
 
	void begin_decode_work();
    
    void stop_decode_work();  
    int list_rows_size();
    bool options_changed();
    void set_options_changed(bool changed);

    uint64_t sample_count();
    uint64_t sample_rate();
    bool out_of_memory();
    void set_mark_index(int64_t index);
    int64_t get_mark_index();
    void frame_ended();

    inline QString error_message(){ 
	    return _error_message;
    }

    inline void *get_key_handel(){
        return _decoder_status;
    }

    inline bool is_capture_end(){
        return _is_capture_end;
    }

    inline void set_capture_end_flag(bool isEnd){
        _is_capture_end = isEnd;
        if (!isEnd){
            _progress = 0;
            _is_decoding = false;
        }
    }

    inline int get_progress(){
        //if (!_is_decoding && _progress == 0)
          //  return -1;
        return _progress;
    }

    bool check_required_probes();

    inline uint64_t get_result_count(){
        return _result_count;
    }

    void set_owner_document(data::SessionDocument *doc) { _owner_document = doc; }
    data::SessionDocument* get_owner_document() { return _owner_document; }

    // Unique handle id assigned by SigSession when the stack is created.
    // Allows the API/MCP layer to stably reference a decoder stack across
    // re-creation (a brand-new stack always gets a fresh handle_id). The
    // version is bumped when an existing stack is re-created in place (e.g.
    // by restart_decoders) so consumers can invalidate cached results.
    inline uint64_t handle_id() const { return _handle_id; }
    inline uint64_t version() const { return _version; }
    inline void set_handle_id(uint64_t id) { _handle_id = id; }
    inline void bump_version() { _version++; }

    // Custom user-facing label for this decoder stack. Set by the View
    // layer (ProtocolDock) or the API layer (SessionService::add_decoder)
    // when the user provides a custom name. Used in exports and list_analyzers
    // to distinguish multiple instances of the same decoder (e.g. two SPI
    // decoders can be labelled "CH2.SPI" and "CH3.SPI").
    inline QString label() const { return _label; }
    inline void set_label(const QString &label) { _label = label; }

    // Set by callers (e.g. SigSession) to mark a stack for asynchronous
    // deletion by the decode thread. Mirrors the legacy
    // view::DecodeTrace::_delete_flag mechanism.
    std::atomic<bool> _delete_flag{false};

private:
    void decode_data(const uint64_t decode_start, const uint64_t decode_end, srd_session *const session);
	void execute_decode_stack();
	static void annotation_callback(srd_proto_data *pdata, void *self);
    void do_decode_work();
  
signals:
	void new_decode_data();
    void decode_done();
  
private: 
	std::list<decode::Decoder*> _stack;
	pv::data::LogicSnapshot *_snapshot;
  
    std::map<const decode::Row, decode::RowData*>   _rows;
    std::map<const decode::Row, bool>       _rows_gshow;
    std::map<const decode::Row, bool>       _rows_lshow;
    std::map<std::pair<const srd_decoder*, int>, decode::Row> _class_rows;
  
    SigSession      *_session;
    data::SessionDocument *_owner_document;
    uint64_t        _handle_id = 0;
    uint64_t        _version = 0;
    decode_state    _decode_state;
    std::atomic<bool> _options_changed{false};
    std::atomic<bool> _no_memory{false};
    int64_t         _mark_index;

    DecoderStatus   *_decoder_status;
    QString         _error_message;
    int64_t	        _samples_decoded;
    uint64_t        _sample_count; 
 
    decode_task_status  *_stask_stauts;    
    mutable std::mutex _output_mutex; 
    bool            _is_capture_end;
    int             _progress;
    bool            _is_decoding;
    uint64_t        _result_count;
    // [PWMDBG] diagnostics: annotations dropped in annotation_callback()
    uint64_t        _ann_dropped_stop = 0;
    uint64_t        _ann_dropped_mem = 0;
    uint64_t        _ann_dropped_row = 0;

    QString         _label; // custom user-facing label for this decoder stack

	friend class DecoderStackTest::TwoDecoderStack;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODERSTACK_H
