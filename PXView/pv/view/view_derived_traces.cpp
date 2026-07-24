/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <support@dreamsourcelab.com>
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

// Phase E (modernize-view-layer-v2): decoder / spectrum / math / lissajous
// derived-trace behaviour extracted from the View God-class.
// ViewDerivedTraces is declared a friend of View so it can touch the private
// derived-trace state (_own_decode_traces / _own_spectrum_traces /
// _own_math_trace / _own_lissajous_trace / _derived_traces_dirty / _session /
// _data_source / _own_signals) directly. Cross-method calls that remain on
// View (e.g. signals_changed, compute_signal_groups, document_snapshot_source)
// go through _view->… so the public View API is unchanged.

#include "view_derived_traces.h"

#include <algorithm>
#include <memory>

#include "view.h"

#include "../data/decode/decoder.h"
#include "../data/decode/decoderstatus.h"
#include "../data/decoderstack.h"
#include "../data/lissajousmodel.h"
#include "../data/spectrumstack.h"
#include "../dsvdef.h"
#include "../log.h"
#include "../sigsession.h"

#include "decodetrace.h"
#include "dsosignal.h"
#include "lissajoustrace.h"
#include "mathtrace.h"
#include "spectrumtrace.h"

using namespace std;

namespace pv {
namespace view {

void ViewDerivedTraces::mark_derived_traces_dirty() {
  _view->_derived_traces_dirty = true;
}

bool ViewDerivedTraces::add_decoder(
    srd_decoder *const dec, bool silent, DecoderStatus *dstatus,
    std::list<pv::data::decode::Decoder *> &sub_decoders,
    std::shared_ptr<pv::data::DecoderStack> &out_stack) {
  if (!_view->_session)
    return false;

  out_stack = nullptr;

  // 1. Core layer creates the DecoderStack and adds it to the active
  //    document's stack list. Core owns the DecoderStack.
  if (!_view->_data_source->add_decoder(dec, silent, dstatus, sub_decoders,
                                        out_stack))
    return false;

  if (!out_stack)
    return false;

  // 2. View directly creates its DecodeTrace wrapper for the new
  //    DecoderStack. We do NOT rely on the signals_changed event callback
  //    (which would only lazily sync via sync_derived_traces()) because the
  //    caller may need to interact with the DecodeTrace immediately after
  //    this method returns (e.g. to attach protocol layer items).
  //    The index used here is the position in the View's own list, matching
  //    the pattern in sync_derived_traces().
  int decode_index = (int)_view->_own_decode_traces.size();
  auto *trace = new DecodeTrace(_view->_session, out_stack, decode_index);
  // Set initial view_index: place after all signal tracks (same pattern
  // as sync_derived_traces). Without this, view_index defaults to -1 and
  // causes incorrect layout ordering in LOGIC mode.
  trace->set_view_index((int)_view->_own_signals.size() + decode_index);
  // CRITICAL: set_view(this) must be called BEFORE create_popup() because
  // the dialog accesses _trace->get_view() to get session and signal_models.
  trace->set_view(_view);

  // 3. If silent is false, show the decoder options dialog so the user can
  //    configure channel mappings, decode range, etc. This was previously
  //    done inside SigSession::add_decoder() via DecodeTrace::create_popup();
  //    it was moved here during de-view-ization because Core must not depend
  //    on Qt Widgets. If the user cancels (or fails to set required probes),
  //    roll back: delete the DecodeTrace and ask Core to delete the
  //    DecoderStack so we don't leave an unconfigured decoder around.
  pxv_info("View: before create_popup(true), silent=%d", silent);
  if (!silent) {
    bool settings_changed = trace->create_popup(true);
    pxv_info("View: create_popup returned %d", settings_changed);
    if (!settings_changed) {
      delete trace;
      void *key = out_stack->get_key_handel();
      _view->_data_source->remove_decoder_by_key_handel(key);
      out_stack = nullptr;
      pxv_info("View: rollback complete, returning false");
      return false;
    }
  }

  _view->_own_decode_traces.push_back(trace);

  // 4. Mark derived traces NOT dirty since we just synced the DecodeTrace
  //    list manually. This prevents sync_derived_traces() from recreating
  //    the DecodeTrace we just added.
  _view->_derived_traces_dirty = false;

  // 5. Broadcast first so that SigSession::on_event(DeviceOptionsUpdated)
  //    can run reload() before we start the decode task. The broadcast is
  //    synchronous (direct function call, not Qt queued signal), so reload()
  //    will clear SignalModel snapshots immediately. We then call
  //    add_decode_task() which internally calls attach_data_to_signal() to
  //    restore the snapshot pointers. If we called add_decode_task() before
  //    broadcast_async<>(), the snapshot would be set and then cleared by
  //    reload(), causing "没有设置需要解码哪些通道的数据".
  //    This broadcast also notifies MCP/WebSocket clients (SessionService maps
  //    DeviceOptionsUpdated to DeviceConfigChanged).
  // Task D6: kept — View as top-level container legitimately broadcasts via session facade.
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});

