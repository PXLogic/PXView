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

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <shared_mutex>

#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"
#include "pv/ui/langresource.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/data/decode/annotation.h"
#include "pv/data/decode/decoder.h"
#include "pv/data/decode/rowdata.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/data/model/signalmodel.h"
#include <ds_types.h>

using namespace pv::data::decode;
using namespace std;

namespace {
// Static error message constants for decode worker thread.
// These avoid accessing LangResource (which may trigger page loading)
// from non-main threads. C++11 magic statics guarantee thread-safe
// initialization on first access.
const std::string s_kRequiredChannelsMissing =
    "One or more required channels have not been specified";
const std::string s_kChannelsNotEnabled =
    "At least one of selected channels are not enabled.";
const std::string s_kCreateDecoderInstanceFailed =
    "Failed to create decoder instance";
}  // namespace

namespace pv {
namespace data {

const double DecoderStack::DecodeMargin = 1.0;
const double DecoderStack::DecodeThreshold = 0.2;
const int64_t DecoderStack::DecodeChunkLength = 4 * 1024;
const unsigned int DecoderStack::DecodeNotifyPeriod = 1024;

DecoderStack::DecoderStack(pv::SigSession *session,
                           const srd_decoder *const dec,
                           DecoderStatus *decoder_status)
    : _session(session) {
  if (!session) {
    pxv_warn("%s", "DecoderStack::DecoderStack: session is nullptr");
    throw std::invalid_argument("DecoderStack: session is nullptr");
  }
  if (!dec) {
    pxv_warn("%s", "DecoderStack::DecoderStack: dec is nullptr");
    throw std::invalid_argument("DecoderStack: dec is nullptr");
  }
  if (!decoder_status) {
    pxv_warn("%s", "DecoderStack::DecoderStack: decoder_status is nullptr");
    throw std::invalid_argument("DecoderStack: decoder_status is nullptr");
  }
  assert(session);
  assert(dec);
  assert(decoder_status);

  _samples_decoded = 0;
  _sample_count.store(0);
  _decode_state.store(Stopped);
  _options_changed = false;
  _no_memory = false;
  _mark_index = -1;
  _decoder_status.reset(decoder_status);
  _stask_stauts = nullptr;  // shared_ptr<decode_task_status> — null init
  _is_capture_end = true;
  _snapshot.reset();
  _progress.store(0);
  _is_decoding.store(false);
  _result_count.store(0);
  _owner_document = nullptr;

  // TS-3 fix: use make_unique for exception safety — if build_row() throws,
  // the unique_ptr automatically frees the Decoder.
  _stack.push_back(std::make_unique<decode::Decoder>(dec));

  build_row();
}

DecoderStack::~DecoderStack() {
  // TS-3 fix: unique_ptr handles all deletion — no manual delete needed.
  // _decoder_status is freed by unique_ptr destructor.
  // _rows elements are freed by unique_ptr destructors when _rows.clear() runs.
  // _stack elements are freed by unique_ptr destructors when _stack.clear() runs.

  // Clear row data (annotations) before the RowData objects are freed.
  for (auto &kv : _rows) {
    if (kv.second)
      kv.second->clear();
  }
  _rows.clear();
  _stack.clear();

  _rows_gshow.clear();
  _rows_lshow.clear();
  _class_rows.clear();
}

void DecoderStack::add_sub_decoder(std::unique_ptr<decode::Decoder> decoder) {
  if (!decoder) {
    pxv_warn("%s", "DecoderStack::add_sub_decoder: decoder is nullptr");
    return;
  }
  _stack.push_back(std::move(decoder));
  build_row();
  _options_changed = true;
}

void DecoderStack::remove_sub_decoder(Decoder *decoder) {
  // Find the decoder in the stack
  auto iter = _stack.begin();
  for (unsigned int i = 0; i < _stack.size(); i++, iter++)
    if (iter->get() == decoder)
      break;

  // Erase the element — unique_ptr auto-deletes the Decoder.
  if (iter != _stack.end()) {
    _stack.erase(iter);
  }

  build_row();
  _options_changed = true;
}

void DecoderStack::remove_decoder_by_handel(const srd_decoder *dec) {
  Decoder *decoder = nullptr;

  for (auto &up : _stack) {
    auto d = up.get();
    if (d->get_dec_handel() == dec) {
      decoder = d;
      break;
    }
  }

  if (decoder) {
    remove_sub_decoder(decoder);
  }
}

void DecoderStack::build_row() {
  // Protect the map rebuild so concurrent readers (annotation_callback on
  // the decode thread, UI-thread read methods) don't see a half-rebuilt map.
  std::lock_guard<std::mutex> lock(_output_mutex);

  // release source — unique_ptr handles deletion of old RowData objects.
  for (auto &kv : _rows) {
    if (kv.second)
      kv.second->clear();
  }
  _rows.clear();

  // Add classes
  for (auto &up : _stack) {
    auto dec = up.get();
    const srd_decoder *const decc = dec->decoder();
    assert(dec->decoder());

    dec->reset_start();

    // Add a row for the decoder if it doesn't have a row list
    if (!decc->annotation_rows) {
      const Row row(decc);
      _rows[row] = std::make_unique<decode::RowData>();
      std::map<const decode::Row, bool>::const_iterator iter =
          _rows_gshow.find(row);
      if (iter == _rows_gshow.end()) {
        _rows_gshow[row] = true;
        if (row.title().contains("bit", Qt::CaseInsensitive) ||
            row.title().contains("warning", Qt::CaseInsensitive)) {
          _rows_lshow[row] = false;
        } else {
          _rows_lshow[row] = true;
        }
      }
    }

    // Add the decoder rows
    int order = 0;
    for (const GSList *l = decc->annotation_rows; l; l = l->next) {
      const srd_decoder_annotation_row *const ann_row =
          (srd_decoder_annotation_row *)l->data;
      if (!ann_row) {
        pxv_warn("%s", "DecoderStack::build_row: ann_row is nullptr, skipping");
        continue;
      }
      assert(ann_row);

      const Row row(decc, ann_row, order);

      // Add a new empty row data object
      _rows[row] = std::make_unique<decode::RowData>();
      std::map<const decode::Row, bool>::const_iterator iter =
          _rows_gshow.find(row);
      if (iter == _rows_gshow.end()) {
        _rows_gshow[row] = true;
        if (row.title().contains("bit", Qt::CaseInsensitive) ||
            row.title().contains("warning", Qt::CaseInsensitive)) {
          _rows_lshow[row] = false;
        } else {
          _rows_lshow[row] = true;
        }
      }

      // Map out all the classes
      for (const GSList *ll = ann_row->ann_classes; ll; ll = ll->next) {
        _class_rows[make_pair(decc, GPOINTER_TO_INT(ll->data))] = Row(row);
      }

      order++;
    }
  }
}

int64_t DecoderStack::samples_decoded() {
  std::lock_guard<std::mutex> decode_lock(_output_mutex);
  return _samples_decoded;
}

void DecoderStack::get_annotation_subset(
    std::vector<pv::data::decode::Annotation *> &dest, const Row &row,
    uint64_t start_sample, uint64_t end_sample) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    (*iter).second->get_annotation_subset(dest, start_sample, end_sample);
}

decode::RowData* DecoderStack::get_row_data(const decode::Row &row)
{
    std::lock_guard<std::mutex> lock(_output_mutex);
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second;
    return nullptr;
}

uint64_t DecoderStack::get_annotation_index(const Row &row,
                                            uint64_t start_sample) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  uint64_t index = 0;
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    index = (*iter).second->get_annotation_index(start_sample);

