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

#include "pv/view/view_derived_traces.h"

#include <algorithm>
#include <memory>

#include "pv/view/view.h"

#include "pv/data/decode/decoder.h"
#include "pv/data/decode/decoderstatus.h"
#include "pv/data/stack/decoderstack.h"
#include "pv/data/stack/lissajousmodel.h"
#include "pv/data/stack/spectrumstack.h"
#include "pv/base/pxvdef.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"

#include "pv/view/trace/decodetrace.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/trace/lissajoustrace.h"
#include "pv/view/trace/mathtrace.h"
#include "pv/view/trace/spectrumtrace.h"

using namespace std;

namespace pv {
namespace view {

ViewDerivedTraces::ViewDerivedTraces(View *view) : _view(view) {}

// Destructor defined in .cpp so unique_ptr<MathTrace/LissajousTrace>
// can see complete type definitions.
ViewDerivedTraces::~ViewDerivedTraces() {
  cleanup();
}

void ViewDerivedTraces::cleanup() {
  // unique_ptr containers auto-delete all elements.
  _own_decode_traces.clear();
  _own_spectrum_traces.clear();

  if (_own_math_trace) {
    _own_math_trace.reset();
  }

  if (_own_lissajous_trace) {
    _own_lissajous_trace.reset();
  }
}

void ViewDerivedTraces::mark_derived_traces_dirty() {
  _derived_traces_dirty = true;
}

bool ViewDerivedTraces::add_decoder(
    srd_decoder *const dec, bool silent, DecoderStatus *dstatus,
    std::list<pv::data::decode::Decoder *> &sub_decoders,
    std::shared_ptr<pv::data::DecoderStack> &out_stack) {
  if (!_view->session_ptr())
    return false;

  out_stack = nullptr;

  // 1. Core layer creates the DecoderStack and adds it to the active
  //    document's stack list. Core owns the DecoderStack.
  if (!_view->data_source()->add_decoder(dec, silent, dstatus, sub_decoders,
                                        out_stack))
    return false;

  if (!out_stack)
    return false;

  // 2. View directly creates its DecodeTrace wrapper for the new
  //    DecoderStack.
  int decode_index = (int)_own_decode_traces.size();
  auto trace = std::make_unique<DecodeTrace>(_view->session_ptr(), out_stack, decode_index);
  trace->set_view_index((int)_view->get_own_signals().size() + decode_index);
  trace->set_view(_view);

  // 3. If silent is false, show the decoder options dialog. If the user
  //    cancels, roll back: unique_ptr auto-deletes the DecodeTrace.
  pxv_info("View: before create_popup(true), silent=%d", silent);
  if (!silent) {
    bool settings_changed = trace->create_popup(true);
    pxv_info("View: create_popup returned %d", settings_changed);
    if (!settings_changed) {
      // trace auto-deleted when it goes out of scope (unique_ptr)
      void *key = out_stack->get_key_handel();
      _view->data_source()->remove_decoder_by_key_handel(key);
      out_stack = nullptr;
      pxv_info("View: rollback complete, returning false");
      return false;
    }
  }

  _own_decode_traces.push_back(std::move(trace));

  // 4. Mark derived traces NOT dirty since we just synced manually.
  _derived_traces_dirty = false;

  // 5. Broadcast DeviceOptionsUpdated so SigSession::on_event triggers reload()
  //    to sync channel state. Note: broadcast_async is ASYNC — reload() runs
  //    in the NEXT event loop iteration, AFTER start_all_decode_tasks() below.
  //    This creates a race where reload() recreates SignalModels with nullptr
  //    snapshots while decode threads are already running. The fix is in
  //    reload() itself: it now sets snapshot pointers on the new models
  //    immediately after creating them, so decode threads always find valid
  //    snapshots regardless of timing.
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});

  // 6. Start the decode task for all decoders (including the newly added one).
  if (!silent && _view->data_source()->have_view_data()) {
    _view->data_source()->start_all_decode_tasks();
  }

  // 7. Refresh layout.
  _view->signals_changed(nullptr);

  return true;
}