  // 6. Now start the decode task after reload() has completed. The public
  //    start_all_decode_tasks() funnel calls attach_data_to_signal() before
  //    starting tasks, restoring snapshot pointers. If there is no view data
  //    yet (decoder added before capture), the capture pipeline will start
  //    the decode for us via CopyToDocDone → frame_ended() +
  //    start_all_decode_tasks().
  if (!silent && _view->_data_source->have_view_data()) {
    _view->_data_source->start_all_decode_tasks();
  }

  // 7. Refresh layout. signals_changed(NULL) calls mark_derived_traces_dirty()
  //    at the top, but since the DecodeTrace list is already in sync, the
  //    subsequent sync_derived_traces() will be a no-op for decoders.
  _view->signals_changed(NULL);

  return true;
}

bool ViewDerivedTraces::rst_decoder_by_key_handel(void *handel, QPoint anchor) {
  if (!_view->_session || !handel)
    return false;

  // Find the View-owned DecodeTrace that wraps this DecoderStack.
  auto find_trace = [&]() -> DecodeTrace * {
    for (auto *trace : _view->_own_decode_traces) {
      if (trace && trace->decoder() &&
          trace->decoder()->get_key_handel() == handel)
        return trace;
    }
    return nullptr;
  };

  DecodeTrace *target = find_trace();

  // Fall back to lazy sync if not found (the list might be dirty).
  if (!target) {
    sync_derived_traces();
    target = find_trace();
  }

  if (!target)
    return false;

  // Re-open the options dialog. If the user cancels (no settings change),
  // do NOT reset the decoder — keep the existing configuration. This
  // restores the pre-de-view-ization behavior where SigSession bailed out
  // of rst_decoder() when create_popup() returned false.
  bool settings_changed = target->create_popup(false, anchor);
  if (!settings_changed)
    return false;

  // Forward to Core to clear the existing decode task and re-add it.
  _view->_data_source->rst_decoder_by_key_handel(handel);
  return true;
}

void ViewDerivedTraces::remove_decoder(DecodeTrace *trace) {
  if (!trace)
    return;

  auto it = std::find(_view->_own_decode_traces.begin(),
                      _view->_own_decode_traces.end(), trace);
  if (it == _view->_own_decode_traces.end())
    return;

  auto stack = trace->decoder();
  void *key_handel = stack ? stack->get_key_handel() : nullptr;

  // 1. View deletes its DecodeTrace (View-owned). The DecodeTrace
  //    destructor does NOT delete the DecoderStack (Core owns it).
  _view->_own_decode_traces.erase(it);
  delete trace;

  // 2. Notify Core layer to delete the corresponding DecoderStack.
  //    Core's remove_decoder_by_key_handel() will remove the stack from
  //    its list and delete it (immediately if no decode thread holds it,
  //    otherwise asynchronously via _delete_flag). Core fires
  //    signals_changed() callback which triggers View::signals_changed(NULL)
  //    — at that point sync_derived_traces() will find no DecodeTrace to
  //    remove (we already deleted it), so no double-free occurs.
  if (key_handel) {
    _view->_data_source->remove_decoder_by_key_handel(key_handel);
  }

  // 3. Broadcast so the API layer can push a ServiceEvent to remote clients.
  //    View cannot call SessionService::broadcast_event directly (View does
  //    not depend on the API layer), so we forward via
  //    SigSession::broadcast_async. DeviceOptionsUpdated is mapped by
  //    SessionService to DeviceConfigChanged, which triggers state
  //    synchronization. (The MCP remove_analyzer path already broadcasts
  //    DecoderRemoved directly; this covers the GUI-triggered path.)
  // Task D6: kept — View as top-level container legitimately broadcasts via session facade.
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});
}