  return index;
}

std::pair<size_t, size_t> DecoderStack::get_visible_range(
    const Row &row, uint64_t start_sample, uint64_t end_sample) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  std::pair<size_t, size_t> range{0, 0};
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    range = (*iter).second->get_visible_range(start_sample, end_sample);

  return range;
}

uint64_t DecoderStack::get_max_annotation(const Row &row) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    return (*iter).second->get_max_annotation();

  return 0;
}

uint64_t DecoderStack::get_min_annotation(const Row &row) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    return (*iter).second->get_min_annotation();

  return 0;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_gshow() {
  std::lock_guard<std::mutex> lock(_output_mutex);
  std::map<const decode::Row, bool> rows_gshow;
  for (std::map<const decode::Row, bool>::const_iterator i =
           _rows_gshow.begin();
       i != _rows_gshow.end(); i++) {
    rows_gshow[(*i).first] = (*i).second;
  }
  return rows_gshow;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_lshow() {
  std::lock_guard<std::mutex> lock(_output_mutex);
  std::map<const decode::Row, bool> rows_lshow;
  for (std::map<const decode::Row, bool>::const_iterator i =
           _rows_lshow.begin();
       i != _rows_lshow.end(); i++) {
    rows_lshow[(*i).first] = (*i).second;
  }
  return rows_lshow;
}

void DecoderStack::set_rows_gshow(const decode::Row row, bool show) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  std::map<const decode::Row, bool>::const_iterator iter =
      _rows_gshow.find(row);
  if (iter != _rows_gshow.end()) {
    _rows_gshow[row] = show;
  }
}

