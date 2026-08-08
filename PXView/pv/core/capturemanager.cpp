#include "capturemanager.h"

#include "eventbus.h"
#include "documentregistry.h"
#include "sessionstatecontext.h"
#include "../sigsession.h"  // SessionData full definition + ds_lock_guard typedef
#include "../data/decoderstack.h"
#include "../data/sessiondocument.h"
#include "../data/signalmodel.h"
#include "../data/spectrumstack.h"
#include "../data/mathstack.h"
#include "../data/lissajousmodel.h"
#include "../log.h"
#include "../ui/langresource.h"
#include "../ui/msgbox.h"
#include "../utility/path.h"
#include "../config/appconfig.h"

#include <QDateTime>
#include <QDir>
#include <QString>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

#include <libsigrok/libsigrok.h>

namespace pv {
namespace core {

// File-local helper duplicated from sigsession.cpp (used by action_start_capture
// to compute the default disk-cache path when the user hasn't set one).
// Kept file-local to avoid leaking the symbol into the public API.
static QString get_default_disk_cache_path() {
  return QDir::tempPath() + "/PXView_cache";
}

CaptureManager::CaptureManager(EventBus *bus, ISessionState *state, ISessionCoordination *coord)
    : _event_bus(bus), _state(state), _coord(coord),
      _clt_mode(COLLECT_SINGLE) {
  _feed_timer.SetCallback(std::bind(&CaptureManager::feed_timeout, this));
  _repeat_timer.SetCallback(
      std::bind(&CaptureManager::repeat_capture_wait_timeout, this));
  _repeat_wait_prog_timer.SetCallback(
      std::bind(&CaptureManager::repeat_wait_prog_timeout, this));
  _refresh_rt_timer.SetCallback(
      std::bind(&CaptureManager::realtime_refresh_timeout, this));
  _trig_check_timer.SetCallback(
      std::bind(&CaptureManager::trig_check_timeout, this));
}

CaptureManager::~CaptureManager() = default;

void CaptureManager::capture_init() {
  // SR_CONF_INSTANT fork key deleted from pxlogic.c — the driver no longer
  // reads instant mode. The application-layer _is_instant flag (accessed via
  // is_instant()/set_is_instant()) is the sole source of truth and is used
  // directly by capture logic (e.g. is_repeat_action()).
  _coord->update_capture();

  // Store samplerate/limits before clear() (these are device properties,
  // not snapshot state). set_cur_snap_samplerate() will be called again
  // AFTER clear() to inject the samplerate into the NEW snapshots.
  const uint64_t dev_samplerate = _state->device_agent().get_sample_rate();
  const uint64_t dev_samplelimits = _state->device_agent().get_sample_limit();

  _data_updated.store(false);
  _coord->set_trigger_flag(false);
  _coord->set_trigger_ch(0);
  _coord->set_hw_replied(false);
  _rt_refresh_time_id.store(0);

  // CRITICAL FIX (fork 迁移遗漏): 旧版 fork libsigrok 在 DS_EV_DEVICE_RUNNING
  // 事件中调用 set_receive_data_len(0) 重置 viewport 的 _sample_received 进度，
  // 使进度条显示"等待触发"状态而非上一次采集的 100%。新版 upstream libsigrok
  // 无此事件，capture_init() 是 exec_capture() 启动设备前最后的状态重置点，
  // 必须在此重置进度，否则用户在等待触发时会看到残留的 100% 进度。
  // set_receive_len(0) 内部会 start_trigger_timer(333) 并将 _transfer_started=false。
  _coord->set_receive_data_len(0);
  _rt_ck_refresh_time_id.store(0);
  _noData_cnt.store(0);

  data_unlock();

  // Init data container — clear() creates fresh snapshots (samplerate=0).
  _state->capture_data()->clear();

  // CRITICAL: set_cur_snap_samplerate MUST be called AFTER clear(), because
  // clear() replaces the shared_ptr snapshots with new instances. Calling it
  // before clear() sets the samplerate on the OLD snapshots, which are then
  // discarded. The new snapshots would have _samplerate=0, causing
  // AnalogSignal::paint_mid to compute samples_per_pixel=0 → flat-line waveform.
  // get_logic() has a fallback injection, but get_analog()/get_dso() did not
  // (fixed in sessiondata.h). This explicit call after clear() is the
  // authoritative path; the get_*() injection is a safety net.
  _coord->set_cur_snap_samplerate(dev_samplerate);
  _coord->set_cur_samplelimits(dev_samplelimits);

  _state->capture_data()->get_logic()->set_disk_cache_config(_disk_cache_config);

  int mode = _state->device_agent().get_work_mode();
  if (mode == DSO) {
    for (auto m : _state->spectrum_stacks()) {
      m->init();
    }

    if (_state->math_stack()) {
      _state->math_stack()->init();
    }
  }

  // In multi-tab architecture, SigSession::_signals do not have viewports.
  // We cannot call UI-dependent methods (like set_zero_ratio) on them here.
  // Hardware offset is already updated via View's own signal events when user
  // changes it.

  // Start timer
  if (mode == DSO || mode == ANALOG)
    _feed_timer.Start(CaptureManager::FeedInterval);
  else
    _feed_timer.Stop();
}

bool CaptureManager::start_capture(bool instant, data::SessionDocument *owner) {
  _is_action.store(true);
  int ret = action_start_capture(instant, owner);
  _is_action.store(false);
  return ret;
}

bool CaptureManager::action_start_capture(bool instant,
                                          data::SessionDocument *owner) {
  assert(_event_bus && _event_bus->has_listeners());

  pxv_info("Start collect.");

  if (_state->is_working()) {
    pxv_err("Error! Is working now.");
    return false;
  }

  if (_state->signal_models().empty()) {
    pxv_info("ERROR: channel list is empty, unable to capture data.");
    return false;
  }

  // Check that a device instance has been selected.
  if (_state->device_agent().have_instance() == false) {
    pxv_err("Error!No device selected");
    assert(false);
    return false;
  }
  if (_state->device_status() == ST_RUNNING ||
      _state->device_agent().is_collecting()) {
    pxv_err("Error!Device is running.");
    return false;
  }

  _coord->clear_all_decode_task2();
  clear_decode_result();

  _state->capture_data()->clear();
  _state->view_data()->clear();
  // 清除毛刺滤波状态(backup 悬垂、active 标志过期),保留 thresholds/modes
  // 供 auto-apply 使用
  _coord->clear_glitch_filter_state_for_capture();
  _is_stream_mode.store(false);
  _capture_times.store(0);
  _dso_packet_count.store(0);

  _coord->set_capture_data(_state->view_data());
  _coord->set_cur_snap_samplerate(_state->device_agent().get_sample_rate());
  _coord->set_cur_samplelimits(_state->device_agent().get_sample_limit());

  _coord->set_session_time(QDateTime::currentDateTime());

  int mode = _state->device_agent().get_work_mode();
  if (mode == LOGIC) {
    if (is_repeat_mode() && _state->device_agent().is_hardware() &&
        _state->device_agent().is_stream_mode()) {
      set_repeat_intvl(0.1);
    }

    if (_state->device_agent().is_hardware()) {
      _is_stream_mode.store(_state->device_agent().is_stream_mode());
    } else if (_state->device_agent().is_demo() ||
               _state->device_agent().is_file()) {
      _is_stream_mode.store(true);
    }

    if (is_loop_mode() && !_is_stream_mode.load()) {
      set_collect_mode(COLLECT_SINGLE); // Reset the capture mode.
    }

    /* Removed: demo-specific loop+pattern restriction that forced COLLECT_SINGLE
     * when pattern != "random". pxlogic has no such restriction — loop mode
     * should work with any pattern for test consistency. The demo driver's
     * loop_mode now skips limit_samples entirely (matching pxlogic's is_loop=1),
     * so all patterns stream correctly in loop mode. */

    if (_state->device_agent().is_hardware() ||
        _state->device_agent().is_demo()) {
      bool bv = is_loop_mode() && _is_stream_mode.load();
      _state->device_agent().set_config_bool(SR_CONF_LOOP_MODE, bv);
    }
  }

  if (mode == DSO && _state->device_agent().is_hardware()) {
    uint32_t ref_max = 0;
    uint32_t ref_min = 0;
    _state->device_agent().get_config_uint32(SR_CONF_REF_MIN, ref_min);
    _state->device_agent().get_config_uint32(SR_CONF_REF_MAX, ref_max);
    _state->view_data()->get_dso()->set_ref_range(ref_max, ref_min);
  }

  _event_bus->broadcast_async<interface::CaptureStateChanged>(
      {_state->is_working(), _state->device_status()});

  bool disk_cache_enabled = false;
  // Disk cache is an application-layer feature (LogicSnapshotDiskCacheWriter
  // and MmapAllocator do not depend on any driver). Query SR_CONF_DISK_CACHE_ENABLE
  // if the driver supports it (DSL/PXLogic), otherwise use default (disabled).
  // This lets fx2lafw and other upstream streaming devices enable disk cache
  // without driver-level support — the feature lives in the app layer.
  _state->device_agent().get_config_bool(SR_CONF_DISK_CACHE_ENABLE,
                                         disk_cache_enabled);
  if (disk_cache_enabled) {
    QString cache_path;
    _state->device_agent().get_config_string(SR_CONF_DISK_CACHE_PATH,
                                               cache_path);
    if (cache_path.isEmpty()) {
      cache_path = get_default_disk_cache_path();
      _state->device_agent().set_config_string(SR_CONF_DISK_CACHE_PATH,
                                                cache_path.toUtf8().data());
    }
  }

  _disk_cache_config.enabled = false;

  pxv_info(
      "SigSession::start_capture: _is_stream_mode=%d, disk_cache_enabled=%d",
      _is_stream_mode.load(), disk_cache_enabled);

  if (_is_stream_mode.load() && disk_cache_enabled) {
    _disk_cache_config.enabled = true;

    QString cache_path;
    _state->device_agent().get_config_string(SR_CONF_DISK_CACHE_PATH,
                                               cache_path);
    if (cache_path.isEmpty()) {
      cache_path = get_default_disk_cache_path();
    }
    _disk_cache_config.cache_path = cache_path.toStdString();

    double disk_gb = 16;
    _state->device_agent().get_config_double(SR_CONF_STREAM_BUFF, disk_gb);
    _disk_cache_config.total_cache_depth_gb = (uint64_t)disk_gb;
    _disk_cache_config.memory_size_gb =
        0; // mmap mode: all data goes to disk file
    _disk_cache_config.calculate();

    uint64_t bytes_per_block = 2105376;
    _disk_cache_config.hot_window_blocks = _disk_cache_config.memory_size_gb *
                                           1024ULL * 1024 * 1024 /
                                           bytes_per_block;

    pxv_info("SigSession::start_capture: Configured disk cache: "
             "disk_gb=%f, path=%s",
             disk_gb, _disk_cache_config.cache_path.c_str());
  } else {
    pxv_info("SigSession::start_capture: Disk cache NOT configured.");
  }

  // update setting
  if (_state->device_agent().is_file())
    _is_instant.store(true);
  else
    _is_instant.store(instant);

  // modernize-core-layer-radical Task 11: pre-broadcast synchronously so
  // MainWindow can commit trigger settings + capture_init + on_state_changed
  // BEFORE exec_capture() starts the device. The legacy async
  // broadcast_sync<StartCollectWorkPrev> path replaces the old int-message
  // dispatch. Caller (start_capture) is on the main thread (user-initiated
  // action).
  _event_bus->broadcast_sync<interface::StartCollectWorkPrev>({});

  if (exec_capture()) {
    _work_time_id.fetch_add(1);
    // CaptureOwnerGuard manages _is_working + _capture_owner_document +
    // CaptureOwnerChanged broadcast as a single RAII unit. Replaces the
    // manual _is_working=true / _capture_owner_document=... pattern.
    _state->document_registry()->acquire_capture_owner(
        owner ? owner : _state->document_registry()->get_active_document());
    _event_bus->broadcast_async<interface::StartCollectWork>({});

    // Start a timer, for able to refresh the view per (1000 / 30)ms.
    if (is_realtime_refresh()) {
      _refresh_rt_timer.Start(1000 / 30);
    }

    return true;
  }

  return false;
}

bool CaptureManager::exec_capture() {
  if (_state->device_agent().is_collecting()) {
    pxv_err("Error!Device is running.");
    return false;
  }

  // copy_data_to_document is now zero-copy (shared_ptr sharing, instant).
  // No need to wait for a background copy thread — is_copy_in_progress()
  // always returns false. The old wait loop and will_swap_buffer calculation
  // have been removed.

  if (_state->device_agent().have_enabled_channel() == false) {
    QString err_str(L_S(STR_PAGE_MSG, S_ID(IDS_MSG_NO_ENABLED_CHANNEL),
                        "No channels enabled!"));
    MsgBox::Show(err_str);
    return false;
  }

  _capture_times.fetch_add(1);
  _coord->set_is_triged(false);

  int mode = _state->device_agent().get_work_mode();
  bool bAddDecoder = false;
  bool bSwapBuffer = false;

  if (mode == DSO || mode == ANALOG) {
    // reset measure of dso signal
    for (auto m : _state->signal_models()) {
      if (m->type() == SR_CHANNEL_DSO) {
        // TODO: verify - view::DsoSignal::set_mValid(false) was a UI method.
        // Validity reset should be handled by the View layer.
      }
    }
  } else {
    if (is_single_mode()) {
      if (_is_stream_mode)
        bAddDecoder = true;
      // Single capture must use SINGLE BUFFER (capture_data == view_data).
      // bSwapBuffer stays false here, so set_capture_data() is never called
      // below and capture_data would keep pointing at whatever back buffer a
      // previous repeat-mode run left behind (capture_data != view_data).
      // The driver then fills capture_data while view_data stays empty.
      // During the capture that still renders, because is_realtime_refresh()
      // is true for stream+single and get_logic_snapshot() returns
      // capture_data. But the moment the capture ends is_working() goes
      // false, is_realtime_refresh() returns false, and get_logic_snapshot()
      // falls back to view_data -> empty snapshot -> blank screen.
      // Pin capture_data to view_data so both phases read the same data.
      _coord->set_capture_data(_state->view_data());
    } else if (is_repeat_mode()) {
      if (_is_stream_mode) {
        if (_capture_times == 1)
          bAddDecoder = true;
        else
          bSwapBuffer = true;
      } else {
        // Non-stream repeat: use SINGLE BUFFER (capture_data == view_data).
        // DSView original design: data goes directly into view_data, so the
        // viewport can show live data during capture via get_logic_snapshot()
        // which returns view_data->get_logic(). Double buffer would put data
        // in capture_data while view_data stays empty → blank screen.
        // Zero-copy shared_ptr makes this safe: copy_data_to_document shares
        // the snapshot, and view_data->clear() on next capture creates a new
        // shared_ptr while the document keeps the old one alive.
        //
        // bSwapBuffer stays false, so set_capture_data() is not called below.
        // Pin capture_data to view_data explicitly, otherwise a back buffer
        // left over from an earlier double-buffered run would still be the
        // capture target and view_data would never receive data.
        _coord->set_capture_data(_state->view_data());
      }
    } else if (is_loop_mode()) {
    }
  }

  if (mode == LOGIC && _state->device_agent().is_hardware() &&
      _state->device_agent().get_hardware_operation_mode() == LO_OP_BUFFER) {
    _trig_check_timer.Start(200);
  }

  if (bAddDecoder) {
    _coord->clear_all_decode_task2();
    clear_decode_result();

    // CRITICAL: Release the active document's shared_ptr references to the old
    // snapshot data. copy_data_to_document() now shares the shared_ptr (zero-copy)
    // instead of deep-copying. If we don't reset the document's shared_ptrs here,
    // the old multi-GB mmap stays alive (ref count > 0) while a new one is
    // created, causing memory to double on every capture.
    // Note: doc->clear() resets the shared_ptrs (releasing the document's
    // reference). If SessionData still holds a shared_ptr to the same snapshot
    // (e.g. view_data), the data stays alive. We must NOT call
    // doc->get_active_logic()->clear() because that would clear the data
    // in-place, affecting SessionData's shared snapshot.
    if (_state->document_registry()->get_active_document()) {
      _state->document_registry()->get_active_document()->clear();
    }
  }

  // Set the buffer to store the captured data
  if (bSwapBuffer) {
    int buf_index = -1;
    for (int i = 0; i < (int)_state->data_list().size(); i++) {
      if (_state->data_list()[i].get() != _state->view_data()) {
        buf_index = i;
        break;
      }
    }

    if (buf_index < 0) {
      _state->data_list().push_back(std::make_unique<SessionData>());
      buf_index = (int)_state->data_list().size() - 1;
    }

    _coord->set_capture_data(_state->data_list()[buf_index].get());
    _state->capture_data()->clear();
    _coord->set_cur_snap_samplerate(_state->device_agent().get_sample_rate());
    _coord->set_cur_samplelimits(_state->device_agent().get_sample_limit());
  }

  capture_init();

  // IMPORTANT: Ensure the session's logic signals point to the current capture
  // buffer. This is required because DecoderStack searches the session's signal
  // list to find the data source. Without this, decoders in stream mode would
  // bind to the old, cleared document snapshot and fail to show results.
  _state->attach_data_to_signal(_state->capture_data());

  // 将应用层 _is_instant 同步到驱动 SR_CONF_INSTANT。
  // demo 驱动读取 devc->instant 决定 DSO 单帧/连续采集语义；其他驱动
  // 不支持此 key 时 set_config_bool 静默失败（pxv_dbg 日志），无副作用。
  // 旧注释"driver no longer reads instant mode"是错误的——demo 驱动确实读取。
    _state->device_agent().set_config_bool(SR_CONF_INSTANT, _is_instant.load());

  // Core→libsigrok 触发配置唯一同步点。在 ds_start_collect 前一次性同步，
  // 消除 TriggerDock/SessionService 各自调 ds_trigger_* 导致的互相覆盖。
  //
  // instant 模式下禁用所有触发（硬件+软件），让 driver 立即采集数据，
  // 恢复旧版 fork 的 instant 语义（"立即采集不等待触发"）。统一在
  // sync_trigger_to_libsigrok 入口处理，避免每个 driver 单独判断 instant。
  _coord->sync_trigger_to_libsigrok(_is_instant.load());

  if (_state->device_agent().start() == false) {
    pxv_err("Start collect error!");
    return false;
  }

  // CRITICAL FIX: fork 迁移遗漏 — 设置 device_status = ST_RUNNING。
  // 旧 fork libsigrok 在 ds_start_collect 内部会通过 sr_status 结构体设置
  // device_status；上游 libsigrok 0.6 无此机制，导致 _device_status 永远停在
  // ST_INIT（set_device 时设置），is_init_status() 恒为 true，
  // viewport_painter.cpp 的 doPaint 永远走 paintCursors 分支，从不调用
  // paintSignals，波形不渲染。
  _state->set_device_status(ST_RUNNING);

  if (mode == LOGIC) {
    for (auto de : _state->decode_traces()) {
      if (bAddDecoder) {
        // 彻底禁止 Stream 模式下“边采边解”（会导致严重的内存锁竞争和 GUI 渲染卡死）。
        // 采集结束时（收到 RevEndPacket 后），底层的 start_all_decode_tasks() 会自动启动离线解码。
        if (!_is_stream_mode.load()) {
          de->set_capture_end_flag(false);
          de->frame_ended();
          _state->add_decode_task(de);
        }
      }
    }
  }

  return true;
}

bool CaptureManager::stop_capture() {
  _is_action.store(true);
  int ret = action_stop_capture();
  _is_action.store(false);
  return ret;
}

bool CaptureManager::action_stop_capture() {
  if (!_state->is_working())
    return false;

  pxv_info("Stop collect.");

  if (_coord->bClose()) {
    _coord->set_is_working(false);
    _repeat_timer.Stop();
    _repeat_wait_prog_timer.Stop();
    _refresh_rt_timer.Stop();
    exit_capture();
    // Task 4: RAII cleanup — join copy thread + clear owner + broadcast.
    _state->document_registry()->release_capture_owner();
    return true;
  }

  bool wait_upload = false;
  if (is_single_mode() &&
      _state->device_agent().get_work_mode() == LOGIC) {
    _state->device_agent().get_config_bool(SR_CONF_WAIT_UPLOAD, wait_upload);
  }

  if (!wait_upload) {
    _coord->set_is_working(false);
    _repeat_timer.Stop();
    _repeat_wait_prog_timer.Stop();
    _refresh_rt_timer.Stop();

    if (_repeat_hold_prg.load() != 0 && is_repeat_mode()) {
      _repeat_hold_prg.store(0);
      _state->repeat_hold(0);
    }

    // modernize-core-layer-radical Task 11: pre-broadcast synchronously so
    // observers (SessionService) can emit "end_collect_prev" BEFORE
    // exit_capture() runs. The legacy async int-message path is removed.
    // Caller (stop_capture) is on the main thread.
    _event_bus->broadcast_sync<interface::EndCollectWorkPrev>({});

    exit_capture();

    // CRITICAL FIX: fork 迁移遗漏 — 手动停止采集时设置 device_status =
    // ST_STOPPED。SR_DF_END 路径只覆盖采集正常完成的情况；用户手动点击
    // 停止按钮走 action_stop_capture → exit_capture 路径，不会触发
    // SR_DF_END，因此需要在此显式设置。
    _state->set_device_status(ST_STOPPED);

    // CRITICAL FIX: 手动停止时也需要调用 frame_ended() 来触发
    // MainWindow::on_frame_ended()，后者会调用 update_toolbar_view_status()
    // 来更新按钮状态。正常采集结束时，LOGIC 模式通过
    // SigSession::on_event(RevEndPacket) → frame_ended() 调用，
    // 非 LOGIC 模式通过 DataFeedParser::SR_DF_END → _state->frame_ended() 调用。
    // 手动停止时这两个路径都不会触发，导致按钮保持禁用状态（无法添加解码器）。
    _state->frame_ended();

    data_unlock();

    if (is_repeat_mode() && _state->device_status() != ST_RUNNING) {
      _event_bus->broadcast_async<interface::EndCollectWork>({});
    }

    // Task 4: RAII cleanup — join copy thread + clear owner + _is_working=false
    // (redundant here, set above) + CaptureOwnerChanged broadcast. Replaces the
    // old manual `_capture_owner_document = nullptr` (gated on !_copy_in_progress)
    // — the guard always joins the copy thread first, which is safer.
    _state->document_registry()->release_capture_owner();

    // 架构修复: stop_capture 也广播 CaptureStateChanged，与 start_capture
    // (capturemanager.cpp:209) 对称。原来只在 start_capture 广播，导致
    // MainWindow::on_event(CaptureStateChanged) (mainwindow.cpp:3062-3065)
    // 在停止路径永远不被调用 —— 该统一处理器同时刷新 sidebar 按钮状态
    // (update_toolbar_view_status) 和 DeviceOptionsDock widget 状态
    // (_device_options_widget->update_widgets_status)。
    //
    // 在 single 模式手动停止时:
    //   - EndCollectWorkPrev 同步广播但 MainWindow 是 no-op
    //   - EndCollectWork 只在 repeat 模式广播 (line 496-498)
    //   - set_is_working(false) / set_device_status(ST_STOPPED) 是静默 setter
    // 所以 DeviceOptionsDock 的 widget 保持禁用(灰色无法点击)。
    //
    // 此广播同时也是 MCP/WS 客户端收到"停止"通知的统一路径
    // (SessionService::on_event(CaptureStateChanged) → broadcast_event)。
    _event_bus->broadcast_async<interface::CaptureStateChanged>(
        {_state->is_working(), _state->device_status()});
    return true;
  } else {
    pxv_info("Data is uploading from device data buffer, waiting for stop.");
  }
  return false;
}

void CaptureManager::exit_capture() {
  _is_instant.store(false);

  _feed_timer.Stop();

  if (_state->device_agent().is_collecting())
    _state->device_agent().stop();
}

bool CaptureManager::get_capture_status(bool &triggered, int &progress) {
  // Fork libsigrok exposed per-sample capture progress via the sr_status
  // struct + ds_get_actived_device_status(); both are gone after the
  // upstream migration. Upstream libsigrok does not expose progress, but
  // the data path already tracks how many samples have been fed in, so we
  // synthesize progress here from the Core trigger flag (is_triged, set by
  // DataFeedParser when the first data packet arrives) and the current
  // capture buffer's sample count vs cur_samplelimits(). Callers (sidebar
  // arc text in viewport_painter.cpp and MCP get_capture_status) gate on
  // the bool return and read the triggered/progress out-params, so we
  // always return true and let the out-params convey the state.
  triggered = _state->is_triged();

  const uint64_t sample_limits = _coord->cur_samplelimits();
  if (sample_limits == 0) {
    progress = 0;
    return true;
  }

  // Pick the active snapshot by work mode. MSO (Mixed Signal Oscilloscope)
  // = LOGIC + analog channels; the logic snapshot is the primary data
  // source, same merge as in SigSession::get_ring_sample_count().
  data::Snapshot *snapshot = nullptr;
  const int mode = _state->device_agent().get_work_mode();
  if (mode == LOGIC || mode == MSO) {
    snapshot = _state->capture_data()->get_logic();
  } else if (mode == DSO) {
    snapshot = _state->capture_data()->get_dso();
  } else {
    snapshot = _state->capture_data()->get_analog();
  }

  if (snapshot == nullptr) {
    progress = 0;
    return true;
  }

  const uint64_t sample_count = snapshot->get_sample_count();
  progress = (int)(sample_count * 100 / sample_limits);
  return true;
}

void CaptureManager::check_update() {
  // Use try_lock to avoid interlock with libsigrok's data_feed_in path.
  // paintEvent calls this on the GUI thread; if data_feed_in holds the lock
  // (processing a USB transfer), blocking here would stall GMainLoop's
  // libusb_handle_events_timeout(), causing empty_transfer_count to spike
  // and abort stream acquisition. Skipping a paint-time check is harmless —
  // the next timer tick will try again.
  std::unique_lock<std::mutex> lock(_state->data_mutex(), std::try_to_lock);
  if (!lock.owns_lock())
    return;

  if (_state->device_agent().is_collecting() == false)
    return;

  if (_data_updated.load()) {
    // DSO mode: skip data_updated() here — the async DataUpdated event
    // (broadcast_async from feed_in_dso) already drives ViewDataSync::
    // data_updated() on the GUI thread. Calling it again from paintEvent
    // creates a feedback loop: paint → check_update → data_updated →
    // update → paint, causing excessive repaints at 40+ FPS.
    if (_state->device_agent().get_work_mode() != LOGIC &&
        _state->device_agent().get_work_mode() != DSO)
      _coord->data_updated();

    _data_updated.store(false);
    _noData_cnt.store(0);
    data_auto_unlock();
  } else {
    if (++_noData_cnt >= (CaptureManager::WaitShowTime / CaptureManager::FeedInterval))
      nodata_timeout();
  }
}

void CaptureManager::nodata_timeout() {
  auto &agent = _state->device_agent();
  // SR_CONF_TRIGGER_SOURCE 仅 DSO 硬件设备支持；demo/file/compat 设备查询会
  // 产生 "Option not available" + ERROR 日志噪音。非 DSO 硬件直接显示等待触发。
  if (!agent.is_hardware_dso()) {
    _state->show_wait_trigger();
    return;
  }
  int flag = 0;
  agent.get_config_byte(SR_CONF_TRIGGER_SOURCE, flag);
  if (flag != DSO_TRIGGER_AUTO) {
    _state->show_wait_trigger();
  }
}

void CaptureManager::feed_timeout() {
  data_unlock();

  if (!_data_updated.load()) {
    if (++_noData_cnt >= (CaptureManager::WaitShowTime / CaptureManager::FeedInterval))
      nodata_timeout();
  }
}

int CaptureManager::get_repeat_hold() const {
  if (_state->is_working() && is_repeat_mode())
    return _repeat_hold_prg.load();
  else
    return 0;
}

void CaptureManager::auto_end() {
  // TODO: view::DsoSignal::auto_end() was a UI rendering method that adjusted
  // the auto-set state and refreshed the trace. After de-view-ization,
  // SigSession does not own view::Signal instances. The View layer is
  // responsible for calling auto_end() on its own cloned DsoSignal objects when
  // this event occurs (e.g. by listening to a broadcast message or callback).
}

void CaptureManager::set_collect_mode(DEVICE_COLLECT_MODE m) {
  assert(!_state->is_working());

  if (_clt_mode != m) {
    _clt_mode = m;
    _repeat_hold_prg.store(0);
  }

  _event_bus->broadcast_async<interface::CollectModeChanged>({});
}

void CaptureManager::repeat_capture_wait_timeout() {
  _repeat_timer.Stop();
  _repeat_wait_prog_timer.Stop();

  _repeat_hold_prg.store(0);

  if (_state->is_working()) {
    _state->repeat_hold(_repeat_hold_prg.load());
    exec_capture();
  }
}

void CaptureManager::repeat_wait_prog_timeout() {
  int val = _repeat_hold_prg.load() - _repeat_wait_prog_step.load();
  if (val < 0)
    val = 0;
  _repeat_hold_prg.store(val);

  if (_state->is_working())
    _state->repeat_hold(val);
}

void CaptureManager::realtime_refresh_timeout() { _rt_refresh_time_id.fetch_add(1); }

bool CaptureManager::have_new_realtime_refresh(bool keep) {
  uint64_t cur = _rt_refresh_time_id.load();
  if (_rt_ck_refresh_time_id.load() != cur) {
    if (!keep) {
      _rt_ck_refresh_time_id.store(cur);
    }
    return true;
  }
  return false;
}

void CaptureManager::clear_decode_result() {
  for (auto stack : _state->decode_traces()) {
    stack->init();
    stack->set_capture_end_flag(false);
  }
  _event_bus->broadcast_async<interface::ClearDecodeData>({});
}

bool CaptureManager::is_first_store_confirm() {
  int cur = _work_time_id.load();
  if (_confirm_store_time_id.load() != cur) {
    _confirm_store_time_id.store(cur);
    return true;
  }
  return false;
}

void CaptureManager::trig_check_timeout() {
  bool triged = false;
  int pro;

  if (_state->is_triged()) {
    _trig_check_timer.Stop();
    return;
  }

  if (get_capture_status(triged, pro) && triged) {
    _coord->set_trig_time(QDateTime::currentDateTime());
    _coord->set_is_triged(true);
    _trig_check_timer.Stop();
  }
}

void CaptureManager::refresh(int holdtime) {
  ds_lock_guard lock(_state->data_mutex());

  data_lock();
  _state->view_data()->get_logic()->init();

  _coord->clear_all_decode_task2();
  clear_decode_result();

  _state->view_data()->get_dso()->init();

  for (auto m : _state->spectrum_stacks()) {
    m->init();
  }

  if (_state->math_stack())
    _state->math_stack()->init();

  _state->view_data()->get_analog()->init();

  _out_timer.TimeOut(holdtime,
                     std::bind(&CaptureManager::feed_timeout, this));
  _data_updated.store(true);
}

void CaptureManager::data_auto_lock(int lock) { _data_auto_lock.store(lock); }

void CaptureManager::data_auto_unlock() {
  int val = _data_auto_lock.load();
  if (val > 0)
    _data_auto_lock.store(val - 1);
  else if (val < 0)
    _data_auto_lock.store(0);
}

bool CaptureManager::get_data_auto_lock() const { return _data_auto_lock.load() != 0; }

bool CaptureManager::is_realtime_refresh() const {
  // After stopping (is_working == false), there is no live capture to
  // refresh from. Returning false here ensures that get_signal_snapshot()
  // and get_*_snapshot() fall back to view_data (the last completed
  // capture's data) instead of capture_data (the back buffer, which was
  // cleared by the RevEndPacket handler on manual stop in repeat mode).
  // Without this check, stream-mode + repeat-mode stops would leave the
  // viewport blank because the snapshot getters returned the cleared
  // capture_data instead of view_data.
  if (!_state->is_working())
    return false;
  if (is_loop_mode())
    return true;
  if (_is_stream_mode.load() && is_single_mode())
    return true;
  if (_is_stream_mode.load() && is_repeat_mode())
    return true;
  return false;
}

bool CaptureManager::is_repeating() const {
  return _clt_mode == COLLECT_REPEAT && !_is_instant.load();
}

bool CaptureManager::is_single_mode() const { return _clt_mode == COLLECT_SINGLE; }

bool CaptureManager::is_repeat_mode() const { return _clt_mode == COLLECT_REPEAT; }

bool CaptureManager::is_loop_mode() const { return _clt_mode == COLLECT_LOOP; }

int CaptureManager::get_collect_mode() const { return (int)_clt_mode; }

} // namespace core
} // namespace pv