void ViewDerivedTraces::remove_decoder(int index) {
  if (index < 0 || index >= (int)_view->_own_decode_traces.size())
    return;
  remove_decoder(_view->_own_decode_traces[index]);
}

void ViewDerivedTraces::remove_decoder_by_key_handel(void *key_handel) {
  if (!_view->_session || !key_handel)
    return;

  // Find the View-owned DecodeTrace that wraps the DecoderStack with this
  // key_handel. This mirrors the pattern in rst_decoder_by_key_handel().
  auto find_trace = [&]() -> DecodeTrace * {
    for (auto *trace : _view->_own_decode_traces) {
      if (trace && trace->decoder() &&
          trace->decoder()->get_key_handel() == key_handel)
        return trace;
    }
    return nullptr;
  };

  DecodeTrace *target = find_trace();

  // Fall back to lazy sync if not found (the list might be dirty).
  if (!target) {
    sync_derived_traces();
    target = find_trace();
  }

  if (!target)
    return;

  remove_decoder(target);
}

void ViewDerivedTraces::clear_all_decoders() {
  if (!_view->_session)
    return;

  // 1. Delete all View-owned DecodeTrace objects first.
  for (auto *trace : _view->_own_decode_traces) {
    delete trace;
  }
  _view->_own_decode_traces.clear();

  // 2. Notify Core to clear all DecoderStacks.
  //    Core's clear_all_decoder() will fire signals_changed() callback
  //    which triggers View::signals_changed(), but since we already
  //    deleted all DecodeTrace, the subsequent sync_derived_traces()
  //    will be a no-op.
  _view->_data_source->clear_all_decoder(true);

  // 3. Broadcast so the API layer can push a ServiceEvent to remote clients.
  // Task D6: kept — View as top-level container legitimately broadcasts via session facade.
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});
}