void DecoderStack::set_rows_lshow(const decode::Row row, bool show) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  std::map<const decode::Row, bool>::const_iterator iter =
      _rows_lshow.find(row);
  if (iter != _rows_lshow.end()) {
    _rows_lshow[row] = show;
  }
}

bool DecoderStack::has_annotations(const Row &row) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    if (0 == (*iter).second->get_max_sample())
      return false;
    else
      return true;
  else
    return false;
}

uint64_t DecoderStack::list_annotation_size() {
  std::lock_guard<std::mutex> lock(_output_mutex);
  uint64_t max_annotation_size = 0;

  for (auto it = _rows.begin(); it != _rows.end(); it++) {
    auto iter = _rows_lshow.find((*it).first);
    if (iter != _rows_lshow.end() && (*iter).second) {
      max_annotation_size =
          max(max_annotation_size, (*it).second->get_annotation_size());
    }
  }

  return max_annotation_size;
}

uint64_t DecoderStack::list_annotation_size(uint16_t row_index) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    auto iter = _rows_lshow.find((*i).first);
    if (iter != _rows_lshow.end() && (*iter).second)
      if (row_index-- == 0) {
        return (*i).second->get_annotation_size();
      }
  }
  return 0;
}

bool DecoderStack::list_annotation(pv::data::decode::Annotation *ann,
                                   uint16_t row_index, uint64_t col_index) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    auto iter = _rows_lshow.find((*i).first);
    if (iter != _rows_lshow.end() && (*iter).second) {
      if (row_index-- == 0) {
        return (*i).second->get_annotation(ann, col_index);
      }
    }
  }

  return false;
}

bool DecoderStack::list_row_title(int row, QString &title) {
  std::lock_guard<std::mutex> lock(_output_mutex);
  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    auto iter = _rows_lshow.find((*i).first);
    if (iter != _rows_lshow.end() && (*iter).second) {
      if (row-- == 0) {
        title = (*i).first.title();
        return 1;
      }
    }
  }
  return 0;
}

void DecoderStack::clear() { init(); }

void DecoderStack::init() {
  _sample_count.store(0);
  _samples_decoded = 0;
  {
    std::lock_guard<std::mutex> lk(_output_mutex);
    _error_message = QString();
  }
  _no_memory = false;
  _snapshot.reset();
  _result_count.store(0);
  _ann_dropped_stop.store(0);
  _ann_dropped_mem.store(0);
  _ann_dropped_row.store(0);

  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    (*i).second->clear();
  }

  set_mark_index(-1);
}

