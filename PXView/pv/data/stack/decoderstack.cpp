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
#include "pv/base/perflog.h"
#include "pv/config/appconfig.h"
#include "pv/data/isession_host.h"
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
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

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

DecoderStack::DecoderStack(pv::data::ISessionHost *host,
                           const srd_decoder *const dec,
                           DecoderStatus *decoder_status)
    : _host(host) {
  if (!host) {
    pxv_warn("%s", "DecoderStack::DecoderStack: host is nullptr");
    throw std::invalid_argument("DecoderStack: host is nullptr");
  }
  if (!dec) {
    pxv_warn("%s", "DecoderStack::DecoderStack: dec is nullptr");
    throw std::invalid_argument("DecoderStack: dec is nullptr");
  }
  if (!decoder_status) {
    pxv_warn("%s", "DecoderStack::DecoderStack: decoder_status is nullptr");
    throw std::invalid_argument("DecoderStack: decoder_status is nullptr");
  }
  assert(host);
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

  // Plan A: one dedicated heap per stack for annotation storage (deque +
  // frozen snapshot segments), so decode threads don't contend with the GUI
  // thread on the shared process heap (the ~1s heap-lock convoy).
  _annotation_heap = decode::make_annotation_heap();

  build_row();
  // Publish the initial (empty) snapshot so the render path and main-thread
  // readers never see a null _published before the first decode cycle.
  publish_snapshot();
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

// P0-A: Centralised error-message setter.  All _error_message assignments
// MUST go through this helper so that the error_message_changed signal is
// reliably emitted (via event_bus_post for thread safety).
void DecoderStack::set_error_message(const QString &msg) {
  // P3-F1: dedup — only emit error_message_changed when the message actually
  // changed. Decode-start failures (e.g. "required channels not enabled") can
  // fire this helper repeatedly per chunk/task; without dedup each identical
  // error spawned an event_bus_post + a full viewport_update() + a pxv_err
  // log write on the main thread (measured ~267/s during the decode-start
  // burst, 96 of 126 viewport_update calls came from that path).
  {
    std::lock_guard<std::mutex> lk(_state_mutex);
    if (_error_message == msg)
      return;
    _error_message = msg;
  }
  auto self = shared_from_this();
  _host->event_bus_post([self, msg]() {
    emit self->error_message_changed(msg);
  });
}

void DecoderStack::add_sub_decoder(std::unique_ptr<decode::Decoder> decoder) {
  if (!decoder) {
    pxv_warn("%s", "DecoderStack::add_sub_decoder: decoder is nullptr");
    return;
  }
  _stack.push_back(std::move(decoder));
  build_row();
  _options_changed = true;
  // Scheme A: rows may have changed; republish so the render path sees the
  // new row set even outside a decode run.
  publish_snapshot();
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
  // Scheme A: republish so the render path sees the reduced row set.
  publish_snapshot();
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

    // TDM Fast text-output state detection
    const bool is_tdm_fast =
        decc->id && std::strcmp(decc->id, "tdm_audio_fast") == 0;
    const char *tdm_output_mode = "waveform";
    if (is_tdm_fast) {
      const auto &opts = dec->options();
      auto out = opts.find("output");
      if (out != opts.end() && out->second &&
          g_variant_is_of_type(out->second, G_VARIANT_TYPE_STRING))
        tdm_output_mode = g_variant_get_string(out->second, nullptr);
    }
    const bool tdm_text_output =
        is_tdm_fast && tdm_output_mode &&
        std::strcmp(tdm_output_mode, "waveform") != 0;

    if (!decc->annotation_rows) {
      const Row row(decc);
      _rows[row] =
          std::make_unique<decode::RowData>(_annotation_heap);
      std::map<const decode::Row, bool>::const_iterator iter =
          _rows_gshow.find(row);
      if (iter == _rows_gshow.end()) {
        _rows_gshow[row] = is_tdm_fast ? tdm_text_output : true;
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

      _rows[row] =
          std::make_unique<decode::RowData>(_annotation_heap);
      std::map<const decode::Row, bool>::const_iterator iter =
          _rows_gshow.find(row);
      if (iter == _rows_gshow.end()) {
        _rows_gshow[row] = is_tdm_fast ? tdm_text_output : true;
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

    // TDM Fast: sync hidden output option with the effective state.
    if (is_tdm_fast) {
      bool any_row_enabled = false;
      for (const auto &entry : _rows_gshow) {
        if (entry.first.decoder() == decc && entry.second) {
          any_row_enabled = true;
          break;
        }
      }
      const bool emit_text = dec->shown() && any_row_enabled;
      dec->set_option("output",
                      g_variant_new_string(emit_text ? "both" : "waveform"));
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
  // NOTE: this API returns pointers into the row's deque, so it must read the
  // live RowData (whose lifetime is owned by _rows) rather than a published
  // snapshot, whose lifetime ends when this function returns. It is used only
  // on explicit user actions (waveform copy / protocol export), not on the
  // per-frame render path, so the shared lock here is acceptable.
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
  const auto snap = published_snapshot();
  if (snap) {
    auto it = snap->find(row);
    if (it != snap->end() && it->second.data)
      return it->second.data->get_annotation_index(start_sample);
    return 0;
  }
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  uint64_t index = 0;
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    index = (*iter).second->get_annotation_index(start_sample);

  return index;
}

std::pair<size_t, size_t> DecoderStack::get_visible_range(
    const Row &row, uint64_t start_sample, uint64_t end_sample) {
  const auto snap = published_snapshot();
  if (snap) {
    auto it = snap->find(row);
    if (it != snap->end() && it->second.data)
      return it->second.data->get_visible_range(start_sample, end_sample);
    return {0, 0};
  }
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  std::pair<size_t, size_t> range{0, 0};
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    range = (*iter).second->get_visible_range(start_sample, end_sample);

  return range;
}

uint64_t DecoderStack::get_max_annotation(const Row &row) {
  const auto snap = published_snapshot();
  if (snap) {
    auto it = snap->find(row);
    if (it != snap->end() && it->second.data)
      return it->second.data->get_max_annotation();
    return 0;
  }
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    return (*iter).second->get_max_annotation();

  return 0;
}

uint64_t DecoderStack::get_min_annotation(const Row &row) {
  const auto snap = published_snapshot();
  if (snap) {
    auto it = snap->find(row);
    if (it != snap->end() && it->second.data)
      return it->second.data->get_min_annotation();
    return 0;
  }
  std::shared_lock<std::shared_mutex> lock(_rows_mutex);
  auto iter = _rows.find(row);
  if (iter != _rows.end())
    return (*iter).second->get_min_annotation();

  return 0;
}

std::map<const decode::Row, bool> DecoderStack::get_rows_gshow() {
  const auto snap = published_snapshot();
  if (snap) {
    std::map<const decode::Row, bool> rows_gshow;
    for (const auto &kv : *snap)
      rows_gshow[kv.first] = kv.second.gshow;
    return rows_gshow;
  }
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
  const auto snap = published_snapshot();
  if (snap) {
    std::map<const decode::Row, bool> rows_lshow;
    for (const auto &kv : *snap)
      rows_lshow[kv.first] = kv.second.lshow;
    return rows_lshow;
  }
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
  {
    std::unique_lock<std::shared_mutex> lock(_rows_mutex);
    std::map<const decode::Row, bool>::const_iterator iter =
        _rows_gshow.find(row);
    if (iter != _rows_gshow.end()) {
      _rows_gshow[row] = show;
    }
  }
  // Scheme A: reflect the gshow toggle in the published snapshot so the
  // render path sees it even after decoding has stopped (no more publishes
  // from the decode thread). publish_snapshot takes _rows_mutex itself, so
  // release our lock first to avoid self-deadlock.
  publish_snapshot();
}

void DecoderStack::set_rows_lshow(const decode::Row row, bool show) {
  {
    std::unique_lock<std::shared_mutex> lock(_rows_mutex);
    std::map<const decode::Row, bool>::const_iterator iter =
        _rows_lshow.find(row);
    if (iter != _rows_lshow.end()) {
      _rows_lshow[row] = show;
    }
  }
  // Scheme A: lshow is captured into the published snapshot and used by
  // main-thread readers (list_annotation / list_row_title / ...), so any
  // change must be republished.
  publish_snapshot();
}

bool DecoderStack::has_annotations(const Row &row) {
  const auto snap = published_snapshot();
  if (snap) {
    auto it = snap->find(row);
    if (it != snap->end() && it->second.data)
      return !it->second.data->empty();
    return false;
  }
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

void DecoderStack::publish_snapshot() {
  // Build a new immutable snapshot from the current _rows/_rows_gshow.
  // Called by the decode thread at throttled notification points. Taking
  // the exclusive _rows_mutex here briefly blocks the decode thread's
  // emplace_annotation (which already holds it per-annotation), but it does
  // NOT block the GUI render path, which reads _published lock-free.
  std::unique_lock<std::shared_mutex> lock(_rows_mutex);

  auto snap = std::make_shared<SnapshotRows>();
  for (auto &kv : _rows) {
    const decode::Row &row = kv.first;
    auto gshow_it = _rows_gshow.find(row);
    auto lshow_it = _rows_lshow.find(row);
    SnapshotRow sr;
    sr.gshow = (gshow_it != _rows_gshow.end()) ? gshow_it->second : true;
    sr.lshow = (lshow_it != _rows_lshow.end()) ? lshow_it->second : true;
    // Incremental publish: only the annotations appended since the last
    // publish are copied into a new frozen segment; earlier segments are
    // reused via shared_ptr inside RowData::frozen_snapshot().
    sr.data = kv.second->frozen_snapshot();
    (*snap)[row] = std::move(sr);
  }

  // Atomic publish: the reader grabs this shared_ptr with a plain load.
  // We use release semantics so the snapshot contents are visible to the
  // GUI thread after it observes the pointer.
  _published.store(std::move(snap), std::memory_order_release);
  _snapshot_generation.fetch_add(1, std::memory_order_relaxed);
#ifdef PXVIEW_DECODE_PERF
  pv::base::perf::record_publish();
#endif
}

std::shared_ptr<const DecoderStack::SnapshotRows>
DecoderStack::published_snapshot() const {
  // Lock-free: atomic load of the immutable snapshot pointer.
  return _published.load(std::memory_order_acquire);
}

uint64_t DecoderStack::list_annotation_size() {
  const auto snap = published_snapshot();
  if (snap) {
    uint64_t max_annotation_size = 0;
    for (const auto &kv : *snap) {
      if (kv.second.lshow && kv.second.data)
        max_annotation_size =
            max(max_annotation_size, kv.second.data->get_annotation_size());
    }
    return max_annotation_size;
  }
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
  const auto snap = published_snapshot();
  if (snap) {
    for (const auto &kv : *snap) {
      if (kv.second.lshow && kv.second.data)
        if (row_index-- == 0)
          return kv.second.data->get_annotation_size();
    }
    return 0;
  }
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
  const auto snap = published_snapshot();
  if (snap) {
    for (const auto &kv : *snap) {
      if (kv.second.lshow && kv.second.data) {
        if (row_index-- == 0)
          return kv.second.data->get_annotation(ann, col_index);
      }
    }
    return false;
  }
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
  const auto snap = published_snapshot();
  if (snap) {
    for (const auto &kv : *snap) {
      if (kv.second.lshow) {
        if (row-- == 0) {
          title = kv.first.title();
          return 1;
        }
      }
    }
    return 0;
  }
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
  const auto snap = published_snapshot();
  if (snap) {
    for (const auto &kv : *snap) {
      if (kv.second.lshow) {
        if (row-- == 0) {
          desc = kv.first.description();
          return 1;
        }
      }
    }
    return 0;
  }
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
  if (_stack.empty() || !_host)
    return QString();
  auto *dec = _stack.front().get();
  if (!dec || !dec->have_probes())
    return QString();
  int probe_idx = dec->first_probe_index();
  if (probe_idx < 0)
    return QString();
  std::shared_lock<std::shared_mutex> lk(_host->signal_models_mutex());
  const auto &models = _host->get_signal_models();
  for (auto &m : models) {
    if (m && m->index() == probe_idx)
      return QString::fromStdString(m->name());
  }
  return QString();
}

void DecoderStack::clear() { init(); }

void DecoderStack::init() {
  clear_analog_data();
  _sample_count.store(0);
  {
    std::lock_guard<std::mutex> lk(_state_mutex);
    _samples_decoded = 0;
  }
set_error_message(QString());
_no_memory = false;
  _snapshot.reset();
  _result_count.store(0);
  _ann_dropped_stop.store(0);
  _ann_dropped_mem.store(0);
  _ann_dropped_row.store(0);

  {
    std::shared_lock<std::shared_mutex> lk(_rows_mutex);
    for (auto i = _rows.begin(); i != _rows.end(); i++) {
      (*i).second->clear();
    }
  }

  // Scheme A: publish an empty snapshot so the render path reads a
  // consistent (cleared) state after stop/restart of decoding.
  publish_snapshot();

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
set_error_message("");
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
set_error_message(QString::fromStdString(s_kRequiredChannelsMissing));
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
    std::shared_lock<std::shared_mutex> lk(_host->signal_models_mutex());
    models_snapshot = _host->get_signal_models();
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
set_error_message(QString::fromStdString(s_kRequiredChannelsMissing));
pxv_err("ERROR:%s", error_message().toStdString().c_str());
    pxv_err("ERROR:Failed to find matching LogicSnapshot for any decoder probe");
    return;
  }

  if (_host->is_realtime_refresh() == false && _snapshot->empty()) {
    pxv_err("ERROR:Decode data is empty.");
    return;
  }

  _samplerate.store(_snapshot->samplerate(), std::memory_order_release);
  if (_samplerate.load(std::memory_order_acquire) == 0.0) {
    pxv_err("ERROR:Decode data got an invalid sample rate.");
    return;
  }

  execute_decode_stack();
}

uint64_t DecoderStack::get_max_sample_count() {
  const auto snap = published_snapshot();
  if (snap) {
    uint64_t max_sample_count = 0;
    for (const auto &kv : *snap) {
      if (kv.second.data)
        max_sample_count =
            max(max_sample_count, kv.second.data->get_max_sample());
    }
    return max_sample_count;
  }
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
  uint64_t notify_cnt = 1;
  srd_decoder_inst *logic_di = nullptr;

  for (const GSList *d = srd_session_inst_list_get(session); d; d = d->next) {
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

  // Adaptive decode chunk for batch-oriented analog decoders.
  const bool adaptive_tdm_fast = logic_di->decoder && logic_di->decoder->id &&
      std::strstr(logic_di->decoder->id, "tdm_audio_fast") != nullptr;
  const bool adaptive_pwm_fast = logic_di->decoder && logic_di->decoder->id &&
      std::strstr(logic_di->decoder->id, "pwm_waveform_c") != nullptr;

  uint64_t i = decode_start;
  bool bError = false;
  bool bEndTime = false;

  if (i >= decode_end) {
    pxv_info("decode data index have been to end");
  }

  std::vector<const uint8_t *> chunk;
  std::vector<uint8_t> chunk_const;

  // P1-B: Iterator protocol support — one iterator per channel, reused
  // across chunks. This avoids re-computing root/lbp/byte indices on
  // every get_samples() call; instead, continue_sample_iteration()
  // advances incrementally.
  std::vector<std::unique_ptr<LogicSnapshot::SegmentDataIterator>> iterators;
  std::vector<bool> iter_valid;

  bool bCheckEnd = false;
  uint64_t end_index = decode_end;

  _progress.store(0);
  uint64_t sended_len = 0;
  _is_decoding.store(true);

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

    // P1-B: Initialize iterators on first iteration (or after wait).
    if (iterators.empty()) {
      for (int j = 0; j < logic_di->dec_num_channels; j++) {
        int sig_index = logic_di->dec_channelmap[j];
        if (sig_index != -1 && _snapshot->has_data(sig_index)) {
          iterators.push_back(_snapshot->begin_sample_iteration(i, sig_index));
          iter_valid.push_back(true);
        } else {
          iterators.push_back(nullptr);
          iter_valid.push_back(false);
        }
      }
    }

    for (int j = 0; j < logic_di->dec_num_channels; j++) {
      int sig_index = logic_di->dec_channelmap[j];

      if (sig_index == -1) {
        chunk.push_back(nullptr);
        chunk_const.push_back(0);
      } else {
        if (_snapshot->has_data(sig_index)) {
          // P1-B: Use iterator protocol instead of get_samples().
          // The iterator holds a raw pointer into the leaf block and
          // advances incrementally, avoiding repeated index computation.
          // NOTE: block memory safety is handled by the iterator refcount
          // (begin_sample_iteration/end_sample_iteration → _iterator_count);
          // free_head_blocks/calc_mipmap defer while iterators are active, so
          // no per-chunk free_decode_lpb bookkeeping is needed here (the old
          // lbp_array[35] mechanism passed a data pointer as a block pointer
          // and was a no-op with a fixed-array overflow hazard).
          auto *it = iterators[j].get();
          const uint8_t *data_ptr = nullptr;
          if (it && !it->exhausted)
            data_ptr = LogicSnapshot::get_iterator_value(it);
          chunk.push_back(data_ptr);
          chunk_const.push_back(_snapshot->get_sample(i, sig_index));
} else {
set_error_message(QString::fromStdString(s_kChannelsNotEnabled));
// P1-B: Clean up iterators before early return.
for (auto &it : iterators) {
  if (it) _snapshot->end_sample_iteration(std::move(it));
}
return;
        }
      }
    }

    if (chunk_end > end_index)
      chunk_end = end_index;
    uint64_t chunk_limit = MaxChunkSize;
    if ((adaptive_tdm_fast || adaptive_pwm_fast) && _samplerate.load(std::memory_order_acquire) > 0.0) {
      const uint64_t available_samples = _snapshot->get_sample_count();
      const uint64_t capture_seconds = static_cast<uint64_t>(
          static_cast<double>(available_samples) / _samplerate.load(std::memory_order_acquire));
      if (available_samples >= 10000000ULL || capture_seconds >= 10ULL)
        chunk_limit = 64 * 1024;
      else if (available_samples >= 1000000ULL || capture_seconds >= 1ULL)
        chunk_limit = 32 * 1024;
    }
    if (chunk_end - i > chunk_limit)
      chunk_end = i + chunk_limit;

    bEndTime = (chunk_end == end_index);

    if (srd_session_send(session, i, chunk_end, chunk.data(),
                         chunk_const.data(), chunk_end - i, nullptr) != SRD_OK) {

      const char *err_msg = srd_get_last_error();
      if (err_msg && *err_msg) {
        set_error_message(QString::fromLocal8Bit(err_msg));
        pxv_err("Failed to call srd_session_send:%s", err_msg);
      }
      srd_clear_last_error();

      bError = true;
      break;
    }

    sended_len += chunk_end - i;
    _progress.store((int)(sended_len * 100 / end_index));

    // P1-B: Advance iterators to the new position.
    // continue_sample_iteration() moves the iterator forward incrementally,
    // reloading the chunk_data pointer only when crossing a leaf-block
    // boundary. This is much cheaper than calling get_samples() which
    // re-computes all indices from scratch.
    {
      uint64_t advance = chunk_end - i;
      for (int j = 0; j < (int)iterators.size(); j++) {
        if (iterators[j] && advance > 0) {
          _snapshot->continue_sample_iteration(iterators[j].get(), advance);
        }
      }
    }

    i = chunk_end;

    {
      std::lock_guard<std::mutex> lock(_state_mutex);
      _samples_decoded = i - decode_start + 1;
    }

    if ((i - last_cnt) > notify_cnt) {
      last_cnt = i;
      // Scheme A: atomically publish the current frozen snapshot before
      // notifying the GUI, so the render path sees the latest decoded data
      // with zero lock contention. Publish + notify are throttled together
      // to at most 5 FPS (200ms): the GUI repaint rate for the decode track
      // is capped by the user requirement, and this also bounds the decode
      // thread's publish cost and the main-thread event queue volume.
      //
      // Plan C (experiment): 200ms -> 50ms was measured and REVERTED. MAX_
      // PUBLISH_DELTA stayed ~5.9K regardless (decoders emit annotation BURSTS
      // faster than any gate interval, so the delta is burst-size not gate-
      // time), and EVENT_LAG_MAX only improved inconsistently (384ms vs 1026ms
      // in different windows) while publish rate doubled (33->59/s). Keep 200ms.
      const auto now = std::chrono::steady_clock::now();
      if (now - _last_publish_time >=
          std::chrono::milliseconds(200)) {
        publish_snapshot();
        _last_publish_time = now;
        // P1: do NOT post a per-stack new_decode_data event here. The 24
        // stacks all align on the same 200ms gate, so posting individually
        // bursts 24 events onto the main thread at once, then dies to a
        // trickle — "忽快忽慢" growth. Instead, hand the stack over to the
        // host, which coalesces all requests into a steady ~100ms main-thread
        // batch. weak_ptr is dropped safely if the stack is destroyed before
        // the batch drains.
        _host->request_decode_notify(weak_from_this());
      }
    }
  }

  // P1-B: Clean up iterators after the decode loop.
  for (auto &it : iterators) {
    if (it)
      _snapshot->end_sample_iteration(std::move(it));
  }
  iterators.clear();

  _progress.store(100);
  _is_decoding.store(false);

  auto self = shared_from_this();

  if (!bError && bEndTime) {
    if (srd_session_end(session, nullptr) != SRD_OK) {
      const char *err_msg = srd_get_last_error();
      if (err_msg && *err_msg) {
        set_error_message(QString::fromLocal8Bit(err_msg));
        pxv_err("Failed to call srd_session_end:%s", err_msg);
      }
      srd_clear_last_error();
    }
  }

  // Publish final-data notification AFTER srd_session_end().
  // Scheme A: publish the final frozen snapshot so the last decoded data is
  // visible to the render path.
  publish_snapshot();
  if (adaptive_tdm_fast || !bError)
    _host->event_bus_post([self]() { self->new_decode_data(); });

  if (!_host->is_closed()) {
    _host->event_bus_post([self]() { self->decode_done(); });
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
set_error_message(QString::fromStdString(s_kCreateDecoderInstanceFailed));
srd_session_destroy(session);
      return;
    }

    if (prev_di)
      srd_inst_stack(session, prev_di, di);

    prev_di = di;
    decode_start = dec->decode_start();

    if (_host->is_realtime_refresh() == false) {
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
                           g_variant_new_uint64((uint64_t)_samplerate.load(std::memory_order_acquire)));

  // Let batch decoders choose an efficient path.
  uint64_t decode_sample_count = _sample_count.load(std::memory_order_acquire);
  if (decode_end != UINT64_MAX && decode_end >= decode_start)
    decode_sample_count = decode_end - decode_start + 1;
  srd_session_metadata_set(session, SRD_CONF_CAPTURE_SAMPLES,
                           g_variant_new_uint64(decode_sample_count));

  // P3-11 fix: obtain status shared_ptr under _status_mutex for the callback
  std::shared_ptr<decode_task_status> status_for_callback;
  {
    std::lock_guard<std::mutex> lk(_status_mutex);
    status_for_callback = _stask_stauts;
  }

  // 方案 E (Task 5): 改用批量回调。引擎按会话把 SRD_OUTPUT_ANN 注解收集进批缓冲
  // （满 1024 或实例 decode 结束时触发一次），批回调内完成拷贝/intern。
  // annotation_callback 保留作非批回退参考，不再注册。
  srd_pd_output_callback_add_batch(session, SRD_OUTPUT_ANN,
                                   DecoderStack::annotation_callback_batch,
                                   status_for_callback.get());

  srd_pd_output_callback_add(session, SRD_OUTPUT_ANALOG,
                             DecoderStack::analog_callback,
                             status_for_callback.get());

  int srd_ret = srd_session_start(session, nullptr);

  if (srd_ret == SRD_OK) {
    decode_data(decode_start, decode_end, session);
  } else {
    const char *err_msg = srd_get_last_error();
    if (err_msg && *err_msg) {
      set_error_message(QString::fromLocal8Bit(err_msg));
    }
    srd_clear_last_error();
  }

  srd_session_destroy(session);
}

uint64_t DecoderStack::sample_count() {
  if (_snapshot)
    return _snapshot->get_sample_count();
  else
    return 0;
}

uint64_t DecoderStack::sample_rate() { return _samplerate.load(std::memory_order_acquire); }

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

#ifdef PXVIEW_DECODE_PERF
  // P3-D8: sample, once per process, which heap a g_malloc'd block lands on
  // from a decode thread (reported as HEAP_TOPOLOGY decode_thread_glib). If it
  // equals the main-thread CRT heap, all decode threads + GUI share one lock.
  static bool s_heap_topology_recorded = false;
  if (!s_heap_topology_recorded) {
    s_heap_topology_recorded = true;
    pv::base::perf::record_decode_thread_heap();
  }
#endif

  const map<pair<const srd_decoder *, int>, Row>::const_iterator r =
      d->_class_rows.find(make_pair(decc, ann_format));
  if (r != d->_class_rows.end())
    row_iter = d->_rows.find((*r).second);
  else {
    row_iter = d->_rows.find(Row(decc));
  }

  if (row_iter == d->_rows.end()) {
    // P3-F1: throttle the per-annotation error log. A decoder emitting an
    // annotation class whose row is not registered (e.g. format=11 from a
    // decoder whose build_row() did not create that row) floods pxv_err with
    // one log-file write PER ANNOTATION on the decode thread — tens of
    // thousands of writes/second serialize on the global log lock and saturate
    // disk, freezing the whole app during decode ("开始解码会卡一段时间").
    // Only the first few are logged; the rest are counted silently via
    // _ann_dropped_row (already an atomic diagnostic counter).
    if (d->_ann_dropped_row.load(std::memory_order_relaxed) < 10)
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

// 方案 E (Task 5): 批量注解消费回调。引擎按会话把 SRD_OUTPUT_ANN 注解收集进
// 批缓冲（满 SRD_ANN_BATCH_MAX 或实例 decode 结束时触发一次），批内字符串由
// arena 拷贝、仅在回调期间有效，因此必须在回调内完成全部拷贝/intern
// （RowData 的 AnnotationResTable 已做文本 intern，会拷贝字符串）。
void DecoderStack::annotation_callback_batch(srd_ann_batch *batch, void *self) {
  if (!batch) {
    pxv_warn("%s", "DecoderStack::annotation_callback_batch: batch is nullptr");
    return;
  }
  if (!self) {
    pxv_warn("%s", "DecoderStack::annotation_callback_batch: self is nullptr");
    return;
  }
  assert(batch);
  assert(self);

  struct decode_task_status *st = (decode_task_status *)self;

  // P0-2 fix: _decoder is now a shared_ptr, keeping the DecoderStack alive.
  auto d = st->_decoder;
  if (!d) {
    pxv_warn("%s", "DecoderStack::annotation_callback_batch: d is nullptr");
    return;
  }
  assert(d);

  // 批级检查一次即可（逐注解路径每注解检查一次）。
  if (st->_bStop) {
    d->_ann_dropped_stop += batch->n;
    return;
  }
  if (!d->_decoder_status) {
    pxv_err("decode task was deleted.");
    return;
  }
  if (d->_no_memory) {
    d->_ann_dropped_mem += batch->n;
    return;
  }
  if (batch->n == 0 || !batch->items)
    return;

  d->_result_count += batch->n;

#ifdef PXVIEW_DECODE_PERF
  // P3-E: batch-annotation pipeline stats (see perflog.h BATCH_STATS).
  pv::base::perf::record_batch_stats(batch->n);
  // P3-D8: sample, once per process, which heap a g_malloc'd block lands on
  // from a decode thread (reported as HEAP_TOPOLOGY decode_thread_glib). If it
  // equals the main-thread CRT heap, all decode threads + GUI share one lock.
  static bool s_heap_topology_recorded = false;
  if (!s_heap_topology_recorded) {
    s_heap_topology_recorded = true;
    pv::base::perf::record_decode_thread_heap();
  }
#endif

  // 只获取一次 _rows_mutex，把整批按行分组（组键用 row_iter 指针 = RowData*，
  // 避免复制 Row）。行查找按批内缓存：每个 ann_format 只解析一次 decc 与行
  // （"批内缓存行查找"），1024 条注解只需数次 map 查找而非逐条两次查找。
  std::unique_lock<std::shared_mutex> lk(d->_rows_mutex);

  // ann_format -> RowData*（nullptr 表示该 format 没有已注册的行）。
  std::map<int, decode::RowData *> row_cache;
  // RowData* -> 该行的注解指针列表（指针只在回调期间有效，用完即弃）。
  std::map<decode::RowData *, std::vector<const srd_ann_item *>> groups;
  bool dropped_logged = false;

  for (size_t i = 0; i < batch->n; i++) {
    const srd_ann_item *it = &batch->items[i];
    const int ann_format = it->ann_class;

    decode::RowData *rd = nullptr;
    const auto cit = row_cache.find(ann_format);
    if (cit != row_cache.end()) {
      rd = cit->second;
    } else {
      // decc 整批相同可用缓存；按 it->decoder 精确解析行（与逐注解路径
      // pdata->pdo->di->decoder 语义一致），stacked 解码器因此能命中各自的行。
      const srd_decoder *const decc = it->decoder;
      if (decc) {
        const map<pair<const srd_decoder *, int>, Row>::const_iterator r =
            d->_class_rows.find(make_pair(decc, ann_format));
        if (r != d->_class_rows.end()) {
          const auto row_iter = d->_rows.find((*r).second);
          if (row_iter != d->_rows.end())
            rd = row_iter->second.get();
        } else {
          const auto row_iter = d->_rows.find(Row(decc));
          if (row_iter != d->_rows.end())
            rd = row_iter->second.get();
        }
      }
      row_cache[ann_format] = rd;
    }

    if (!rd) {
      // P3-F1: 频控日志，整批最多打一次；其余静默计数。
      if (!dropped_logged &&
          d->_ann_dropped_row.load(std::memory_order_relaxed) < 10) {
        pxv_err("Unexpected annotation (batch): format = %d", ann_format);
        dropped_logged = true;
      }
      d->_ann_dropped_row++;
      continue;
    }
    groups[rd].push_back(it);
  }

  lk.unlock();

  // 释放 _rows_mutex 后再落库（RowData 内部有自己的 _visitor_mutex），
  // 与逐注解路径的锁序（先 _rows_mutex 后 _visitor_mutex）一致。
  for (auto &g : groups) {
    if (!g.first->emplace_annotations(g.second, d->_decoder_status.get())) {
      d->_no_memory = true;
      break;  // 停止剩余行的落库
    }
  }
}

void DecoderStack::analog_callback(srd_proto_data *pdata, void *self) {
  if (!pdata || !self)
    return;
  auto *st = static_cast<decode_task_status *>(self);
  auto d = st->_decoder;
  if (!d || st->_bStop.load(std::memory_order_relaxed))
    return;
  if (!pdata->pdo || pdata->pdo->output_type != SRD_OUTPUT_ANALOG)
    return;
  const auto *pda = static_cast<const srd_proto_data_analog *>(pdata->data);
  if (!pda || !pda->data || pda->num_samples == 0)
    return;
  std::lock_guard<std::mutex> lock(d->_analog_mutex);
  std::shared_ptr<DecoderAnalogData> ch_data;
  for (const auto &ad : d->_analog_data) {
    if (ad && ad->channel() == pda->channel) { ch_data = ad; break; }
  }
  if (!ch_data) {
    char label[32];
    std::snprintf(label, sizeof(label), "CH%d", pda->channel);
    ch_data = std::make_shared<DecoderAnalogData>(pda->channel, pda->num_channels, label);
    d->_analog_data.push_back(ch_data);
    srd_decoder_inst *di = pdata->pdo->di;
    if (di && di->c_options) {
      char key[40]; GVariant *v = nullptr;
      std::snprintf(key, sizeof(key), "ch%d_enable", pda->channel);
      v = static_cast<GVariant *>(g_hash_table_lookup(di->c_options, key));
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
        ch_data->set_visible(g_variant_get_int64(v) != 0);
      std::snprintf(key, sizeof(key), "ch%d_vpos", pda->channel);
      v = static_cast<GVariant *>(g_hash_table_lookup(di->c_options, key));
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
        ch_data->set_v_offset(static_cast<float>(g_variant_get_double(v)));
      std::snprintf(key, sizeof(key), "ch%d_vzoom", pda->channel);
      v = static_cast<GVariant *>(g_hash_table_lookup(di->c_options, key));
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
        ch_data->set_v_scale(static_cast<float>(g_variant_get_double(v)));
      DecoderAnalogRangeMode range_mode = DecoderAnalogRangeMode::Bipolar;
      double eng_min = -1.0, eng_max = 1.0;
      std::string unit = "V";
      std::snprintf(key, sizeof(key), "ch%d_range_mode", pda->channel);
      v = static_cast<GVariant *>(g_hash_table_lookup(di->c_options, key));
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
        const char *mode = g_variant_get_string(v, nullptr);
        if (mode && std::strcmp(mode, "unipolar") == 0) range_mode = DecoderAnalogRangeMode::Unipolar;
        else if (mode && std::strcmp(mode, "custom") == 0) range_mode = DecoderAnalogRangeMode::Custom;
      }
      std::snprintf(key, sizeof(key), "ch%d_eng_min", pda->channel);
      v = static_cast<GVariant *>(g_hash_table_lookup(di->c_options, key));
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) eng_min = g_variant_get_double(v);
      std::snprintf(key, sizeof(key), "ch%d_eng_max", pda->channel);
      v = static_cast<GVariant *>(g_hash_table_lookup(di->c_options, key));
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) eng_max = g_variant_get_double(v);
      std::snprintf(key, sizeof(key), "ch%d_unit", pda->channel);
      v = static_cast<GVariant *>(g_hash_table_lookup(di->c_options, key));
      if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) unit = g_variant_get_string(v, nullptr);
      ch_data->set_engineering_config(range_mode, eng_min, eng_max, unit);
    }
  }
  if (pda->start_samples && pda->end_samples)
    ch_data->append_samples_timed(pda->start_samples, pda->end_samples, pda->data, static_cast<size_t>(pda->num_samples));
  else
    ch_data->append_samples(pdata->start_sample, pdata->end_sample, pda->data, static_cast<size_t>(pda->num_samples));
}

void DecoderStack::clear_analog_data() {
  std::lock_guard<std::mutex> lock(_analog_mutex);
  _analog_data.clear();
}

std::vector<std::shared_ptr<DecoderAnalogData>> DecoderStack::analog_data_copy() const {
  std::lock_guard<std::mutex> lock(_analog_mutex);
  return _analog_data;
}

size_t DecoderStack::analog_data_size() const {
  std::lock_guard<std::mutex> lock(_analog_mutex);
  return _analog_data.size();
}

namespace {
GVariant *zb_decoder_option_value(pv::data::decode::Decoder *decoder, const char *id) {
  if (!decoder || !id) return nullptr;
  const auto &options = decoder->options();
  const auto it = options.find(id);
  if (it != options.end()) return it->second;
  const srd_decoder *definition = decoder->decoder();
  if (!definition) return nullptr;
  for (GSList *item = definition->options; item; item = item->next) {
    const auto *option = static_cast<const srd_decoder_option *>(item->data);
    if (option && option->id && !strcmp(option->id, id)) return option->def;
  }
  return nullptr;
}
}

bool DecoderStack::get_analog_display_trigger_config(DecoderAnalogTriggerConfig &config) const {
  config = DecoderAnalogTriggerConfig{};
  if (_stack.empty()) return false;
  decode::Decoder *decoder = _stack.front().get();
  const srd_decoder *definition = decoder ? decoder->decoder() : nullptr;
  if (!definition || !definition->id) return false;
  const bool is_tdm = strncmp(definition->id, "tdm_audio", strlen("tdm_audio")) == 0;
  const bool is_pwm = strncmp(definition->id, "pwm_waveform", strlen("pwm_waveform")) == 0;
  if (!is_tdm && !is_pwm) return false;
  const AppOptions &app_options = AppConfig::Instance().appOptions;
  const bool remembered_valid = is_tdm ? app_options.analogDisplayTriggerTdmValid
                                       : app_options.analogDisplayTriggerPwmValid;
  if (remembered_valid) {
    config.enabled = is_tdm ? app_options.analogDisplayTriggerTdmEnable
                            : app_options.analogDisplayTriggerPwmEnable;
    if (!config.enabled) return false;
    const QString mode = is_tdm ? app_options.analogDisplayTriggerTdmMode
                                : app_options.analogDisplayTriggerPwmMode;
    if (mode == "normal") config.mode = DecoderAnalogTriggerMode::Normal;
    config.channel = is_tdm ? app_options.analogDisplayTriggerTdmChannel
                            : app_options.analogDisplayTriggerPwmChannel;
    config.channel = std::clamp(config.channel, 0, is_pwm ? 3 : 7);
    const QString edge = is_tdm ? app_options.analogDisplayTriggerTdmEdge
                                : app_options.analogDisplayTriggerPwmEdge;
    if (edge == "falling") config.edge = DecoderAnalogTriggerEdge::Falling;
    else if (edge == "either") config.edge = DecoderAnalogTriggerEdge::Either;
    config.level = is_tdm ? app_options.analogDisplayTriggerTdmLevel
                          : app_options.analogDisplayTriggerPwmLevel;
    config.display_position_percent = is_tdm ? app_options.analogDisplayTriggerTdmPosition
                                             : app_options.analogDisplayTriggerPwmPosition;
    config.display_position_percent = std::clamp(config.display_position_percent, 0, 100);
    return true;
  }
  GVariant *value = zb_decoder_option_value(decoder, "display_trigger_enable");
  if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) || !g_variant_get_boolean(value)) return false;
  config.enabled = true;
  value = zb_decoder_option_value(decoder, "display_trigger_mode");
  if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
    const char *mode = g_variant_get_string(value, nullptr);
    if (mode && !strcmp(mode, "normal")) config.mode = DecoderAnalogTriggerMode::Normal;
  }
  value = zb_decoder_option_value(decoder, "display_trigger_channel");
  if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) config.channel = (int)g_variant_get_int64(value);
  config.channel = std::clamp(config.channel, 0, is_pwm ? 3 : 7);
  value = zb_decoder_option_value(decoder, "display_trigger_edge");
  if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
    const char *edge = g_variant_get_string(value, nullptr);
    if (edge && !strcmp(edge, "falling")) config.edge = DecoderAnalogTriggerEdge::Falling;
    else if (edge && !strcmp(edge, "either")) config.edge = DecoderAnalogTriggerEdge::Either;
  }
  value = zb_decoder_option_value(decoder, "display_trigger_level");
  if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE)) config.level = g_variant_get_double(value);
  value = zb_decoder_option_value(decoder, "display_trigger_position");
  if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_INT64)) config.display_position_percent = (int)g_variant_get_int64(value);
  config.display_position_percent = std::clamp(config.display_position_percent, 0, 100);
  return true;
}