void ViewDerivedTraces::sync_derived_traces() {
  if (!_view->_derived_traces_dirty)
    return;

  _view->_derived_traces_dirty = false;

  auto *source = _view->document_snapshot_source();
  if (!source)
    return;

  // Track whether any trace was added or removed. If so, _signal_groups
  // (which caches raw Trace* pointers) must be rebuilt to avoid dangling
  // pointers. Header::paintEvent reads _signal_groups via get_signal_groups(),
  // and a stale pointer there causes SIGSEGV (UAF).
  bool changed = false;

  // ---- Sync DecodeTrace list from DecoderStack list ----
  auto &decoder_stacks = source->get_decoder_stacks();

  // Remove DecodeTrace whose DecoderStack no longer exists.
  for (auto it = _view->_own_decode_traces.begin();
       it != _view->_own_decode_traces.end();) {
    DecodeTrace *dt = *it;
    auto target = dt->decoder().get();
    auto it_stack = std::find_if(
        decoder_stacks.begin(), decoder_stacks.end(),
        [target](const std::shared_ptr<pv::data::DecoderStack> &s) {
          return s.get() == target;
        });
    if (it_stack == decoder_stacks.end()) {
      delete dt;
      it = _view->_own_decode_traces.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  // Add DecodeTrace for new DecoderStacks that have no wrapper yet.
  int decode_index = 0;
  for (auto stack : decoder_stacks) {
    bool exists = false;
    for (auto *dt : _view->_own_decode_traces) {
      if (dt->decoder().get() == stack.get()) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      auto *dt = new DecodeTrace(_view->_session, stack, decode_index);
      // Sync _visible from Decoder::_shown so that a decoder the user
      // previously hid stays hidden after tab switch (sync_derived_traces
      // is called lazily when the DecodeTrace list is accessed).
      if (!stack->stack().empty() && !stack->stack().front()->shown()) {
        dt->set_visible(false);
      }
      // Set initial view_index: place after all signal tracks.
      // Without this, view_index defaults to -1 (trace.h:81) and
      // causes incorrect layout ordering in LOGIC mode signals_changed.
      dt->set_view_index((int)_view->_own_signals.size() + decode_index);
      _view->_own_decode_traces.push_back(dt);
      changed = true;
    }
    decode_index++;
  }

  // ---- Sync SpectrumTrace list from SpectrumStack list ----
  auto &spectrum_stacks = source->get_spectrum_stacks();

  // Remove SpectrumTrace whose SpectrumStack no longer exists.
  for (auto it = _view->_own_spectrum_traces.begin();
       it != _view->_own_spectrum_traces.end();) {
    SpectrumTrace *st = *it;
    auto target = st->get_spectrum_stack().get();
    auto it_stack = std::find_if(
        spectrum_stacks.begin(), spectrum_stacks.end(),
        [target](const std::shared_ptr<pv::data::SpectrumStack> &s) {
          return s.get() == target;
        });
    if (it_stack == spectrum_stacks.end()) {
      delete st;
      it = _view->_own_spectrum_traces.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  // Add SpectrumTrace for new SpectrumStacks that have no wrapper yet.
  for (auto stack : spectrum_stacks) {
    bool exists = false;
    for (auto *st : _view->_own_spectrum_traces) {
      if (st->get_spectrum_stack().get() == stack.get()) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      auto *st = new SpectrumTrace(_view->_session, stack, stack->get_index());
      _view->_own_spectrum_traces.push_back(st);
      changed = true;
    }
  }

  // ---- Sync MathTrace from MathStack ----
  auto math_stack = source->get_math_stack();
  if (math_stack) {
    if (!_view->_own_math_trace ||
        _view->_own_math_trace->get_math_stack().get() != math_stack.get()) {
      // Tear down any stale MathTrace bound to a previous MathStack.
      if (_view->_own_math_trace) {
        delete _view->_own_math_trace;
        _view->_own_math_trace = nullptr;
        changed = true;
      }

      // MathStack now exposes the source channel indices. Look up the
      // matching DsoSignal instances in _own_signals so the MathTrace can
      // read DSO-specific UI state (ref_min/ref_max, view rect, etc.).
      DsoSignal *dso1 = nullptr;
      DsoSignal *dso2 = nullptr;
      const int idx1 = math_stack->ch1_index();
      const int idx2 = math_stack->ch2_index();
      for (auto *sig : _view->_own_signals) {
        if (!sig || sig->signal_type() != SR_CHANNEL_DSO)
          continue;
        if (sig->get_index() == idx1)
          dso1 = dynamic_cast<DsoSignal *>(sig);
        if (sig->get_index() == idx2)
          dso2 = dynamic_cast<DsoSignal *>(sig);
        if (dso1 && dso2)
          break;
      }

      if (dso1 && dso2) {
        _view->_own_math_trace = new MathTrace(true, math_stack, dso1, dso2);
        changed = true;
      } else {
        pxv_warn("View::sync_derived_traces: DsoSignal not found for "
                 "math src1=%d or src2=%d — MathTrace creation skipped.",
                 idx1, idx2);
      }
    }
  } else {
    if (_view->_own_math_trace) {
      delete _view->_own_math_trace;
      _view->_own_math_trace = nullptr;
      changed = true;
    }
  }

  // ---- Sync LissajousTrace from LissajousModel ----
  auto *lissajous_model = source->get_lissajous_model();
  if (lissajous_model && lissajous_model->enabled()) {
    if (!_view->_own_lissajous_trace) {
      auto *snapshot = source->get_dso_snapshot();
      _view->_own_lissajous_trace = new LissajousTrace(
          lissajous_model->enabled(), snapshot, lissajous_model->x_index(),
          lissajous_model->y_index(), lissajous_model->percent());
      changed = true;
    }
  } else {
    if (_view->_own_lissajous_trace) {
      delete _view->_own_lissajous_trace;
      _view->_own_lissajous_trace = nullptr;
      changed = true;
    }
  }

  // Rebuild _signal_groups if any trace was added or removed.
  // _signal_groups caches raw Trace* pointers; without this rebuild, a
  // deleted trace's pointer becomes dangling, causing SIGSEGV in
  // Header::paintEvent when it iterates group.traces.
  // Safe against recursion: _derived_traces_dirty is already false, so
  // compute_signal_groups() -> get_traces() -> get_own_decode_traces() ->
  // sync_derived_traces() returns immediately.
  if (changed) {
    _view->compute_signal_groups();
  }
}

} // namespace view
} // namespace pv
