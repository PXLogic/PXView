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
#include <chrono>

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
  _stask_stauts = nullptr;
  _is_capture_end = true;
  _snapshot.reset();
  _progress.store(0);
  _is_decoding.store(false);
  _result_count.store(0);
  _owner_document = nullptr;

  _stack.push_back(std::make_unique<decode::Decoder>(dec));

  build_row();
}

DecoderStack::~DecoderStack() {
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
  auto iter = _stack.begin();
  for (unsigned int i = 0; i < _stack.size(); i++, iter++)
    if (iter->get() == decoder)
      break;

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
  // P1-4 fix: use _rows_mutex (unique lock) instead of _output_mutex
  std::unique_lock<std::shared_mutex> lock(_rows_mutex);

  for (auto &kv : _rows) {
    if (kv.second)
      kv.second->clear();
  }
  _rows.clear();

  for (auto &up : _stack) {
    auto dec = up.get();
    const srd_decoder *const decc = dec->decoder();
    assert(dec->decoder());

    dec->reset_start();

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

      for (const GSList *ll = ann_row->ann_classes; ll; ll = ll->next) {
        _class_rows[make_pair(decc, GPOINTER_TO_INT(ll->data))] = Row(row);
      }

      order++;
    }
  }
}

int64_t DecoderStack::samples_decoded() {
  // P1-4 fix: use _state_mutex instead of _output_mutex
  std::lock_guard<std::mutex> decode_lock(_state_mutex);
  return _samples_decoded;
}

void DecoderStack::get_annotation_subset(
    std::vector<const pv::data::decode::Annotation *> &dest, const Row &row,
    uint64_t start_sample, uint64_t end_sample) {
  // P1-4 fix: use shared_lock on _rows_mutex for concurrent read access
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    (*iter).second->get_annotation_subset(dest, start_sample, end_sample);
}

decode::RowData* DecoderStack::get_row_data(const decode::Row &row)
{
    std::shared_lock<std::shared_mutex> lock(_rows_mutex);
    auto iter = _rows.find(row);
    if (iter != _rows.end())
        return (*iter).second.get();
    return nullptr;
}

uint64_t DecoderStack::get_annotation_index(const Row &row,
                                            uint64_t start_sample) {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  uint64_t index = 0;
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    index = (*iter).second->get_annotation_index(start_sample);

  return index;
}

std::pair<size_t, size_t> DecoderStack::get_visible_range(
    const Row &row, uint64_t start_sample, uint64_t end_sample) {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  std::pair<size_t, size_t> range{0, 0};
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    range = (*iter).second->get_visible_range(start_sample, end_sample);

  return range;
}

uint64_t DecoderStack::get_max_annotation(const Row &row) {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    return (*iter).second->get_max_annotation();

  return 0;
}