void DecoderStack::stop_decode_work() {
  // set the flag to exit from task thread
  // atomic_load so that if do_decode_work() on the decode thread is
  // simultaneously replacing _stask_stauts, we still get a valid shared_ptr
  // copy (the old object stays alive until our copy is released).
  auto status = std::atomic_load(&_stask_stauts);
  if (status) {
    status->_bStop = true;
  }
  _decode_state.store(Stopped, std::memory_order_release);
}

void DecoderStack::begin_decode_work() {
  // 防御性检查:若已有解码线程在运行(RevEndPacket 与 CopyToDocDone 竞态,
  // 或 add_decode_task 重复添加遗漏),直接返回避免状态被覆盖。
  // assert 在 Release 下是空操作,必须显式 if 检查 + early return。
  if (_decode_state.load(std::memory_order_acquire) != Stopped)
    return;
  {
    std::lock_guard<std::mutex> lk(_output_mutex);
    _error_message = "";
  }
  _decode_state.store(Running, std::memory_order_release);
  do_decode_work();
  _decode_state.store(Stopped, std::memory_order_release);
}

bool DecoderStack::check_required_probes() {
  for (auto &up : _stack) {
    auto dec = up.get();
    if (!dec->have_required_probes()) {
      return false;
    }
  }

  return true;
}

void DecoderStack::do_decode_work() {
  // set the flag to exit from task thread
  // atomic_load/store so that concurrent stop_decode_work() on another
  // thread can safely read the old shared_ptr while we replace it.
  auto old = std::atomic_load(&_stask_stauts);
  if (old) {
    old->_bStop = true;
  }
  auto new_status = std::make_shared<decode_task_status>();
  new_status->_bStop = false;
  new_status->_decoder = this;
  std::atomic_store(&_stask_stauts, new_status);
  // old shared_ptr is released here — the object is freed only if no
  // concurrent stop_decode_work() holds a copy. This fixes the leak where
  // the previous raw-new object was never deleted.
  _decoder_status->clear(); // clear old items

  if (!_options_changed) {
    return;
  }
  _options_changed = false;

  init();

  _snapshot.reset();

  pxv_info("DecoderStack::do_decode_work: _stack size=%zu, checking required probes", _stack.size());

  if (!check_required_probes()) {
    {
      std::lock_guard<std::mutex> lk(_output_mutex);
      _error_message =
          QString::fromStdString(s_kRequiredChannelsMissing);
    }
    pxv_err("ERROR:%s", error_message().toStdString().c_str());
    // Diagnostic: log which decoder has missing required probes
    for (auto &up : _stack) {
      auto dec = up.get();
      if (!dec->have_required_probes()) {
        pxv_err("ERROR:Decoder %p is missing required probes", dec);
      }
    }
    return;
  }

  // Take a snapshot of signal_models under a shared_lock so concurrent
  // writers (init_signals on the UI thread) cannot invalidate our iterators.
  std::vector<std::shared_ptr<data::SignalModel>> models_snapshot;
  {
    std::shared_lock<std::shared_mutex> lk(_session->signal_models_mutex());
    models_snapshot = _session->get_signal_models();
  }

  pxv_info("DecoderStack::do_decode_work: required probes OK, signal_models count=%zu",
           models_snapshot.size());

  for (auto &up : _stack) {
    auto dec = up.get();
    if (dec->have_probes()) {
      int probe_idx = dec->first_probe_index();
      pxv_info("DecoderStack::do_decode_work: decoder %p has probes, first_probe_index=%d, checking %zu signal_models",
               dec, probe_idx, models_snapshot.size());

      for (auto m : models_snapshot) {
        bool index_match = (m->index() == probe_idx);
        bool type_match = (m->type() == SR_CHANNEL_LOGIC);
        bool snapshot_ok = (m->snapshot() != nullptr);

        pxv_info("  model: index=%d, type=%d (Logic=%d), snapshot=%p, index_match=%d, type_match=%d, snapshot_ok=%d",
                 m->index(), (int)m->type(), (int)SR_CHANNEL_LOGIC,
                 m->snapshot().get(), index_match, type_match, snapshot_ok);

        if (index_match && type_match) {
          _snapshot = std::static_pointer_cast<pv::data::LogicSnapshot>(m->snapshot());
          pxv_info("DecoderStack::do_decode_work: found matching model! _snapshot=%p", _snapshot.get());
          if (_snapshot != nullptr)
            break;
        }
      }
      if (_snapshot != nullptr)
        break;
    } else {
      pxv_info("DecoderStack::do_decode_work: decoder %p has no probes, skipping", up.get());
    }
  }

  if (_snapshot == nullptr) {
    {
      std::lock_guard<std::mutex> lk(_output_mutex);
      _error_message =
          QString::fromStdString(s_kRequiredChannelsMissing);
    }
    pxv_err("ERROR:%s", error_message().toStdString().c_str());
    pxv_err("ERROR:Failed to find matching LogicSnapshot for any decoder probe");
    return;
  }

  if (_session->is_realtime_refresh() == false && _snapshot->empty()) {
    pxv_err("ERROR:Decode data is empty.");
    return;
  }

  // Get the samplerate
  _samplerate = _snapshot->samplerate();
  if (_samplerate == 0.0) {
    pxv_err("ERROR:Decode data got an invalid sample rate.");
    return;
  }

  execute_decode_stack();
}

