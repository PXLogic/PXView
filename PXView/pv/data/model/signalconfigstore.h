/*
 * This file is part of the PXView project.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef PXVIEW_PV_DATA_SIGNALCONFIGSTORE_H
#define PXVIEW_PV_DATA_SIGNALCONFIGSTORE_H

#include <QJsonObject>
#include <QString>
#include <map>
#include <memory>
#include <cstdint>
#include <vector>

class DeviceAgent;

namespace pv {

class SigSession;

namespace data {

class SignalModel;

// NOTE: view_index/v_offset/own_height are UI layout fields that conceptually
// belong to the View layer. They remain in ChannelConfig for .pxc serialization
// compatibility. Full migration to pv::view::DockUiState is deferred pending
// .pxc format extension (see fix-remaining-architecture-issues spec C3).
struct ChannelLayoutState {
  int view_index;
  int v_offset;
  int own_height;
  ChannelLayoutState() : view_index(-1), v_offset(0), own_height(-1) {}
};

struct ChannelConfig {
  int index;
  bool enabled;
  uint64_t vdiv;
  int coupling;
  bool map_default;
  uint16_t hw_offset;
  uint16_t offset;
  uint16_t zero_offset;
  int trig_type;  // R2: Logic 通道触发类型 (SignalModel::LogicTrigType)，仅
                  // LOGIC 模式有意义
  int view_index; // UI 布局：通道在视图中的顺序，-1
                  // 表示未设置（按启用顺序派生）
  int v_offset;   // UI 布局：垂直偏移
  int own_height; // UI 布局：轨道高度，-1 表示自动高度
  // purify-architecture-concepts Task 3: 补齐原 MainWindow::gen_config_json
  // 直访 view::Signal 写入的字段，使 SignalConfigStore 成为 .pxc channel 配置
  // 唯一序列化路径。下述字段统一使用 ChannelConfig 字段名，不再保留 MainWindow
  // 路径的 JSON key（strigger/trigValue/zeroPos/mapUnit/mapMin/mapMax/mapDefault
  // /colour/view_index/type/name/vfactor）。
  int type;            // 通道类型 (SR_CHANNEL_LOGIC/DSO/ANALOG)，元数据
  std::string name;    // 通道名（元数据）
  std::string colour;  // 信号颜色（View 概念，过渡存放，阶段 6 迁移到 uiLayout）
  uint64_t vfactor;    // 电压因子 (DSO/ANALOG)
  uint8_t trig_value;  // DSO 触发电平原始值 (probe->trig_value)
  std::string map_unit; // Analog 映射单位
  double map_min;      // Analog 映射最小值
  double map_max;      // Analog 映射最大值

  ChannelConfig()
      : index(0), enabled(false), vdiv(0), coupling(0),
        map_default(true), hw_offset(0), offset(0), zero_offset(0),
        trig_type(0), view_index(-1), v_offset(0), own_height(-1), type(0),
        vfactor(0), trig_value(0), map_min(0.0), map_max(0.0) {}
};

struct SignalConfig {
  int work_mode;
  /* Task 10/Phase 3: operation_mode/channel_mode migrated from int to QString
   * to match the driver's string-based config_get/config_set. The driver
   * returns the mode string (e.g. "Buffer Mode", "Use 32 Channels (Max 250MHz)")
   * via g_variant_new_string; this is stored verbatim and written back via
   * set_config_string. */
  QString operation_mode;
  QString channel_mode;
  bool is_demo;
  QString demo_operation_mode;
  std::vector<ChannelConfig> channels;
  bool is_valid;

  SignalConfig()
      : work_mode(0), operation_mode(), channel_mode(), is_demo(false),
        is_valid(false) {}
};

// SignalConfigStore: owns signal/pending device config and handles
// serialization + DeviceAgent sync. Decoupled from SessionDocument so that
// SessionDocument no longer needs to depend on DeviceAgent directly.
// The DeviceAgent is obtained internally via the injected SigSession.
class SignalConfigStore {
public:
  SignalConfigStore(SigSession *session);
  ~SignalConfigStore();

  // Serialize signal config (work_mode/channels/...) to JSON.
  // NOTE: triggerConfig is NOT included here; SessionDocument wraps this
  // and adds triggerConfig from its own TriggerConfig member to keep .pxc
  // format unchanged.
  QJsonObject signal_config_to_json() const;
  void signal_config_from_json(const QJsonObject &obj);

  void save_signal_config(
      const std::vector<std::shared_ptr<SignalModel>> &signal_models = {},
      const std::map<int, ChannelLayoutState> &channel_layout = {},
      const std::map<int, std::string> &channel_colours = {});
  void apply_signal_config();
  void apply_pending_config();
  bool has_signal_config() const;
  bool has_pending_config() const;

  const SignalConfig &get_signal_config() const { return _signal_config; }
  SignalConfig &get_signal_config() { return _signal_config; }
  void set_pending_config(const SignalConfig &cfg) {
    _pending_device_config = cfg;
  }

  // For TabContext to iterate channels and restore trig_type after reload.
  const std::vector<ChannelConfig> &get_channels() const {
    return _signal_config.channels;
  }

private:
  SigSession *_session;
  SignalConfig _signal_config;
  SignalConfig _pending_device_config;
};

} // namespace data
} // namespace pv

#endif // PXVIEW_PV_DATA_SIGNALCONFIGSTORE_H
