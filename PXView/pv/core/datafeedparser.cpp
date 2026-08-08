#include "pv/core/datafeedparser.h"

#include "pv/core/capturemanager.h"
#include "pv/core/decodetaskmanager.h"
#include "pv/core/documentregistry.h"
#include "pv/core/eventbus.h"
#include "pv/core/filterprocessor.h"
#include "pv/core/sessionstatecontext.h"
#include "pv/session/sigsession.h"  // SessionData full definition
#include "pv/data/snapshot/analogsnapshot.h"
#include "pv/data/snapshot/dsosnapshot.h"
#include "pv/data/snapshot/logicsnapshot.h"
#include "pv/data/stack/mathstack.h"
#include "pv/data/stack/spectrumstack.h"
#include "pv/base/log.h"

#include <QDateTime>
#include <cassert>

namespace pv {
namespace core {

DataFeedParser::DataFeedParser(EventBus *bus, ISessionState *state, ISessionCoordination *coord)
    : _event_bus(bus), _state(state), _coord(coord) {}

DataFeedParser::~DataFeedParser() {}

void DataFeedParser::feed_in_header(const sr_dev_inst *sdi) {
  (void)sdi;
  _coord->receive_header();
}

void DataFeedParser::feed_in_meta(const sr_dev_inst *sdi,
                                  const sr_datafeed_meta &meta) {
  (void)sdi;

  for (const GSList *l = meta.config; l; l = l->next) {
    const sr_config *const src = (const sr_config *)l->data;
    switch (src->key) {
    case SR_CONF_SAMPLERATE:
      /// @todo handle samplerate changes
      /// samplerate = (uint64_t *)src->value;
      break;
    }
  }
}

void DataFeedParser::feed_in_trigger() {
  // Upstream SR_DF_TRIGGER has NO payload (fork ds_trigger_pos removed).
  // Query the real trigger sample position from the driver via the
  // PXView-local SR_CONF_TRIGGER_POS key. PXLogic exposes it (returns
  // devc->trigger_pos_set); other devices return 0 (start-of-capture
  // fallback, matching prior behavior). device_agent() returns a
  // reference (never nullptr); the no-device case is handled inside
  // get_config (returns false → get_trigger_pos returns 0).
  //
  // Note: For DSO mode, the demo driver does NOT emit SR_DF_TRIGGER —
  // the trigger position is carried inside each SR_DF_DSO packet via
  // sr_datafeed_dso::trig_offset, and is processed in feed_in_dso().
  // For real DSO hardware (PXLogic), SR_DF_TRIGGER may still be emitted
  // in DSO mode, so we no longer exclude DSO here — the driver query
  // path is harmless if unsupported (returns 0).
  _coord->set_hw_replied(true);

  // Set trigger flag for ALL modes (including DSO). Previously DSO was
  // excluded, which caused _trigger_flag to stay false → trigd() returns
  // false → paint_prepare() skipped trigger-search → set_trig_hoff(0),
  // and viewport "Trig'd" status display was permanently wrong.
  _coord->set_trigger_flag(true);

  // Read the trigger position reported by the driver.
  _state->capture_data()->_trig_pos = _state->device_agent().get_trigger_pos();

  // Update trig position for current view.
  if (_state->capture_data() == _state->view_data()) {
    _coord->receive_trigger(_state->capture_data()->_trig_pos);
  }
}

void DataFeedParser::feed_in_logic(const sr_datafeed_logic &o) {
  // P2: a non-zero length with a NULL data pointer would make the underlying
  // LogicSnapshot memcpy into/append from an invalid address → segfault.
  if (o.length > 0 && o.data == nullptr) {
    pxv_err("feed_in_logic: length=%llu but data is NULL",
            (unsigned long long)o.length);
    return;
  }
  if (_state->capture_data()->get_logic()->memory_failed()) {
    pxv_err("Unexpected logic packet");
    return;
  }

  if (!_state->is_triged() && o.length > 0) {
    _coord->set_is_triged(true);
    _coord->set_trig_time(QDateTime::currentDateTime());
  }

  if (_state->capture_data()->get_logic()->last_ended()) {
    _state->capture_data()->get_logic()->set_loop(
        _capture_mgr->is_loop_mode());

    // ═══════════════════════════════════════════════════════════════════
    // FUNDAMENTAL FIX: Stop all decoder threads BEFORE first_payload()
    // modifies the snapshot.
    //
    // Root cause of repeat-mode SIGSEGV chain:
    //   copy_data_to_document() is zero-copy (shared_ptr sharing) →
    //   document's LogicSnapshot == capture's LogicSnapshot (same object) →
    //   decoder threads hold raw pointers (di->inbuf) into mmap leaf blocks →
    //   next capture's first_payload + append_payload modify the snapshot →
    //   decoder reads stale/decommitted pages → SIGSEGV.
    //
    // Previous patches tried to prevent decommit at each crash site
    // (calc_mipmap, first_payload else branch, MmapAllocator reuse),
    // but each fix just moved the crash to the next code path that
    // touches the same shared memory.
    //
    // This fix addresses the ROOT CAUSE: stop decoder threads before
    // the snapshot is modified, establishing a strict protocol:
    //   1. Capture writes to snapshot (mutable)
    //   2. Capture ends → snapshot becomes immutable, shared with document
    //   3. Decoder reads from immutable snapshot (safe)
    //   4. Next capture starts → STOP decoder → snapshot becomes mutable again
    //   5. Go to 1
    //
    // We stop decoders HERE (on the datafeed thread, which receives data
    // from the device) rather than in TrigNextCollect (on the main thread)
    // to avoid blocking the UI. The datafeed thread can afford a brief
    // wait (milliseconds) for the decoder to finish its current chunk.
    //
    // After clear_all_decode_task():
    //   - Decode worker threads are joined (finished)
    //   - di->inbuf is NULL (set by decoder after processing)
    //   - di_thread is blocked on got_new_samples_cond (not reading)
    //   - Snapshot is safe to modify
    //
    // Decoders are restarted by RevEndPacket → CopyToDocDone →
    // start_all_decode_tasks() after the new capture completes.
    // ═══════════════════════════════════════════════════════════════════
    // M4 note: clear_all_decode_task() joins decode threads here, which
    // blocks the datafeed thread. This is intentional — the snapshot
    // lifecycle protocol requires decoders to be fully stopped before
    // first_payload() can safely reset the mmap allocator. The join is
    // typically fast (milliseconds) because decoders are designed to
    // stop promptly when stop_decode_work() is called. If a decoder is
    // stuck, clear_all_decode_task() has its own internal timeout via
    // the decode thread's stop flag. No change needed for correctness;
    // the blocking is a deliberate trade-off.
    if (_decode_mgr->has_running_tasks()) {
      pxv_info("feed_in_logic: stopping decoder thread(s) before "
               "first_payload (snapshot lifecycle protocol)");
      int runDex = 0;
      _decode_mgr->clear_all_decode_task(runDex);
    }

    // In non-stream mode (view_data == capture_data), always keep leaf
    // blocks mapped (_able_free = false). This protects:
    //   (a) The UI, which reads from view_data via get_samples() without
    //       holding _mutex after the call returns — decommitting a block
    //       the UI is reading would SIGSEGV.
    //   (b) Any decoder thread that might not have fully exited yet
    //       (defense-in-depth even after clear_all_decode_task).
    // In stream mode (view_data != capture_data), the UI reads from
    // view_data (previous capture's buffer), so decommitting blocks in
    // the capture buffer is safe.
    bool bNotFree = (_state->view_data() == _state->capture_data());

    _state->capture_data()->get_logic()->first_payload(
        o, _state->device_agent().get_ring_sample_count(),
        _state->device_agent().get_channels(), !bNotFree);

    // @todo Putting this here means that only listeners querying
    // for logic will be notified. Currently the only user of
    // frame_began is DecoderStack, but in future we need to signal
    // this after both analog and logic sweeps have begun.
    _coord->frame_began();
  } else {
    // Append to the existing data snapshot
    _state->capture_data()->get_logic()->append_payload(o);
  }

  if (_state->capture_data()->get_logic()->memory_failed()) {
    _coord->set_error(SessionStateContext::Malloc_err);
    _coord->session_error();
    return;
  }

  // DSO/ANALOG 模式下可能收到 logic packet（demo 驱动始终发送 logic 数据），
  // 但 get_ch_num(SR_CHANNEL_LOGIC) 返回 0 会导致除零异常。
  const int logic_ch_num = _coord->get_ch_num(SR_CHANNEL_LOGIC);
  if (logic_ch_num > 0) {
    _coord->set_receive_data_len(o.length * 8 / logic_ch_num);
  } else {
    // 无 logic 通道时，按字节长度记录接收数据量
    _coord->set_receive_data_len(o.length);
  }

  _capture_mgr->set_data_updated(true);

  // modernize-core-layer-radical Task 13: emit DataUpdated typed event.
  // feed_in_logic runs on the libsigrok data-feed thread; use broadcast_async
  // to queue on_event(DataUpdated) onto qApp's event loop, so MainWindow's
  // handler runs on the main thread (safe to touch QWidget).
  _event_bus->broadcast_async<interface::DataUpdated>({});
}

void DataFeedParser::feed_in_analog(const sr_datafeed_analog &o) {
  // P2: guard against NULL data with non-zero sample count (memcpy would crash).
  if (o.num_samples > 0 && o.data == nullptr) {
    pxv_err("feed_in_analog: num_samples=%llu but data is NULL",
            (unsigned long long)o.num_samples);
    return;
  }
  if (_state->capture_data()->get_analog()->memory_failed()) {
    pxv_err("Unexpected analog packet");
    return; // This analog packet was not expected.
  }

  if (_state->capture_data()->get_analog()->last_ended()) {
    // In multi-tab architecture, SigSession::_signals do not have viewports,
    // so we cannot and should not call UI rendering methods on them.

    // first payload
    _state->capture_data()->get_analog()->first_payload(
        o, _state->device_agent().get_ring_sample_count(),
        _state->device_agent().get_channels());
    _coord->frame_began();
  } else {
    // Append to the existing data snapshot
    _state->capture_data()->get_analog()->append_payload(o);
  }

  if (_state->capture_data()->get_analog()->memory_failed()) {
    _coord->set_error(SessionStateContext::Malloc_err);
    _coord->session_error();
    return;
  }

  // Only track progress for ANALOG mode. In LOGIC/MSO mode the logic
  // packet is the primary data source and already calls
  // set_receive_data_len in feed_in_logic. Counting analog samples here
  // too would multiply _sample_received by (1 + num_analog_channels),
  // making the progress bar reach 100% long before the capture finishes.
  const int mode = _state->device_agent().get_work_mode();
  if (mode == ANALOG) {
    _coord->set_receive_data_len(o.num_samples);
  }
  _capture_mgr->set_data_updated(true);

  // modernize-core-layer-radical Task 13: emit DataUpdated (async, worker thread).
  _event_bus->broadcast_async<interface::DataUpdated>({});
}

void DataFeedParser::feed_in_dso(const sr_datafeed_dso &o) {
  // Hot-path debug logging removed for performance — was printing 40+ lines/sec
  // P2: guard against NULL data with non-zero sample count (memcpy would crash).
  if (o.num_samples > 0 && o.data == nullptr) {
    pxv_err("feed_in_dso: num_samples=%llu but data is NULL",
            (unsigned long long)o.num_samples);
    return;
  }
  if (_state->capture_data()->get_dso()->memory_failed()) {
    pxv_err("Unexpected dso packet");
    return;
  }

  if (!_state->is_triged() && o.num_samples > 0) {
    _coord->set_is_triged(true);
    _coord->set_trig_time(QDateTime::currentDateTime());
  }

  // Record the sample count BEFORE first_payload/append_payload — needed
  // to compute the absolute trigger position from o.trig_offset (which
  // is relative to the current packet's first sample).
  const uint64_t pre_sample_count =
      _state->capture_data()->get_dso()->get_sample_count();

  if (_state->capture_data()->get_dso()->last_ended()) {
    // first payload
    _state->capture_data()->get_dso()->first_payload(
        o, _state->device_agent().get_ring_sample_count(),
        _state->device_agent().get_channels(),
        _capture_mgr->is_instant(),
        _state->device_agent().is_file());
    _coord->frame_began();
  } else {
    // Append to the existing data snapshot
    _state->capture_data()->get_dso()->append_payload(o);
  }

  if (_state->capture_data()->get_dso()->memory_failed()) {
    _coord->set_error(SessionStateContext::Malloc_err);
    _coord->session_error();
    return;
  }

  // Port from upstream DSView's feed_in_dso (Reference/DSView-master/
  // DSView/pv/sigsession.cpp:1343-1344). Without this, _trigger_flag stays
  // false in DSO mode → trigd() returns false → paint_prepare() skips
  // trigger-search → set_trig_hoff(0), and viewport "Trig'd" status display
  // is permanently wrong. Also, downstream receive_trigger() is never
  // invoked for DSO mode → horizontal trigger cursor stays stale.
  if (o.trig_flag) {
    _coord->set_trigger_flag(true);
    _coord->set_trigger_ch(o.trig_ch);

    // o.trig_offset is the trigger sample offset WITHIN the current DSO
    // packet. Convert to absolute sample index by adding the sample count
    // that existed before this packet was appended. This is the upstream
    // replacement for the fork ds_trigger_pos.real_pos field that the
    // original DSView used in its feed_in_trigger DSO else-branch.
    uint64_t abs_trig_pos = pre_sample_count +
                            (o.trig_offset > 0 ? (uint64_t)o.trig_offset : 0);
    _state->capture_data()->_trig_pos = abs_trig_pos;

    // Update trig position for current view (DSO mode has
    // capture_data == view_data, so the receive_trigger path is taken).
    if (_state->capture_data() == _state->view_data()) {
      _coord->receive_trigger(_state->capture_data()->_trig_pos);
    }
  }

  // Sync the snapshot samplerate so paint_mid's samples_per_pixel uses the
  // current acquisition's samplerate (port from Reference/DSView-master/
  // DSView/pv/sigsession.cpp:1319). Without this, downstream code reading
  // cur_snap_samplerate() may use a stale value (e.g. 0) → samples_per_pixel
  // is incorrect → waveform may render at the wrong scale or not at all.
  const uint64_t cur_samplerate = _state->device_agent().get_sample_rate();
  if (cur_samplerate > 0) {
    _coord->set_cur_snap_samplerate(cur_samplerate);
  }

  // Only track progress for DSO mode. In other modes (LOGIC/MSO/ANALOG)
  // the corresponding primary feed_in_* handler already calls
  // set_receive_data_len. Counting DSO samples here in addition would
  // inflate _sample_received and make the progress bar reach 100% early.
  const int mode = _state->device_agent().get_work_mode();
  if (mode == DSO) {
    _coord->set_receive_data_len(o.num_samples);
  }
  _capture_mgr->set_data_updated(true);

  // modernize-core-layer-radical Task 13: emit DataUpdated (async, worker thread).
  _event_bus->broadcast_async<interface::DataUpdated>({});
}

void DataFeedParser::data_feed_in(const struct sr_dev_inst *sdi,
                                  const struct sr_datafeed_packet *packet) {
  if (!sdi) {
    pxv_warn("%s", "SigSession::data_feed_in: sdi is nullptr");
    return;
  }
  if (!packet) {
    pxv_warn("%s", "SigSession::data_feed_in: packet is nullptr");
    return;
  }
  // Track C1: redundant assert(sdi)/assert(packet) removed —
  // already guarded by nullptr checks + early return above.

  // Static packet counter removed — was only used by the removed timing log.

  ds_lock_guard lock(_state->data_mutex());

  if (_capture_mgr->is_data_lock() && packet->type != SR_DF_END)
    return;

  // Upstream sr_datafeed_packet has no `status` field (fork-only).
  // Error checking is now done via SR_DF_END handling and session stopped
  // callback.

  switch (packet->type) {
  case SR_DF_HEADER:
    feed_in_header(sdi);
    break;

  case SR_DF_META:
    // P2: assert() is compiled out in Release builds — a NULL payload would
    // dereference and segfault. Guard with a hard early-return instead.
    if (!packet->payload) {
      pxv_err("SR_DF_META packet with NULL payload");
      break;
    }
    feed_in_meta(sdi, *(const sr_datafeed_meta *)packet->payload);
    break;

  case SR_DF_TRIGGER:
    // Upstream SR_DF_TRIGGER has NO payload.
    feed_in_trigger();
    break;

  case SR_DF_LOGIC:
    // P2: see above — never trust payload in Release builds.
    if (!packet->payload) {
      pxv_err("SR_DF_LOGIC packet with NULL payload");
      break;
    }
    feed_in_logic(*(const sr_datafeed_logic *)packet->payload);
    break;

  case SR_DF_ANALOG:
    if (!packet->payload) {
      pxv_err("SR_DF_ANALOG packet with NULL payload");
      break;
    }
    feed_in_analog(*(const sr_datafeed_analog *)packet->payload);
    break;

  case SR_DF_DSO:
    if (!packet->payload) {
      pxv_err("SR_DF_DSO packet with NULL payload");
      break;
    }
    feed_in_dso(*(const sr_datafeed_dso *)packet->payload);
    break;

  case SR_DF_END: {
    _state->capture_data()->get_logic()->capture_ended();
    _state->capture_data()->get_dso()->capture_ended();
    _state->capture_data()->get_analog()->capture_ended();

    // CRITICAL FIX: fork 迁移遗漏 — 采集正常结束时设置 device_status =
    // ST_STOPPED。旧 fork libsigrok 在 ds_stop_collect 内部会通过 sr_status
    // 结构体设置 device_status；上游 libsigrok 0.6 无此机制，导致
    // _device_status 永远停在 ST_RUNNING/ST_INIT，is_stopped_status() 恒为
    // false，viewport_painter.cpp 的 doPaint 无法进入 paintSignals 分支。
    _state->set_device_status(ST_STOPPED);

    int mode = _state->device_agent().get_work_mode();

    // Post a message to start all decode tasks.
    if (mode == LOGIC) {
      _event_bus->broadcast_async<interface::RevEndPacket>({});
    } else {
      _state->frame_ended();

      // MSO 模式包含 LOGIC 通道与 ANALOG 通道。LOGIC 模式通过 RevEndPacket 触发
      // copy_data_to_document + CopyToDocDone + set_data_document 来绑定 analog
      // 信号的 _data 指针；MSO 模式走本 else 分支，原代码遗漏了 RevEndPacket 广播，
      // 导致 copy_data_to_document 不执行、analog 信号 _data 恒为 null，
      // paint_mid() 直接 return，模拟通道波形空白。此处补发 RevEndPacket 复用
      // on_event(RevEndPacket) 已有的 MSO 处理分支。
      if (mode == MSO) {
        _event_bus->broadcast_async<interface::RevEndPacket>({});
      }

      // 解码器启动策略：
      //  - MSO 模式：RevEndPacket 路径会在 copy_data_to_document +
      //    CopyToDocDone 之后由 on_event(CopyToDocDone) 启动解码器，故此处
      //    不再重复启动，避免重复 start_all_decode_tasks。
      //  - DSO / ANALOG 模式：无 LOGIC 通道，不触发 RevEndPacket，此处直接
      //    启动解码器。
      // CaptureOwnerGuard 的释放 + EndCollectWork 广播由 SessionStopped 事件
      // 统一处理（在 DeviceAgent worker 线程的 sr_session_run() 返回后触发），
      // 而不再在 SR_DF_END 时提前释放。SR_DF_END 触发时 libsigrok 的 main
      // loop 可能仍在运行，提前释放 guard 会让第二次 sr_session_start() 与
      // 上一次 session 的停止发生竞争。
      // repeat/loop 模式下也由 SessionStopped 处理，但 SessionStopped 只在
      // _is_working 为 true 时才释放 guard —— repeat 模式每帧的 guard 释放
      // 由 TrigNextCollect / stop_capture 路径负责。
      if (mode != MSO) {
        _decode_mgr->start_all_decode_tasks();
      }

      // 架构修复：MSO 模式包含 LOGIC 通道，采集完成后若启用 auto-apply
      // 且有保存的 thresholds，则自动重新应用毛刺滤波。
      // LOGIC 模式走 RevEndPacket 路径已在 on_event(RevEndPacket) 中处理；
      // MSO 模式走本 else 分支，原代码遗漏了 auto-apply。
      if (mode == MSO &&
          _state->view_data()->_glitch_filter_auto_apply &&
          !_state->view_data()->_glitch_filter_thresholds.empty() &&
          _state->view_data()->get_logic() &&
          !_state->view_data()->get_logic()->empty() &&
          _state->filter_processor()) {
        _state->filter_processor()->set_glitch_filter(
            _state->view_data()->_glitch_filter_thresholds,
            _state->view_data()->_glitch_filter_modes);
      }
    }

    break;
  }
  }

  // Hot-path timing logging removed for performance — was printing on every
  // data feed packet (40+ lines/sec in DSO continuous mode).
}

void DataFeedParser::data_feed_callback_ex(const struct sr_dev_inst *sdi,
                                           const struct sr_datafeed_packet *packet,
                                           void *user_data) {
  if (!user_data) {
    pxv_warn("%s", "SigSession::data_feed_callback_ex: user_data is nullptr");
    return;
  }
  assert(user_data);
  static_cast<DataFeedParser *>(user_data)->data_feed_in(sdi, packet);
}

} // namespace core
} // namespace pv