uint64_t DecoderStack::get_max_sample_count() {
  std::lock_guard<std::mutex> lock(_output_mutex);
  uint64_t max_sample_count = 0;

  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    max_sample_count = max(max_sample_count, (*i).second->get_max_sample());
  }

  return max_sample_count;
}

void DecoderStack::decode_data(const uint64_t decode_start,
                               const uint64_t decode_end,
                               srd_session *const session) {
  decode_task_status *status = _stask_stauts.get();

  // uint8_t *chunk = nullptr;
  uint64_t last_cnt = 0;
  uint64_t notify_cnt = (decode_end - decode_start + 1) / 1000;
  if (notify_cnt == 0) notify_cnt = 1;
  srd_decoder_inst *logic_di = nullptr;

  // find the first level decoder instant
  for (GSList *d = session->di_list; d; d = d->next) {
    srd_decoder_inst *di = (srd_decoder_inst *)d->data;
    srd_decoder *decoder = di->decoder;
    const bool have_probes = (decoder->channels || decoder->opt_channels) != 0;
    if (have_probes) {
      logic_di = di;
      break;
    }
  }

  if (!logic_di) {
    pxv_warn("%s", "DecoderStack::decode_data: logic_di is nullptr");
    return;
  }
  assert(logic_di);

  uint64_t entry_cnt = 0;
  uint64_t i = decode_start;
  char *error = nullptr;
  bool bError = false;
  bool bEndTime = false;
  // struct srd_push_param push_param;

  if (i >= decode_end) {
    pxv_info("decode data index have been to end");
  }

  std::vector<const uint8_t *> chunk;
  std::vector<uint8_t> chunk_const;

  bool bCheckEnd = false;
  uint64_t end_index = decode_end;

  _progress.store(0);
  uint64_t sended_len = 0;
  _is_decoding.store(true);

  void *lbp_array[35];

  for (int j = 0; j < logic_di->dec_num_channels; j++) {
    lbp_array[j] = nullptr;
  }

  while (i < end_index && !_no_memory && !status->_bStop) {
    chunk.clear();
    chunk_const.clear();

    if (_is_capture_end) {
      if (!bCheckEnd) {
        bCheckEnd = true;

        uint64_t align_sample_count = _snapshot->get_sample_count();
        pxv_info("DecoderStack debug: bCheckEnd triggered. get_sample_count() = %llu, end_index = %llu",
                 (unsigned long long)align_sample_count, (unsigned long long)end_index);

        if (end_index >= align_sample_count) {
          end_index = align_sample_count > 0 ? align_sample_count - 1 : 0;
          pxv_info("Reset the decode end sample, new:%llu, old:%llu",
                   (u64_t)end_index, (u64_t)decode_end);
        }

        if (i >= align_sample_count) {
          pxv_info("ERROR: the decoding sample index is out of range.");
          break;
        }
      }
    } else if (i >= _snapshot->get_sample_count()) {
      // Wait the data is ready.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (_is_capture_end && i >= _snapshot->get_sample_count()) {
      break;
    }

    uint64_t chunk_end = end_index;

    for (int j = 0; j < logic_di->dec_num_channels; j++) {
      int sig_index = logic_di->dec_channelmap[j];
      void *lbp = nullptr;

      if (sig_index == -1) {
        chunk.push_back(nullptr);
        chunk_const.push_back(0);
      } else {
        if (_snapshot->has_data(sig_index)) {
          const uint8_t *data_ptr =
              _snapshot->get_samples(i, chunk_end, sig_index, &lbp);
          chunk.push_back(data_ptr);
          chunk_const.push_back(_snapshot->get_sample(i, sig_index));

          if (_snapshot->is_able_free() == false) {
            if (lbp_array[j] != lbp) {
              if (lbp_array[j] != nullptr)
                _snapshot->free_decode_lpb(lbp_array[j]);
              lbp_array[j] = lbp;
            }
          }
        } else {
          {
            std::lock_guard<std::mutex> lk(_output_mutex);
            _error_message =
                QString::fromStdString(s_kChannelsNotEnabled);
          }
          return;
        }
      }
    }

    if (chunk_end > end_index)
      chunk_end = end_index;
    if (chunk_end - i > MaxChunkSize)
      chunk_end = i + MaxChunkSize;

    bEndTime = (chunk_end == end_index);

    if (srd_session_send(session, i, chunk_end, chunk.data(),
                         chunk_const.data(), chunk_end - i, &error) != SRD_OK) {

      if (error) {
        {
          std::lock_guard<std::mutex> lk(_output_mutex);
          _error_message = QString::fromLocal8Bit(error);
        }
        pxv_err("Failed to call srd_session_send:%s", error);
        g_free(error);
        error = nullptr;
      }

      bError = true;
      break;
    }

    sended_len += chunk_end - i;
    _progress.store((int)(sended_len * 100 / end_index));

    i = chunk_end;

    // use mutex
    {
      std::lock_guard<std::mutex> lock(_output_mutex);
      _samples_decoded = i - decode_start + 1;
    }

    if ((i - last_cnt) > notify_cnt) {
      last_cnt = i;
      // CRITICAL: Must NOT emit new_decode_data() signal directly here — this
      // runs on the decode worker thread. Qt AutoConnection cross-thread
      // signal emission calls QThread::currentThread() → creates QThreadData
      // on the worker thread → SIGSEGV on thread exit (LdrShutdownThread).
      // Post the emit to the main thread via postEvent so AutoConnection
      // resolves to DirectConnection (same thread) — no QThreadData created.
      _session->event_bus_post([this]() { new_decode_data(); });
    }

    entry_cnt++;
  }

  _progress.store(100);
  _is_decoding.store(false);

  // Final progress notification via postEvent (not direct signal emit) — see above.
  _session->event_bus_post([this]() { new_decode_data(); });

  // the task is normal ends,so all samples was processed;
  if (!bError && bEndTime) {
    srd_session_end(session, &error);

    if (error != nullptr) {
      {
        std::lock_guard<std::mutex> lk(_output_mutex);
        _error_message = QString::fromLocal8Bit(error);
      }
      pxv_err("Failed to call srd_session_end:%s", error);
    }
  }

  if (error != nullptr)
    g_free(error);

  if (!_session->is_closed())
    // Post to main thread — see new_decode_data comment above.
    _session->event_bus_post([this]() { decode_done(); });
}

