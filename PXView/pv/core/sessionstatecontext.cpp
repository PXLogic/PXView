#include "sessionstatecontext.h"

#include "capturemanager.h"
#include "decodetaskmanager.h"
#include "documentregistry.h"
#include "eventbus.h"
#include "../sigsession.h"  // SessionData full definition
#include "../data/analogsnapshot.h"
#include "../data/decoderstack.h"
#include "../data/dsosnapshot.h"
#include "../data/logicsnapshot.h"
#include "../data/sessiondocument.h"
#include "../data/signalmodel.h"
#include "../data/spectrumstack.h"
#include "../interface/events.h"
#include "../interface/icallbacks.h"
#include "../log.h"

#include <assert.h>

namespace pv {
namespace core {

namespace {
// File-local empty stack returned by get_decoder_stacks() when no document is
// active. Mirrors the legacy SigSession::_empty_decoder_stacks static.
std::vector<std::shared_ptr<data::DecoderStack>> _empty_decoder_stacks;
} // namespace

SessionStateContext::SessionStateContext() {
  _sampling_mutex = std::make_unique<std::mutex>();
  _data_mutex = std::make_unique<std::mutex>();

  _data_list.push_back(new SessionData());
  _data_list.push_back(new SessionData());
  _view_data = _data_list[0];
  _capture_data = _data_list[0];
}

SessionStateContext::~SessionStateContext() {
  for (auto p : _data_list) {
    if (p) {
      p->clear();
      delete p;
    }
  }
  _data_list.clear();
}

// --- EventBus dispatch helpers (migrated from SigSession) -------------------

void SessionStateContext::data_updated() {
  _event_bus->dispatch_to<IDataCallback>(
      [](IDataCallback *cb) { cb->data_updated(); });
}

void SessionStateContext::set_receive_data_len(quint64 len) {
  _event_bus->dispatch_to<IDataCallback>(
      [len](IDataCallback *cb) { cb->receive_data_len(len); });
}

void SessionStateContext::receive_header() {
  _event_bus->dispatch_to<IDataCallback>(
      [](IDataCallback *cb) { cb->receive_header(); });
}

void SessionStateContext::cur_snap_samplerate_changed() {
  _event_bus->dispatch_to<IDataCallback>(
      [](IDataCallback *cb) { cb->cur_snap_samplerate_changed(); });
}

void SessionStateContext::frame_began() {
  _event_bus->dispatch_to<ICaptureCallback>(
      [](ICaptureCallback *cb) { cb->frame_began(); });
}

void SessionStateContext::frame_ended() {
  _event_bus->dispatch_to<ICaptureCallback>(
      [](ICaptureCallback *cb) { cb->frame_ended(); });
}

void SessionStateContext::update_capture() {
  _event_bus->dispatch_to<ICaptureCallback>(
      [](ICaptureCallback *cb) { cb->update_capture(); });
}

void SessionStateContext::repeat_hold(int percent) {
  _event_bus->dispatch_to<ICaptureCallback>(
      [percent](ICaptureCallback *cb) { cb->repeat_hold(percent); });
}

void SessionStateContext::receive_trigger(quint64 trigger_pos) {
  _event_bus->dispatch_to<ITriggerCallback>(
      [trigger_pos](ITriggerCallback *cb) {
        cb->receive_trigger(trigger_pos);
      });
}

void SessionStateContext::show_wait_trigger() {
  _event_bus->dispatch_to<ITriggerCallback>(
      [](ITriggerCallback *cb) { cb->show_wait_trigger(); });
}

void SessionStateContext::signals_changed() {
  _event_bus->dispatch_to<ISessionStateCallback>(
      [](ISessionStateCallback *cb) { cb->signals_changed(); });
  // 异步广播:避免在 on_event handler 中(如 on_event(DeviceOptionsUpdated)
  // → reload() → signals_changed())同步触发广播导致 _broadcast_depth>1 断言;
  // 同时保证 task thread 调用时的线程安全(Qt::QueuedConnection marshal 到主线程)。
  _event_bus->broadcast_async<interface::SignalsChanged>({});
}

void SessionStateContext::session_error() {
  _event_bus->dispatch_to<ISessionStateCallback>(
      [](ISessionStateCallback *cb) { cb->session_error(); });
}

void SessionStateContext::delay_prop_msg(QString strMsg) {
  _event_bus->dispatch_to<ISessionStateCallback>(
      [strMsg](ISessionStateCallback *cb) { cb->delay_prop_msg(strMsg); });
}

// --- Cross-manager helpers (migrated from SigSession) -----------------------

std::vector<std::shared_ptr<data::DecoderStack>> &
SessionStateContext::get_decoder_stacks(data::SessionDocument *doc) {
  data::SessionDocument *target =
      doc ? doc : _document_registry->get_active_document();
  return target ? target->get_decoder_stacks() : _empty_decoder_stacks;
}

std::vector<std::shared_ptr<data::DecoderStack>> &
SessionStateContext::decode_traces(data::SessionDocument *doc) {
  return get_decoder_stacks(doc);
}

std::shared_ptr<data::DecoderStack>
SessionStateContext::get_decoder_trace(int index, data::SessionDocument *doc) {
  auto &traces = decode_traces(doc);
  if (index >= 0 && index < (int)traces.size()) {
    return traces[index];
  }
  assert(false);
  return nullptr;
}

int SessionStateContext::get_trace_index_by_key_handel(void *handel,
                                                       data::SessionDocument *doc) {
  int dex = 0;
  for (auto stack : decode_traces(doc)) {
    if (stack->get_key_handel() == handel) {
      return dex;
    }
    ++dex;
  }
  return -1;
}

void SessionStateContext::clear_all_decode_task2() {
  _decode_task_manager->clear_all_decode_task2();
}

void SessionStateContext::add_decode_task(
    std::shared_ptr<data::DecoderStack> stack) {
  _decode_task_manager->add_decode_task(stack);
}

void SessionStateContext::attach_data_to_signal(SessionData *data) {
  _decode_task_manager->attach_data_to_signal(data);
}

uint16_t SessionStateContext::get_ch_num(int type) {
  uint16_t num_channels = 0;
  uint16_t logic_ch_num = 0;
  uint16_t dso_ch_num = 0;
  uint16_t analog_ch_num = 0;

  if (_device_agent.have_instance()) {
    for (auto m : _signal_models) {
      if (!m->enabled())
        continue;

      if (m->type() == SR_CHANNEL_LOGIC)
        logic_ch_num++;
      else if (m->type() == SR_CHANNEL_DSO)
        dso_ch_num++;
      else if (m->type() == SR_CHANNEL_ANALOG)
        analog_ch_num++;
    }
  }

  switch (type) {
  case SR_CHANNEL_LOGIC:
    num_channels = logic_ch_num;
    break;
  case SR_CHANNEL_DSO:
    num_channels = dso_ch_num;
    break;
  case SR_CHANNEL_ANALOG:
    num_channels = analog_ch_num;
    break;
  default:
    num_channels = logic_ch_num + dso_ch_num + analog_ch_num;
    break;
  }

  return num_channels;
}

uint64_t SessionStateContext::cur_samplelimits() {
  return _capture_data->_cur_samplelimits;
}

uint64_t SessionStateContext::cur_snap_samplerate() {
  return _capture_data->_cur_snap_samplerate;
}

void SessionStateContext::set_cur_snap_samplerate(uint64_t samplerate) {
  if (samplerate == 0) {
    pxv_warn("SessionStateContext::set_cur_snap_samplerate: samplerate=0, ignoring (device may not support SR_CONF_SAMPLERATE)");
    return;
  }

  _capture_data->_cur_snap_samplerate = samplerate;
  _capture_data->get_logic()->set_samplerate(samplerate);
  _capture_data->get_analog()->set_samplerate(samplerate);
  _capture_data->get_dso()->set_samplerate(samplerate);

  int mode = _device_agent.get_work_mode();

  if (mode == DSO) {
    for (auto m : _signal_models) {
      if (m->type() == SR_CHANNEL_DSO) {
        _capture_data->get_dso()->set_measure_voltage_factor(
            (uint64_t)m->vfactor(), m->index());
        _capture_data->get_dso()->set_data_scale(m->vdiv(), m->index());
      }
    }
  }

  for (auto d : decode_traces()) {
    d->set_samplerate(samplerate);
  }

  if (_math_stack)
    _math_stack->set_samplerate(_device_agent.get_sample_rate());
  for (auto m : _spectrum_stacks) {
    m->set_samplerate(samplerate);
  }

  cur_snap_samplerate_changed();
}

void SessionStateContext::set_cur_samplelimits(uint64_t samplelimits) {
  if (samplelimits == 0) {
    pxv_warn("SessionStateContext::set_cur_samplelimits: samplelimits=0, ignoring (device may not support SR_CONF_LIMIT_SAMPLES)");
    return;
  }
  _capture_data->_cur_samplelimits = samplelimits;
  _event_bus->dispatch_to<ICaptureCallback>(
      [](ICaptureCallback *cb) { cb->cur_samplelimits_changed(); });
}

void SessionStateContext::sync_trigger_to_libsigrok(bool disable_trigger) {
  // Core→libsigrok 触发配置唯一同步点。在 sr_session_start 前一次性同步。
  //
  // Fork libsigrok 删除后，ds_trigger_* API 不复存在。改用上游 sr_trigger_*
  // API 同步 simple trigger。Adv/Serial trigger 字段保留在 TriggerConfig 中
  // 但暂不下发（UI 保留供 PXLogic 驱动未来扩展）。
  //
  // instant 模式（disable_trigger=true）：清除 session 上的 sr_trigger，
  // 让所有 driver 都不等待触发：
  //   - demo/fx2lafw: sr_session_trigger_get 返回 NULL → 不创建
  //     soft_trigger_logic → 持续发送数据（恢复旧版 fork demo 行为）
  //   - pxlogic: set_trigger() 走 "No session trigger set" 分支 →
  //     trig_zero/one/fall/rise 全 0 → 硬件不配置触发位图
  // 这样统一处理，避免在每个 driver 内部单独判断 instant 标志。
  if (disable_trigger) {
    if (_device_agent.sr_session()) {
      struct sr_trigger *old =
          sr_session_trigger_get(_device_agent.sr_session());
      if (old) {
        sr_trigger_free(old);
      }
      sr_session_trigger_set(_device_agent.sr_session(), nullptr);
      pxv_info("sync_trigger_to_libsigrok: instant mode, trigger disabled "
               "(all drivers skip trigger wait)");
    }
    return;
  }

  const auto &cfg = _trigger_config;

  // Only Simple trigger mode is synced to the driver. Adv/Serial trigger
  // configurations are retained in TriggerConfig for future PXLogic driver
  // extension but not currently synced (stub).
  if (cfg.mode() != data::TriggerConfig::Simple) {
    pxv_info("sync_trigger_to_libsigrok: Adv/Serial trigger not synced (stub)");
    return;
  }

  // Build an upstream sr_trigger from the SignalModel trig_type fields.
  struct sr_trigger *trig = sr_trigger_new("pxview");
  if (!trig) {
    pxv_err("sync_trigger_to_libsigrok: sr_trigger_new failed");
    return;
  }

  struct sr_trigger_stage *stage = sr_trigger_stage_add(trig);
  if (!stage) {
    sr_trigger_free(trig);
    return;
  }

  bool any_triggered = false;
  for (const auto &m : _signal_models) {
    if (!m || m->type() != SR_CHANNEL_LOGIC)
      continue;

    // Find the sr_channel for this SignalModel index.
    struct sr_channel *ch = nullptr;
    for (const GSList *l = _device_agent.get_channels(); l; l = l->next) {
      struct sr_channel *probe = (struct sr_channel *)l->data;
      if (probe && probe->index == m->index()) {
        ch = probe;
        break;
      }
    }
    if (!ch)
      continue;

    int match = 0;
    switch (m->trig_type()) {
    case data::SignalModel::POSTRIG:  match = SR_TRIGGER_RISING;  any_triggered = true; break;
    case data::SignalModel::NEGTRIG:  match = SR_TRIGGER_FALLING; any_triggered = true; break;
    case data::SignalModel::HIGTRIG:  match = SR_TRIGGER_ONE;     any_triggered = true; break;
    case data::SignalModel::LOWTRIG:  match = SR_TRIGGER_ZERO;    any_triggered = true; break;
    case data::SignalModel::EDGTRIG:  match = SR_TRIGGER_EDGE;    any_triggered = true; break;
    case data::SignalModel::NONTRIG:
    default: continue; // skip non-trigger channels
    }

    if (sr_trigger_match_add(stage, ch, match, 0.0f) != SR_OK) {
      pxv_warn("sync_trigger_to_libsigrok: sr_trigger_match_add failed for ch %d", m->index());
    }
  }

  // sr_session_trigger_set does NOT copy the trigger — it stores the pointer
  // directly (session->trigger = trig). The session reads this pointer during
  // sr_session_start (verify_trigger) and drivers (e.g. PXLogic) read it via
  // sr_session_trigger_get throughout acquisition. Freeing it here would leave
  // session->trigger dangling → use-after-free → capture fails to start and
  // repeated attempts crash from heap corruption. The session takes ownership;
  // we only free the previous trigger to avoid leaking across captures.
  if (_device_agent.sr_session()) {
    struct sr_trigger *old = sr_session_trigger_get(_device_agent.sr_session());
    if (old)
      sr_trigger_free(old);
    if (any_triggered) {
      sr_session_trigger_set(_device_agent.sr_session(), trig);
      pxv_info("sync_trigger_to_libsigrok: simple trigger synced (%d matches)",
               stage->matches ? g_slist_length(stage->matches) : 0);
      // Session owns trig now; do NOT free it here.
    } else {
      sr_session_trigger_set(_device_agent.sr_session(), nullptr);
      pxv_info("sync_trigger_to_libsigrok: no trigger matches, trigger disabled");
      sr_trigger_free(trig);
    }
  } else {
    sr_trigger_free(trig);
  }
}

void SessionStateContext::clear_glitch_filter_state_for_capture() {
  // 新采集开始时调用:清除滤波激活状态和 backup,
  // 但保留 thresholds/modes(供 auto-apply 使用)。
  // 不恢复数据 — _view_data->get_logic() 已被 clear(),无数据可恢复。
  if (_view_data->_logic_backup) {
    delete _view_data->_logic_backup;
    _view_data->_logic_backup = nullptr;
  }
  if (_view_data->_glitch_filter_active) {
    _view_data->_glitch_filter_active = false;
    _event_bus->broadcast_async<interface::GlitchFilterCleared>({});
  }
}

} // namespace core
} // namespace pv
