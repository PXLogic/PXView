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
#include <memory>
#include <optional>
#include <QObject>
#include <QString>
#include <mutex> 

#include "pv/data/decode/row.h" 
#include "pv/data/model/signaldata.h"
#include "pv/data/decode/decoderstatus.h"
 

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

    // TS-3 fix: _stack now owns decoders via unique_ptr. stack() returns
    // a reference to the unique_ptr list. Callers iterate with
    // `for (auto &up : stack()) { auto dec = up.get(); ... }` or access
    // front/back via `.get()`. This eliminates manual delete in the
    // destructor and prevents leaks if build_row() throws after push_back.
    inline std::list<std::unique_ptr<decode::Decoder>>& stack(){
        return _stack;
    }

    const char* get_root_decoder_id();

	void add_sub_decoder(std::unique_ptr<decode::Decoder> decoder);
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
        return _decode_state.load(std::memory_order_acquire) == Running;
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
        std::lock_guard<std::mutex> lock(_output_mutex);
        return _error_message;
    }

    inline void *get_key_handel(){
        return _decoder_status.get();
    }

    inline bool is_capture_end(){
        return _is_capture_end;
    }

    inline void set_capture_end_flag(bool isEnd){
        _is_capture_end = isEnd;
        if (!isEnd){
            _progress.store(0);
            _is_decoding.store(false);
        }
    }

    inline int get_progress(){
        //if (!_is_decoding && _progress == 0)
          //  return -1;
        return _progress.load(std::memory_order_relaxed);
    }

    bool check_required_probes();

    inline uint64_t get_result_count(){
        return _result_count.load(std::memory_order_relaxed);
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
	// TS-3 fix: _stack owns decoders via unique_ptr — no manual delete needed.
	std::list<std::unique_ptr<decode::Decoder>> _stack;
    std::shared_ptr<pv::data::LogicSnapshot> _snapshot;
  
    // TS-3 fix: _rows owns RowData via unique_ptr — no manual delete needed.
    std::map<const decode::Row, std::unique_ptr<decode::RowData>>   _rows;
    std::map<const decode::Row, bool>       _rows_gshow;
    std::map<const decode::Row, bool>       _rows_lshow;
    std::map<std::pair<const srd_decoder*, int>, decode::Row> _class_rows;
  
    SigSession      *_session;
    data::SessionDocument *_owner_document;
    uint64_t        _handle_id = 0;
    uint64_t        _version = 0;
    std::atomic<decode_state> _decode_state;
    std::atomic<bool> _options_changed{false};
    std::atomic<bool> _no_memory{false};
    int64_t         _mark_index;

    // TS-3 fix: _decoder_status owned via unique_ptr — no DESTROY_OBJECT needed.
    std::unique_ptr<DecoderStatus> _decoder_status;
    QString         _error_message;
    int64_t	        _samples_decoded;
    std::atomic<uint64_t> _sample_count{0};
 
    std::shared_ptr<decode_task_status> _stask_stauts;    
    mutable std::mutex _output_mutex; 
    bool            _is_capture_end;
    std::atomic<int> _progress{0};
    std::atomic<bool> _is_decoding{false};
    std::atomic<uint64_t> _result_count{0};
    // [PWMDBG] diagnostics: annotations dropped in annotation_callback()
    std::atomic<uint64_t> _ann_dropped_stop{0};
    std::atomic<uint64_t> _ann_dropped_mem{0};
    std::atomic<uint64_t> _ann_dropped_row{0};

    QString         _label; // custom user-facing label for this decoder stack

	friend class DecoderStackTest::TwoDecoderStack;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_DECODERSTACK_H