void DecoderStack::execute_decode_stack() {
  srd_session *session = nullptr;
  srd_decoder_inst *prev_di = nullptr;
  uint64_t decode_start = 0;
  uint64_t decode_end = 0;

  if (!_snapshot) {
    pxv_warn("%s", "DecoderStack::execute_decode_stack: _snapshot is nullptr");
    return;
  }
  assert(_snapshot);

  // Create the session
  // one decoderstatck onwer one session
  // all decoderstatck execute in sequence
  srd_session_new(&session);

  if (session == nullptr) {
    pxv_err("Failed to call srd_session_new()");
    return;
  }

  _sample_count.store(_snapshot->get_sample_count());

  // Create the decoders
  for (auto &up : _stack) {
    auto dec = up.get();
    srd_decoder_inst *const di = dec->create_decoder_inst(session);

    if (!di) {
      {
        std::lock_guard<std::mutex> lk(_output_mutex);
        _error_message =
            QString::fromStdString(s_kCreateDecoderInstanceFailed);
      }
      srd_session_destroy(session);
      return;
    }

    if (prev_di)
      srd_inst_stack(session, prev_di, di);

    prev_di = di;
    decode_start = dec->decode_start();

    if (_session->is_realtime_refresh() == false) {
      // If decode_end is 0 (not explicitly set, e.g. when added via MCP
      // with silent=true which skips create_popup), use the full data range.
      uint64_t dec_end = dec->decode_end();
      if (dec_end == 0)
        dec_end = _sample_count - 1;
      decode_end = min(dec_end, _sample_count - 1);
    } else {
      // In realtime refresh mode (stream/single mode, e.g. demo/file devices),
      // data arrives incrementally. If decode_end is 0 (meaning "decode to end"),
      // use UINT64_MAX so the decode loop waits for data and is bounded by
      // _is_capture_end check in decode_data() (which sets end_index to
      // align_sample_count - 1 when capture ends).
      // Without this, decode_end stays 0 and the decode loop never executes,
      // causing "send to decoder times: 0" and no decode results.
      uint64_t dec_end = dec->decode_end();
      if (dec_end == 0)
        dec_end = UINT64_MAX;
      decode_end = max(dec_end, decode_end);
    }
  }

  // Start the session
  srd_session_metadata_set(session, SRD_CONF_SAMPLERATE,
                           g_variant_new_uint64((uint64_t)_samplerate));

  srd_pd_output_callback_add(session, SRD_OUTPUT_ANN,
                             DecoderStack::annotation_callback, _stask_stauts.get());

  char *error = nullptr;
  int srd_ret = srd_session_start(session, &error);

  if (srd_ret == SRD_OK) {
    // need a lot time
    decode_data(decode_start, decode_end, session);
  } else if (error != nullptr) {
    {
      std::lock_guard<std::mutex> lk(_output_mutex);
      _error_message = QString::fromLocal8Bit(error);
    }
  }

  // Destroy the session
  if (error != nullptr) {
    g_free(error);
  }

  srd_session_destroy(session);
}