bool DecoderStack::find_analog_display_trigger(uint64_t &sample_position, DecoderAnalogTriggerConfig *config_out) const {
  DecoderAnalogTriggerConfig config;
  if (!get_analog_display_trigger_config(config)) return false;
  std::shared_ptr<DecoderAnalogData> channel_data;
  for (const auto &data : analog_data_copy()) {
    if (data && data->channel() == config.channel) { channel_data = data; break; }
  }
  if (!channel_data) return false;
  auto read_view = channel_data->read_samples();
  const auto &samples = read_view.samples();
  if (samples.size() < 2) return false;
  const uint64_t range_start = samples.front().start_sample;
  const uint64_t range_end = samples.back().start_sample;
  const uint64_t target = range_start + (range_end - range_start) * (uint64_t)config.display_position_percent / 100U;
  bool found = false;
  uint64_t best_sample = 0;
  uint64_t best_distance = std::numeric_limits<uint64_t>::max();
  for (size_t i = 1; i < samples.size(); ++i) {
    const auto &previous = samples[i - 1];
    const auto &current = samples[i];
    if (current.start_sample <= previous.start_sample) continue;
    const double previous_value = channel_data->engineering_value(previous.value);
    const double current_value = channel_data->engineering_value(current.value);
    const bool rising = previous_value < config.level && current_value >= config.level;
    const bool falling = previous_value > config.level && current_value <= config.level;
    const bool matches = config.edge == DecoderAnalogTriggerEdge::Rising ? rising
                         : config.edge == DecoderAnalogTriggerEdge::Falling ? falling : (rising || falling);
    if (!matches) continue;
    double fraction = 0.0;
    const double delta = current_value - previous_value;
    if (std::isfinite(delta) && std::abs(delta) > 1e-20)
      fraction = std::clamp((config.level - previous_value) / delta, 0.0, 1.0);
    const double interpolated = (double)previous.start_sample + fraction * (double)(current.start_sample - previous.start_sample);
    const uint64_t crossing = interpolated <= 0.0 ? 0U : (uint64_t)std::llround(interpolated);
    const uint64_t distance = crossing > target ? crossing - target : target - crossing;
    if (!found || distance < best_distance) { found = true; best_sample = crossing; best_distance = distance; }
  }
  if (!found) return false;
  sample_position = best_sample;
  if (config_out) *config_out = config;
  return true;
}

void DecoderStack::frame_ended() {
  _options_changed = true;

  if (_host) {
    const uint64_t limit = _host->get_ring_sample_count();
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
