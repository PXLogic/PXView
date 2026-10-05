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

// ---------------------------------------------------------------------------
// Single creation points for derived traces.
//
// Every DecodeTrace / SpectrumTrace MUST be created through these helpers
// so that view_index and visibility are always set at construction time.
//
// _view and _viewport are NOT set here — they are set later by
// layout_time_signals() (for DecodeTrace) or update_fft_viewport() (for
// SpectrumTrace). Until then, the trace is in a "not-yet-laid-out" state:
//   - _view is nullptr
//   - _v_offset is INT_MAX (the sentinel set by the Trace constructor)
//
// This is safe because the paint methods (paint_back / paint_mid) check
// for the INT_MAX sentinel and return early, preventing any dereference
// of _view. This is a principled defense at the paint layer rather than
// a pre-initialisation safety net that masks event-routing bugs.
// ---------------------------------------------------------------------------

std::unique_ptr<DecodeTrace> ViewDerivedTraces::create_decode_trace(
    std::shared_ptr<pv::data::DecoderStack> stack, int index) {
  auto dt = std::make_unique<DecodeTrace>(_view->session_ptr(), stack, index);
  dt->set_view(_view);
  const int restored_index = stack->view_index_hint();
  dt->set_view_index(restored_index >= 0
                         ? restored_index
                         : static_cast<int>(_view->get_own_signals().size()) + index);
  if (!stack->stack().empty() && !stack->stack().front()->shown())
    dt->set_visible(false);
  return dt;
}

std::unique_ptr<SpectrumTrace> ViewDerivedTraces::create_spectrum_trace(
    std::shared_ptr<pv::data::SpectrumStack> stack) {
  auto st = std::make_unique<SpectrumTrace>(
      _view->session_ptr(), stack, stack->get_index());
  st->set_view(_view);
  return st;
}

bool ViewDerivedTraces::add_decoder(
    srd_decoder *const dec, bool silent, DecoderStatus *dstatus,
    std::list<pv::data::decode::Decoder *> &sub_decoders,
    std::shared_ptr<pv::data::DecoderStack> &out_stack) {
  if (!_view->session_ptr())
    return false;

  out_stack = nullptr;

  // 1. Core layer creates the DecoderStack and adds it to THIS view's
  //    document stack list. Core owns the DecoderStack.
  //
  //    The target document MUST be named explicitly. Falling back to the
  //    registry's global "active document" is not equivalent: an Open-path
  //    file tab (.sr / .pxl) replays through the real capture pipeline, so
  //    TabContext::claim_active_document() is skipped for the whole duration
  //    (`if (!_session->is_working())` guard) and the active document can
  //    point at another tab — or at nothing at all, in which case
  //    SigSession::add_decoder() returns the stack via out_stack WITHOUT
  //    storing it anywhere and the decoder silently vanishes on save.
  //    Binding to document_ptr() (the render document this view was given by
  //    TabContext::restore_view_data / on_copy_to_doc_done) keeps the stack
  //    where save_workspace() reads it back from (ctx->document()).
  if (!_view->data_source()->add_decoder(dec, silent, dstatus, sub_decoders,
                                        out_stack,
                                        _view->data_sync_delegate()->document_ptr()))
    return false;

  if (!out_stack)
    return false;

  // 2. View directly creates its DecodeTrace wrapper for the new
  //    DecoderStack, using the single creation point to ensure all
  //    fields (_view, _viewport, view_index, visibility) are initialised.
  int decode_index = static_cast<int>(_own_decode_traces.size());
  auto trace = create_decode_trace(out_stack, decode_index);

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

  // 5. 命令阶段：显式触发 Core 状态收敛（模型重建 + 快照重绑）。同步执行
  //    于 start_all_decode_tasks() 之前 —— 消除旧实现的竞态（旧实现经
  //    broadcast_async 在下一拍才 reload，decode 线程已启动，依赖 reload
  //    内部的快照指针补绑兜底）。命令/通知拆分后 reload 在本拍完成。
  _view->session().apply_device_options();
  // 6. 通知阶段：API 层订阅此事件向 MCP/WS 客户端推送 DeviceConfigChanged。
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

  _view->cancel_trace_drag_interaction();

  // 1. View erases its DecodeTrace (unique_ptr auto-deletes).
  _own_decode_traces.erase(it);

  // 2. Notify Core layer to delete the corresponding DecoderStack.
  if (key_handel) {
    _view->data_source()->remove_decoder_by_key_handel(key_handel);
  }

  // 3. 命令阶段：显式触发 Core 状态收敛（命令/通知拆分约定）。
  _view->session().apply_device_options();
  // 4. 通知阶段：API 层订阅此事件向 MCP/WS 客户端推送 DeviceConfigChanged。
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});
}