bool ViewDerivedTraces::rst_decoder_by_key_handel(void *handel, QPoint anchor) {
  if (!_view->session_ptr() || !handel)
    return false;

  // Find the View-owned DecodeTrace that wraps this DecoderStack.
  auto find_trace = [&]() -> DecodeTrace * {
    for (auto &trace : _own_decode_traces) {
      if (trace && trace->decoder() &&
          trace->decoder()->get_key_handel() == handel)
        return trace.get();
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

  // Re-open the options dialog.
  bool settings_changed = target->create_popup(false, anchor);
  if (!settings_changed)
    return false;

  // Forward to Core to clear the existing decode task and re-add it.
  _view->data_source()->rst_decoder_by_key_handel(handel);
  return true;
}

void ViewDerivedTraces::remove_decoder(DecodeTrace *trace) {
  if (!trace)
    return;

  auto it = std::find_if(_own_decode_traces.begin(),
                         _own_decode_traces.end(),
                         [trace](const std::unique_ptr<DecodeTrace> &p) {
                           return p.get() == trace;
                         });
  if (it == _own_decode_traces.end())
    return;

  auto stack = trace->decoder();
  void *key_handel = stack ? stack->get_key_handel() : nullptr;

  // 1. View erases its DecodeTrace (unique_ptr auto-deletes).
  _own_decode_traces.erase(it);

  // 2. Notify Core layer to delete the corresponding DecoderStack.
  if (key_handel) {
    _view->data_source()->remove_decoder_by_key_handel(key_handel);
  }

  // 3. Broadcast so the API layer can push a ServiceEvent.
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});
}

void ViewDerivedTraces::remove_decoder(int index) {
  if (index < 0 || index >= (int)_own_decode_traces.size())
    return;
  remove_decoder(_own_decode_traces[index].get());
}

void ViewDerivedTraces::remove_decoder_by_key_handel(void *key_handel) {
  if (!_view->session_ptr() || !key_handel)
    return;

  // Find the View-owned DecodeTrace that wraps the DecoderStack with this
  // key_handel.
  auto find_trace = [&]() -> DecodeTrace * {
    for (auto &trace : _own_decode_traces) {
      if (trace && trace->decoder() &&
          trace->decoder()->get_key_handel() == key_handel)
        return trace.get();
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
  if (!_view->session_ptr())
    return;

  // 1. Clear all View-owned DecodeTrace objects (unique_ptr auto-deletes).
  _own_decode_traces.clear();

  // 2. Notify Core to clear all DecoderStacks.
  _view->data_source()->clear_all_decoder(true);

  // 3. Broadcast so the API layer can push a ServiceEvent.
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});
}