uint64_t DecoderStack::sample_count() {
  if (_snapshot)
    return _snapshot->get_sample_count();
  else
    return 0;
}

uint64_t DecoderStack::sample_rate() { return _samplerate; }

// the decode callback, annotation object will be create
void DecoderStack::annotation_callback(srd_proto_data *pdata, void *self) {
  if (!pdata) {
    pxv_warn("%s", "DecoderStack::annotation_callback: pdata is nullptr");
    return;
  }
  if (!self) {
    pxv_warn("%s", "DecoderStack::annotation_callback: self is nullptr");
    return;
  }
  assert(pdata);
  assert(self);

  struct decode_task_status *st = (decode_task_status *)self;

  DecoderStack *const d = st->_decoder;
  if (!d) {
    pxv_warn("%s", "DecoderStack::annotation_callback: d is nullptr");
    return;
  }
  assert(d);

  if (st->_bStop) {
    d->_ann_dropped_stop++;
    return;
  }
  if (!d->_decoder_status) {
    pxv_err("decode task was deleted.");
    return;
  }

  if (d->_no_memory) {
    d->_ann_dropped_mem++;
    return;
  }

  Annotation *a = new Annotation(pdata, d->_decoder_status.get());
  if (a == nullptr) {
    d->_no_memory = true;
    return;
  }
  d->_result_count++;

  // Find the row
  assert(pdata->pdo);
  assert(pdata->pdo->di);
  const srd_decoder *const decc = pdata->pdo->di->decoder;
  if (!decc) {
    pxv_warn("%s", "DecoderStack::annotation_callback: decc is nullptr");
    return;
  }
  assert(decc);

  // Protect _rows/_class_rows lookups AND push_annotation against concurrent
  // build_row() on the UI thread. The lock must be held during push_annotation
  // because build_row() deletes RowData objects — releasing the lock before
  // push_annotation would leave target_row dangling.
  // Lock order: _output_mutex -> _visitor_mutex (inside push_annotation) is
  // consistent with build_row() -> RowData::clear().
  {
    std::lock_guard<std::mutex> lk(d->_output_mutex);

    auto row_iter = d->_rows.end();

    // Try looking up the sub-row of this class
    const map<pair<const srd_decoder *, int>, Row>::const_iterator r =
        d->_class_rows.find(make_pair(decc, a->format()));
    if (r != d->_class_rows.end())
      row_iter = d->_rows.find((*r).second);
    else {
      // Failing that, use the decoder as a key
      row_iter = d->_rows.find(Row(decc));
    }

    if (row_iter == d->_rows.end()) {
      // Row not found — map may have been rebuilt by build_row(). Drop the
      // annotation rather than crashing on the assert.
      pxv_err("Unexpected annotation: decoder = 0x%x, format = %d", (void *)decc,
              a->format());
      d->_ann_dropped_row++;
      delete a;
      return;
    }

    // Add the annotation while still holding _output_mutex
    if (!(*row_iter).second->push_annotation(a))
      d->_no_memory = true;
  }
}