void ViewDerivedTraces::remove_decoder(int index) {
  if (index < 0 || index >= static_cast<int>(_own_decode_traces.size()))
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

  _view->cancel_trace_drag_interaction();

  // 1. Clear all View-owned DecodeTrace objects (unique_ptr auto-deletes).
  _own_decode_traces.clear();

  // 2. Notify Core to clear all DecoderStacks.
  _view->data_source()->clear_all_decoder(true);

  // 3. 命令阶段：显式触发 Core 状态收敛（命令/通知拆分约定）。
  _view->session().apply_device_options();
  // 4. 通知阶段：API 层订阅此事件向 MCP/WS 客户端推送 DeviceConfigChanged。
  _view->session().broadcast_async<interface::DeviceOptionsUpdated>({});
}

void ViewDerivedTraces::sync_derived_traces() {
  if (!_derived_traces_dirty)
    return;

  _derived_traces_dirty = false;

  // 【硬约束】解码/频谱/数学轨迹是 per-tab 的：只同步【本 tab 渲染文档】
  // 持有的栈，绝不按 document_snapshot_source() 的裁决源同步。
  //
  // 原因：document_snapshot_source() 的 LiveBuffer / legacy-fallback 分支
  // 返回会话级 _data_source，而会话源的 get_decoder_stacks() 解析到的是
  // **全局 active document**。懒恢复文件 tab 激活时采集回放进行中
  // （is_working），claim_active_document 有意跳过 set_active_document，
  // "active" 仍是上一个 tab 的文档——于是本 tab 的同步把上一个 tab 的
  // 解码栈加进当前视口（幽灵轨迹：注解用别家采样率映射 x，波形/行高全部
  // 错位），本 tab 自己的解码轨迹反而被当作"栈已不存在"删掉。
  // 这与 ProtocolDock/restore 的教训同构：任何 per-document 的解析都必须
  // 显式取所属文档，绝不取"当前 active"。
  auto *doc = _view->data_sync_delegate()->document_ptr();
  if (!doc)
    return;

  bool changed = false;

  // ---- Sync DecodeTrace list from DecoderStack list ----
  auto &decoder_stacks = doc->get_decoder_stacks();

  bool decoder_structure_changed =
      decoder_stacks.size() != _own_decode_traces.size();
  if (!decoder_structure_changed) {
    for (const auto &trace : _own_decode_traces) {
      const auto *target = trace->decoder().get();
      const bool exists = std::any_of(
          decoder_stacks.begin(), decoder_stacks.end(),
          [target](const std::shared_ptr<pv::data::DecoderStack> &stack) {
            return stack.get() == target;
          });
      if (!exists) {
        decoder_structure_changed = true;
        break;
      }
    }
  }
  if (decoder_structure_changed)
    _view->cancel_trace_drag_interaction();

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
      _own_decode_traces.push_back(create_decode_trace(stack, decode_index));
      changed = true;
    }
    decode_index++;
  }

  // ---- Sync SpectrumTrace list from SpectrumStack list ----
  auto &spectrum_stacks = doc->get_spectrum_stacks();

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
      _own_spectrum_traces.push_back(create_spectrum_trace(stack));
      changed = true;
    }
  }

  // ---- Sync MathTrace from MathStack ----
  auto math_stack = doc->get_math_stack();
  if (math_stack) {
    // 解析 MathStack 的 ch1/ch2 对应的当前 DsoSignal 指针。
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

    if (!_own_math_trace ||
        _own_math_trace->get_math_stack().get() != math_stack.get()) {
      _view->cancel_trace_drag_interaction();
      if (_own_math_trace) {
        _own_math_trace.reset();
        changed = true;
      }

      if (dso1 && dso2) {
        _own_math_trace = std::make_unique<MathTrace>(true, math_stack, dso1, dso2);
        changed = true;
      } else {
        pxv_warn("View::sync_derived_traces: DsoSignal not found for "
                 "math src1=%d or src2=%d — MathTrace creation skipped.",
                 idx1, idx2);
      }
    } else if (_own_math_trace) {
      // MathStack 未变时 MathTrace 保留,但信号重建已销毁旧的源 DsoSignal
      // (其 sig_released 已把 _dsoSig1/_dsoSig2 置空)。按当前信号集重新
      // 绑定源指针,避免 MathTrace 长期持有失效的 DsoSignal 指针。
      _own_math_trace->rebind_sources(dso1, dso2);
    }
  } else {
    if (_own_math_trace) {
      _view->cancel_trace_drag_interaction();
      _own_math_trace.reset();
      changed = true;
    }
  }

  // ---- Sync LissajousTrace from LissajousModel ----
  // LissajousModel 是会话级对象（DSO XY 视图），仍经数据绑定裁决源取；
  // 判空保守处理：裁决为 None（外来采集）时跳过，不 destroy 现有轨迹。
  data::DataSource *source = _view->document_snapshot_source();
  auto *lissajous_model = source ? source->get_lissajous_model() : nullptr;
  if (lissajous_model && lissajous_model->enabled()) {
    if (!_own_lissajous_trace) {
      _view->cancel_trace_drag_interaction();
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
      _view->cancel_trace_drag_interaction();
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
