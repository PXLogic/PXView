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
#include <shared_mutex>
#include <condition_variable>

#include "pv/data/decode/row.h" 
#include "pv/data/model/signaldata.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/data/decoderanalogdata.h"


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
    // P0-2 fix: use shared_ptr instead of raw pointer so the callback
    // safely keeps the DecoderStack alive for the duration of its execution.
    std::shared_ptr<DecoderStack> _decoder;
};

 //a torotocol have a DecoderStack, destroy by DecodeTrace
// P0-2 fix: inherit enable_shared_from_this so callbacks can obtain a
// shared_ptr to self, preventing use-after-free if the stack is destroyed
// while a callback is in flight.
class DecoderStack : public QObject,
                     public SignalData,
                     public std::enable_shared_from_this<DecoderStack>
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

    // post-decode analog display trigger for TDM/PWM repeat.
    bool get_analog_display_trigger_config(
        DecoderAnalogTriggerConfig &config) const;
    bool find_analog_display_trigger(
        uint64_t &sample_position,
        DecoderAnalogTriggerConfig *config_out = nullptr) const;

	void add_sub_decoder(std::unique_ptr<decode::Decoder> decoder);
    void remove_sub_decoder(decode::Decoder *decoder);
    void remove_decoder_by_handel(const srd_decoder *dec);
    
    void build_row();

	int64_t samples_decoded();

	/**
	 * Extracts sorted annotations between two period into a vector.
	 */
	void get_annotation_subset(
		std::vector<const pv::data::decode::Annotation *> &dest,
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
    bool list_row_description(int row, QString &desc);
	 
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

    // decoded analog output exposed to DecodeTrace.
    std::vector<std::shared_ptr<DecoderAnalogData>> analog_data_copy() const;
    size_t analog_data_size() const;
    void clear_analog_data();
    bool analog_visible() const { return _analog_visible; }
    void set_analog_visible(bool v) { _analog_visible = v; }

    inline QString error_message(){
        std::lock_guard<std::mutex> lock(_state_mutex);
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
        // P0-1 fix: notify the decode thread that capture has ended so it
        // can wake up from the condition variable wait and check the flag.
        _data_cond.notify_all();
    }

    inline int get_progress(){
        return _progress.load(std::memory_order_relaxed);
    }

    bool check_required_probes();

    inline uint64_t get_result_count(){
        return _result_count.load(std::memory_order_relaxed);
    }

    void set_owner_document(data::SessionDocument *doc) { _owner_document = doc; }
    data::SessionDocument* get_owner_document() { return _owner_document; }

    // Unique handle id assigned by SigSession when the stack is created.
    inline uint64_t handle_id() const { return _handle_id; }
    inline uint64_t version() const { return _version; }
    inline void set_handle_id(uint64_t id) { _handle_id = id; }
    inline void bump_version() { _version++; }

    // Custom user-facing label for this decoder stack.
    inline QString label() const { return _label; }
    inline void set_label(const QString &label) { _label = label; }

    // Auto-generate a display label from the first bound channel name
    QString auto_label() const;

    // P0-1 fix: Called by the data feed (e.g. SigSession::feed_in_logic)
    // to notify the decode thread that new sample data is available.
    void notify_data_ready();

    // P0-2 fix: Returns a shared_ptr to this DecoderStack.
    std::shared_ptr<DecoderStack> get_shared_ptr() {
        return shared_from_this();
    }

private: 
	void decode_data(const uint64_t decode_start, const uint64_t decode_end, srd_session *const session);
	void execute_decode_stack();
	static void annotation_callback(srd_proto_data *pdata, void *self);
    static void analog_callback(srd_proto_data *pdata, void *self);
    void do_decode_work();

    // P0-A: Centralised error-message setter that emits error_message_changed
    // via event_bus_post (thread-safe, shared_ptr-captured).  All assignments
    // to _error_message MUST go through this helper.
    void set_error_message(const QString &msg);
  
signals:
	void new_decode_data();
    void decode_done();
    void error_message_changed(const QString &msg);
  
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
 
    // P3-11 fix: _stask_stauts is protected by _status_mutex instead of
    // std::atomic_load/store (which is a legacy C++11 workaround). This
    // is simpler and more maintainable.
    std::shared_ptr<decode_task_status> _stask_stauts;
    mutable std::mutex _status_mutex;

    // P1-4 fix: Split the single _output_mutex into two focused locks:
    //   _rows_mutex  — shared_mutex protecting _rows, _class_rows,
    //                  _rows_gshow, _rows_lshow. Read-heavy (UI rendering),
    //                  so shared_mutex allows concurrent reads.
    //   _state_mutex — mutex protecting _error_message, _samples_decoded.
    //                  Write-heavy (decode thread updates frequently).
    mutable std::shared_mutex _rows_mutex;
    mutable std::mutex _state_mutex;

    // P0-1 fix: Condition variable replaces sleep_for(100ms) polling.
    // The decode thread waits on _data_cond when no data is available;
    // the data feed calls notify_data_ready() to wake it immediately.
    std::condition_variable _data_cond;
    std::mutex _data_wait_mutex;

    bool            _is_capture_end;
    std::atomic<int> _progress{0};
    std::atomic<bool> _is_decoding{false};
    std::atomic<uint64_t> _result_count{0};

    // decoder-generated analog samples (TDM/PWM).
    std::vector<std::shared_ptr<DecoderAnalogData>> _analog_data;
    mutable std::mutex _analog_mutex;
    bool _analog_visible = true;

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