void DecoderStack::frame_ended() {
  _options_changed = true;

  if (_session) {
    const uint64_t limit = _session->get_ring_sample_count();
    const uint64_t last_samples = limit > 0 ? limit - 1 : 0;

  for (auto &up : _stack) {
    auto dec = up.get();
    uint64_t start = dec->decode_start();
      uint64_t end = dec->decode_end();
      const uint64_t raw_start = start;
      const uint64_t raw_end = end;

      if (start > last_samples) {
        start = 0;
      }

      // end == 0 is a sentinel meaning "decode to the actual data end".
      // Do NOT replace it with last_samples (ring buffer capacity) here,
      // because that would permanently overwrite the sentinel in the
      // decoder's stored config. execute_decode_stack() resolves 0 to the
      // real _sample_count at decode time, adapting to varying capture
      // lengths. Only clamp non-zero values that exceed the buffer.
      if (end != 0 && end > last_samples) {
        end = last_samples;
      }

      dec->set_decode_region(start, end);
    }
  }
}

int DecoderStack::list_rows_size() {
  std::lock_guard<std::mutex> lock(_output_mutex);
  int rows_size = 0;
  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    auto iter = _rows_lshow.find((*i).first);
    if (iter != _rows_lshow.end() && (*iter).second)
      rows_size++;
  }
  return rows_size;
}

bool DecoderStack::options_changed() { return _options_changed; }

void DecoderStack::set_options_changed(bool changed) {
  _options_changed = changed;
}

bool DecoderStack::out_of_memory() { return _no_memory; }

void DecoderStack::set_mark_index(int64_t index) { _mark_index = index; }

int64_t DecoderStack::get_mark_index() { return _mark_index; }

const char *DecoderStack::get_root_decoder_id() {
  if (_stack.size() > 0) {
    decode::Decoder *dec = _stack.front().get();
    return dec->decoder()->id;
  }
  return nullptr;
}

} // namespace data
} // namespace pv