void ViewDerivedTraces::sync_derived_traces() {
  if (!_derived_traces_dirty)
    return;

  _derived_traces_dirty = false;

  auto *source = _view->document_snapshot_source();
  if (!source)
    return;

  bool changed = false;

  // ---- Sync DecodeTrace list from DecoderStack list ----
  auto &decoder_stacks = source->get_decoder_stacks();

  // Remove DecodeTrace whose DecoderStack no longer exists.
  for (auto it = _own_decode_traces.begin();
       it != _own_decode_traces.end();) {
    DecodeTrace *dt = it->get();
    auto target = dt->decoder().get();
    auto it_stack = std::find_if(
        decoder_stacks.begin(), decoder_stacks.end(),
        [target](const std::shared_ptr<pv::data::DecoderStack> &s) {
          return s.get() == target;
        });
    if (it_stack == decoder_stacks.end()) {
      // unique_ptr auto-deletes when erased
      it = _own_decode_traces.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  // Add DecodeTrace for new DecoderStacks that have no wrapper yet.
  int decode_index = 0;
  for (auto stack : decoder_stacks) {
    bool exists = false;
    for (auto &dt : _own_decode_traces) {
      if (dt->decoder().get() == stack.get()) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      auto dt = std::make_unique<DecodeTrace>(_view->session_ptr(), stack, decode_index);
      if (!stack->stack().empty() && !stack->stack().front()->shown()) {
        dt->set_visible(false);
      }
      dt->set_view_index((int)_view->get_own_signals().size() + decode_index);
      // Initialize _view and _viewport so paint_back/paint_mid can safely
      // access _view->scale(), _view->offset(), etc. without SIGSEGV.
      // Without this, the Modified path in signals_modified_refresh()
      // creates DecodeTrace objects that are painted before
      // layout_time_signals() calls set_view().
      dt->set_view(_view);
      if (_view->get_time_view())
        dt->set_viewport(_view->get_time_view());
      _own_decode_traces.push_back(std::move(dt));
      changed = true;
    }
    decode_index++;
  }

  // ---- Sync SpectrumTrace list from SpectrumStack list ----
  auto &spectrum_stacks = source->get_spectrum_stacks();

  // Remove SpectrumTrace whose SpectrumStack no longer exists.
  for (auto it = _own_spectrum_traces.begin();
       it != _own_spectrum_traces.end();) {
    SpectrumTrace *st = it->get();
    auto target = st->get_spectrum_stack().get();
    auto it_stack = std::find_if(
        spectrum_stacks.begin(), spectrum_stacks.end(),
        [target](const std::shared_ptr<pv::data::SpectrumStack> &s) {
          return s.get() == target;
        });
    if (it_stack == spectrum_stacks.end()) {
      // unique_ptr auto-deletes when erased
      it = _own_spectrum_traces.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  // Add SpectrumTrace for new SpectrumStacks that have no wrapper yet.
  for (auto stack : spectrum_stacks) {
    bool exists = false;
    for (auto &st : _own_spectrum_traces) {
      if (st->get_spectrum_stack().get() == stack.get()) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      auto st = std::make_unique<SpectrumTrace>(_view->session_ptr(), stack, stack->get_index());
      st->set_view(_view);
      if (_view->fft_viewport())
        st->set_viewport(_view->fft_viewport());
      _own_spectrum_traces.push_back(std::move(st));
      changed = true;
    }
  }

  // ---- Sync MathTrace from MathStack ----
  auto math_stack = source->get_math_stack();
  if (math_stack) {
    if (!_own_math_trace ||
        _own_math_trace->get_math_stack().get() != math_stack.get()) {
      if (_own_math_trace) {
        _own_math_trace.reset();
        changed = true;
      }

      DsoSignal *dso1 = nullptr;
      DsoSignal *dso2 = nullptr;
      const int idx1 = math_stack->ch1_index();
      const int idx2 = math_stack->ch2_index();
      for (auto &sig : _view->get_own_signals()) {
        if (!sig)
          continue;
        if (sig->get_index() == idx1)
          dso1 = sig->as_dso();
        if (sig->get_index() == idx2)
          dso2 = sig->as_dso();
        if (dso1 && dso2)
          break;
      }

      if (dso1 && dso2) {
        _own_math_trace = std::make_unique<MathTrace>(true, math_stack, dso1, dso2);
        changed = true;
      } else {
        pxv_warn("View::sync_derived_traces: DsoSignal not found for "
                 "math src1=%d or src2=%d — MathTrace creation skipped.",
                 idx1, idx2);
      }
    }
  } else {
    if (_own_math_trace) {
      _own_math_trace.reset();
      changed = true;
    }
  }

  // ---- Sync LissajousTrace from LissajousModel ----
  auto *lissajous_model = source->get_lissajous_model();
  if (lissajous_model && lissajous_model->enabled()) {
    if (!_own_lissajous_trace) {
      auto *snapshot = source->get_dso_snapshot();
      _own_lissajous_trace = std::make_unique<LissajousTrace>(
          lissajous_model->enabled(), snapshot, lissajous_model->x_index(),
          lissajous_model->y_index(), lissajous_model->percent());
      changed = true;
    } else {
      _own_lissajous_trace->set_xIndex(lissajous_model->x_index());
      _own_lissajous_trace->set_yIndex(lissajous_model->y_index());
      _own_lissajous_trace->set_percent(lissajous_model->percent());
      _own_lissajous_trace->set_data(source->get_dso_snapshot());
    }
  } else {
    if (_own_lissajous_trace) {
      _own_lissajous_trace.reset();
      changed = true;
    }
  }

  // Rebuild _signal_groups if any trace was added or removed.
  if (changed) {
    _view->compute_signal_groups();
  }
}

} // namespace view
} // namespace pv