uint64_t DecoderStack::get_min_annotation(const Row &row) {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    return (*iter).second->get_min_annotation();

  return 0;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_gshow() {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  std::map<const decode::Row, bool> rows_gshow;
  for (std::map<const decode::Row, bool>::const_iterator i =
           _rows_gshow.begin();
       i != _rows_gshow.end(); i++) {
    rows_gshow[(*i).first] = (*i).second;
  }
  return rows_gshow;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_lshow() {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  std::map<const decode::Row, bool> rows_lshow;
  for (std::map<const decode::Row, bool>::const_iterator i =
           _rows_lshow.begin();
       i != _rows_lshow.end(); i++) {
    rows_lshow[(*i).first] = (*i).second;
  }
  return rows_lshow;
}

void DecoderStack::set_rows_gshow(const decode::Row row, bool show) {
  std::unique_lock<std::shared_mutex> lock(_rows_mutex);
  std::map<const decode::Row, bool>::const_iterator iter =
      _rows_gshow.find(row);
  if (iter != _rows_gshow.end()) {
    _rows_gshow[row] = show;
  }
}

void DecoderStack::set_rows_lshow(const decode::Row row, bool show) {
  std::unique_lock<std::shared_mutex> lock(_rows_mutex);
  std::map<const decode::Row, bool>::const_iterator iter =
      _rows_lshow.find(row);
  if (iter != _rows_lshow.end()) {
    _rows_lshow[row] = show;
  }
}

bool DecoderStack::has_annotations(const Row &row) {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
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
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
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
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
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
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
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
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
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

bool DecoderStack::list_row_description(int row, QString &desc) {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    auto iter = _rows_lshow.find((*i).first);
    if (iter != _rows_lshow.end() && (*iter).second) {
      if (row-- == 0) {
        desc = (*i).first.description();
        return 1;
      }
    }
  }
  return 0;
}

QString DecoderStack::auto_label() const {
  if (_stack.empty() || !_session)
    return QString();
  auto *dec = _stack.front().get();
  if (!dec || !dec->have_probes())
    return QString();
  int probe_idx = dec->first_probe_index();
  if (probe_idx < 0)
    return QString();
  std::shared_lock<std::shared_mutex> lk(_session->signal_models_mutex());
  const auto &models = _session->get_signal_models();
  for (auto &m : models) {
    if (m && m->index() == probe_idx)
      return QString::fromStdString(m->name());
  }
  return QString();
}

void DecoderStack::clear() { init(); }

void DecoderStack::init() {
  _sample_count.store(0);
  _samples_decoded = 0;
  {
    std::lock_guard<std::mutex> lk(_state_mutex);
    _error_message = QString();
  }
  _no_memory = false;
  _snapshot.reset();
  _result_count.store(0);
  _ann_dropped_stop.store(0);
  _ann_dropped_mem.store(0);
  _ann_dropped_row.store(0);

  std::shared_lock<std::shared_mutex> lk(_rows_mutex);
  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    (*i).second->clear();
  }

  set_mark_index(-1);
}

void DecoderStack::stop_decode_work() {
  // P3-11 fix: use _status_mutex instead of atomic_load/store
  {
    std::lock_guard<std::mutex> lk(_status_mutex);
    if (_stask_stauts) {
      _stask_stauts->_bStop = true;
    }
  }
  _decode_state.store(Stopped, std::memory_order_release);
  // P0-1 fix: wake up the decode thread if it's waiting on the condition variable
  _data_cond.notify_all();
}

void DecoderStack::begin_decode_work() {
  if (_decode_state.load(std::memory_order_acquire) != Stopped)
    return;
  {
    std::lock_guard<std::mutex> lk(_state_mutex);
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
  // P3-11 fix: use _status_mutex for thread-safe access to _stask_stauts
  {
    std::lock_guard<std::mutex> lk(_status_mutex);
    if (_stask_stauts) {
      _stask_stauts->_bStop = true;
    }
    auto new_status = std::make_shared<decode_task_status>();
    new_status->_bStop = false;
    // P0-2 fix: use shared_from_this() instead of raw 'this' pointer
    new_status->_decoder = shared_from_this();
    _stask_stauts = new_status;
  }
  _decoder_status->clear();

  if (!_options_changed) {
    return;
  }
  _options_changed = false;

  init();

  _snapshot.reset();

  pxv_info("DecoderStack::do_decode_work: _stack size=%zu, checking required probes", _stack.size());

  if (!check_required_probes()) {
    {
      std::lock_guard<std::mutex> lk(_state_mutex);
      _error_message =
          QString::fromStdString(s_kRequiredChannelsMissing);
    }
    pxv_err("ERROR:%s", error_message().toStdString().c_str());
    for (auto &up : _stack) {
      auto dec = up.get();
      if (!dec->have_required_probes()) {
        pxv_err("ERROR:Decoder %p is missing required probes", dec);
      }
    }
    return;
  }

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
      std::lock_guard<std::mutex> lk(_state_mutex);
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

  _samplerate = _snapshot->samplerate();
  if (_samplerate == 0.0) {
    pxv_err("ERROR:Decode data got an invalid sample rate.");
    return;
  }

  execute_decode_stack();
}

uint64_t DecoderStack::get_max_sample_count() {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  uint64_t max_sample_count = 0;

  for (auto i = _rows.begin(); i != _rows.end(); i++) {
    max_sample_count = max(max_sample_count, (*i).second->get_max_sample());
  }

  return max_sample_count;
}

void DecoderStack::notify_data_ready() {
  // P0-1 fix: Wake up the decode thread which may be waiting in
  // decode_data() for new sample data to arrive.
  _data_cond.notify_all();
}

void DecoderStack::decode_data(const uint64_t decode_start,
                               const uint64_t decode_end,
                               srd_session *const session) {
  // P3-11 fix: obtain status shared_ptr under _status_mutex
  std::shared_ptr<decode_task_status> status;
  {
    std::lock_guard<std::mutex> lk(_status_mutex);
    status = _stask_stauts;
  }

  uint64_t last_cnt = 0;
  uint64_t notify_cnt = (decode_end - decode_start + 1) / 1000;
  if (notify_cnt == 0) notify_cnt = 1;
  srd_decoder_inst *logic_di = nullptr;

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
      // P0-1 fix: Use condition_variable instead of sleep_for(100ms) polling.
      // The decode thread waits until either:
      //   1) new data arrives (notify_data_ready() is called), or
      //   2) capture ends (set_capture_end_flag() notifies), or
      //   3) stop is requested (stop_decode_work() notifies), or
      //   4) a 1-second timeout expires (safety net to recheck state).
      std::unique_lock<std::mutex> wait_lk(_data_wait_mutex);
      _data_cond.wait_for(wait_lk, std::chrono::seconds(1),
          [this, &i]() {
            return i < _snapshot->get_sample_count() ||
                   _is_capture_end ||
                   (_stask_stauts && _stask_stauts->_bStop.load());
          });
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
            std::lock_guard<std::mutex> lk(_state_mutex);
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
          std::lock_guard<std::mutex> lk(_state_mutex);
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

    {
      std::lock_guard<std::mutex> lock(_state_mutex);
      _samples_decoded = i - decode_start + 1;
    }

    if ((i - last_cnt) > notify_cnt) {
      last_cnt = i;
      // P2-10 fix: Capture shared_ptr instead of raw 'this' to prevent
      // use-after-free if the DecoderStack is destroyed before the
      // lambda executes on the main thread.
      auto self = shared_from_this();
      _session->event_bus_post([self]() { self->new_decode_data(); });
    }

    entry_cnt++;
  }

  _progress.store(100);
  _is_decoding.store(false);

  // Final progress notification
  auto self = shared_from_this();
  _session->event_bus_post([self]() { self->new_decode_data(); });

  if (!bError && bEndTime) {
    srd_session_end(session, &error);

    if (error != nullptr) {
      {
        std::lock_guard<std::mutex> lk(_state_mutex);
        _error_message = QString::fromLocal8Bit(error);
      }
      pxv_err("Failed to call srd_session_end:%s", error);
    }
  }

  if (error != nullptr)
    g_free(error);

  if (!_session->is_closed()) {
    _session->event_bus_post([self]() { self->decode_done(); });
  }
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

  srd_session_new(&session);

  if (session == nullptr) {
    pxv_err("Failed to call srd_session_new()");
    return;
  }

  _sample_count.store(_snapshot->get_sample_count());

  for (auto &up : _stack) {
    auto dec = up.get();
    srd_decoder_inst *const di = dec->create_decoder_inst(session);

    if (!di) {
      {
        std::lock_guard<std::mutex> lk(_state_mutex);
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
      uint64_t dec_end = dec->decode_end();
      if (dec_end == 0)
        dec_end = _sample_count - 1;
      decode_end = min(dec_end, _sample_count - 1);
    } else {
      uint64_t dec_end = dec->decode_end();
      if (dec_end == 0)
        dec_end = UINT64_MAX;
      decode_end = max(dec_end, decode_end);
    }
  }

  srd_session_metadata_set(session, SRD_CONF_SAMPLERATE,
                           g_variant_new_uint64((uint64_t)_samplerate));

  // P3-11 fix: obtain status shared_ptr under _status_mutex for the callback
  std::shared_ptr<decode_task_status> status_for_callback;
  {
    std::lock_guard<std::mutex> lk(_status_mutex);
    status_for_callback = _stask_stauts;
  }

  srd_pd_output_callback_add(session, SRD_OUTPUT_ANN,
                             DecoderStack::annotation_callback,
                             status_for_callback.get());

  char *error = nullptr;
  int srd_ret = srd_session_start(session, &error);

  if (srd_ret == SRD_OK) {
    decode_data(decode_start, decode_end, session);
  } else if (error != nullptr) {
    {
      std::lock_guard<std::mutex> lk(_state_mutex);
      _error_message = QString::fromLocal8Bit(error);
    }
  }

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

// P2-7 fix: annotation_callback now uses emplace_annotation instead of
// new/delete. The Annotation is stored as a value in RowData's deque,
// eliminating manual memory management.
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

  // P0-2 fix: _decoder is now a shared_ptr, keeping the DecoderStack alive
  auto d = st->_decoder;  // copies the shared_ptr, ensuring lifetime
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

  d->_result_count++;

  assert(pdata->pdo);
  assert(pdata->pdo->di);
  const srd_decoder *const decc = pdata->pdo->di->decoder;
  if (!decc) {
    pxv_warn("%s", "DecoderStack::annotation_callback: decc is nullptr");
    return;
  }
  assert(decc);

  // P1-4 fix: use _rows_mutex instead of _output_mutex
  std::unique_lock<std::shared_mutex> lk(d->_rows_mutex);

  auto row_iter = d->_rows.end();

  // Determine the annotation class from the proto data
  const srd_proto_data_annotation *const pda =
      (const srd_proto_data_annotation *)pdata->data;
  int ann_format = pda->ann_class;

  const map<pair<const srd_decoder *, int>, Row>::const_iterator r =
      d->_class_rows.find(make_pair(decc, ann_format));
  if (r != d->_class_rows.end())
    row_iter = d->_rows.find((*r).second);
  else {
    row_iter = d->_rows.find(Row(decc));
  }

  if (row_iter == d->_rows.end()) {
    pxv_err("Unexpected annotation: decoder = 0x%x, format = %d", (void *)decc,
            ann_format);
    d->_ann_dropped_row++;
    return;
  }

  // P2-7 fix: emplace_annotation constructs the Annotation in-place
  // inside the deque — no new/delete, no memory pool needed.
  if (!(*row_iter).second->emplace_annotation(pdata, d->_decoder_status.get()))
    d->_no_memory = true;
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

      if (start > last_samples) {
        start = 0;
      }

      if (end != 0 && end > last_samples) {
        end = last_samples;
      }

      dec->set_decode_region(start, end);
    }
  }
}

int DecoderStack::list_rows_size() {
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
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
