/*
 * This file is part of the PXView project.
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/data/model/signalconfigstore.h"
#include "pv/data/idevice_config_port.h"
#include "pv/base/log.h"
#include "pv/base/pxvdef.h"
#include "pv/data/model/signalmodel.h"
#include <QDebug>
#include <QJsonArray>
#include <libsigrok/libsigrok.h>

namespace pv {
namespace data {

SignalConfigStore::SignalConfigStore(IDeviceConfigPort *device_port)
    : _device_port(device_port) {}

SignalConfigStore::~SignalConfigStore() {}

QJsonObject SignalConfigStore::signal_config_to_json() const {
  QJsonObject obj;
  obj["work_mode"] = _signal_config.work_mode;
  obj["operation_mode"] = _signal_config.operation_mode;
  obj["channel_mode"] = _signal_config.channel_mode;
  obj["is_demo"] = _signal_config.is_demo;
  obj["demo_operation_mode"] = _signal_config.demo_operation_mode;

  QJsonArray ch_array;
  for (const auto &ch : _signal_config.channels) {
    QJsonObject ch_obj;
    ch_obj["index"] = ch.index;
    ch_obj["enabled"] = ch.enabled;
    ch_obj["vdiv"] = (qint64)ch.vdiv;
    ch_obj["coupling"] = ch.coupling;
    ch_obj["map_default"] = ch.map_default;
    ch_obj["hw_offset"] = ch.hw_offset;
    ch_obj["offset"] = ch.offset;
    ch_obj["zero_offset"] = ch.zero_offset;
    ch_obj["trig_type"] = ch.trig_type;
    ch_obj["view_index"] = ch.view_index;
    ch_obj["v_offset"] = ch.v_offset;
    ch_obj["own_height"] = ch.own_height;
    // Task 3: 原 MainWindow::gen_config_json 直访 view::Signal 写入的字段，
    // 统一使用 ChannelConfig 字段名（不保留 strigger/trigValue/zeroPos/
    // mapUnit/mapMin/mapMax/mapDefault/colour/type/name/vfactor 旧 key）。
    ch_obj["type"] = ch.type;
    ch_obj["name"] = QString::fromStdString(ch.name);
    ch_obj["colour"] = QString::fromStdString(ch.colour);
    ch_obj["vfactor"] = (qint64)ch.vfactor;
    ch_obj["trig_value"] = (int)ch.trig_value;
    ch_obj["map_unit"] = QString::fromStdString(ch.map_unit);
    ch_obj["map_min"] = ch.map_min;
    ch_obj["map_max"] = ch.map_max;
    ch_array.append(ch_obj);
  }
  obj["channels"] = ch_array;

  return obj;
}

void SignalConfigStore::signal_config_from_json(const QJsonObject &obj) {
  _signal_config.work_mode = obj["work_mode"].toInt();
  _signal_config.operation_mode = obj["operation_mode"].toString();
  _signal_config.channel_mode = obj["channel_mode"].toString();
  _signal_config.is_demo = obj["is_demo"].toBool();
  _signal_config.demo_operation_mode = obj["demo_operation_mode"].toString();

  _signal_config.channels.clear();
  if (obj.contains("channels")) {
    QJsonArray ch_array = obj["channels"].toArray();
    for (const auto &ch_val : ch_array) {
      QJsonObject ch_obj = ch_val.toObject();
      ChannelConfig cfg;
      cfg.index = ch_obj["index"].toInt();
      cfg.enabled = ch_obj["enabled"].toBool();
      cfg.vdiv = (uint64_t)ch_obj["vdiv"].toVariant().toULongLong();
      cfg.coupling = ch_obj["coupling"].toInt();
      cfg.map_default = ch_obj["map_default"].toBool();
      // Task 6: hw_offset/offset/zero_offset 补齐 contains() 保护，与其他字段风格一致。
      cfg.hw_offset = (uint16_t)(ch_obj.contains("hw_offset")
                                     ? ch_obj["hw_offset"].toInt()
                                     : 0);
      cfg.offset = (uint16_t)(ch_obj.contains("offset")
                                  ? ch_obj["offset"].toInt()
                                  : 0);
      cfg.zero_offset = (uint16_t)(ch_obj.contains("zero_offset")
                                       ? ch_obj["zero_offset"].toInt()
                                       : 0);
      cfg.trig_type =
          ch_obj.contains("trig_type") ? ch_obj["trig_type"].toInt() : 0;
      cfg.view_index =
          ch_obj.contains("view_index") ? ch_obj["view_index"].toInt() : -1;
      cfg.v_offset =
          ch_obj.contains("v_offset") ? ch_obj["v_offset"].toInt() : 0;
      cfg.own_height =
          ch_obj.contains("own_height") ? ch_obj["own_height"].toInt() : -1;
      // Task 3: 读取原 MainWindow 路径补齐的字段。
      cfg.type = ch_obj.contains("type") ? ch_obj["type"].toInt() : 0;
      cfg.name = ch_obj.contains("name") ? ch_obj["name"].toString().toStdString()
                                         : std::string();
      cfg.colour = ch_obj.contains("colour")
                       ? ch_obj["colour"].toString().toStdString()
                       : std::string();
      // Guard: vfactor=0 is invalid (causes assertion failure in
      // dslDial::set_factor). Old config files saved in LA mode may
      // have vfactor=0 for DSO channels. Clamp to 1 (x1 probe default).
      cfg.vfactor = ch_obj.contains("vfactor")
                        ? (uint64_t)ch_obj["vfactor"].toVariant().toULongLong()
                        : 0;
      if (cfg.vfactor == 0) {
        pxv_warn("SignalConfigStore: ch[%d] vfactor==0 in JSON, clamping to 1",
                 cfg.index);
        cfg.vfactor = 1;
      }
      cfg.trig_value = ch_obj.contains("trig_value")
                           ? (uint8_t)ch_obj["trig_value"].toInt()
                           : 0;
      cfg.map_unit = ch_obj.contains("map_unit")
                         ? ch_obj["map_unit"].toString().toStdString()
                         : std::string();
      cfg.map_min =
          ch_obj.contains("map_min") ? ch_obj["map_min"].toDouble() : 0.0;
      cfg.map_max =
          ch_obj.contains("map_max") ? ch_obj["map_max"].toDouble() : 0.0;
      _signal_config.channels.push_back(cfg);
    }
  }

  _signal_config.is_valid = true;
}

void SignalConfigStore::save_signal_config(
    const std::vector<std::shared_ptr<SignalModel>> &signal_models,
    const std::map<int, ChannelLayoutState> &channel_layout,
    const std::map<int, std::string> &channel_colours) {
  IDeviceConfigPort *agent = _device_port;
  if (!agent || !agent->have_instance()) {
    return;
  }

  _signal_config.work_mode = agent->get_work_mode();

  /* Task 10/Phase 3: read operation_mode/channel_mode as strings (driver
   * config_get now returns g_variant_new_string). */
  agent->get_config_string(SR_CONF_OPERATION_MODE, _signal_config.operation_mode);
  agent->get_config_string(SR_CONF_CHANNEL_MODE, _signal_config.channel_mode);

  _signal_config.is_demo = agent->is_demo();

  if (_signal_config.is_demo)
    _signal_config.demo_operation_mode = agent->get_demo_operation_mode();

  // 保存旧配置的布局信息，用于为不在 channel_layout 中的通道（被禁用且
  // 已从 View 的 _own_signals 中移除的通道）保留其 v_offset / own_height。
  // view_index 不继承（写 -1），由 normalize_view_indices() 按类型 + index
  // 统一赋值，防止过期 view_index 跨模式传播导致通道交错排序。
  std::map<int, ChannelConfig> old_channels;
  for (const auto &ch : _signal_config.channels)
    old_channels[ch.index] = ch;

  _signal_config.channels.clear();
  int mode = _signal_config.work_mode;
  for (const GSList *l = agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;
    ChannelConfig cfg;
    cfg.index = (int)probe->index;
    cfg.enabled = probe->enabled;
    cfg.vdiv = 0;
    cfg.coupling = 0;
    cfg.map_default = true;

    // Task 3: 通道元数据（所有模式）。type/name 直接读 sr_channel。
    cfg.type = probe->type;
    cfg.name = probe->name ? probe->name : "";

    // 查找当前通道对应的 SignalModel（fork sr_channel 扩展字段已不在
    // upstream libsigrok 的 sr_channel 中，改从 SignalModel 读取）。
    std::shared_ptr<SignalModel> matched_model;
    for (auto m : signal_models) {
      if (m && m->index() == cfg.index) {
        matched_model = m;
        break;
      }
    }

    // ROOT FIX: Use channel TYPE (not work mode) to decide whether to save
    // DSO/ANALOG fields. The device may have DSO channels even in LOGIC mode
    // (e.g. DSLogic/DSCope have both LOGIC + DSO channels). The old code
    // checked `mode == ANALOG || mode == DSO`, so in LA mode DSO channels
    // had vfactor=0 (the struct default), which caused assertion failures
    // when the file was loaded later (dslDial::set_factor assert(factor > 0)).
    if (probe->type == SR_CHANNEL_DSO ||
        probe->type == SR_CHANNEL_ANALOG) {
      // SR_CONF_PROBE_VDIV / SR_CONF_PROBE_COUPLING fork DSO keys deleted;
      // cfg.vdiv / cfg.coupling keep their defaults (0). map_default is still
      // queried (key retained in pxvdef.h, migrated in Phase 2).
      bool map_default = true;
      agent->get_config_bool(SR_CONF_PROBE_MAP_DEFAULT, map_default, probe,
                             nullptr);
      cfg.map_default = map_default;

      // Fork sr_channel fields (hw_offset/offset/zero_offset/vfactor) removed
      // in upstream libsigrok. Read from SignalModel instead — model state
      // is the single source of truth (set_* methods sync to driver via
      // set_config_*).
      cfg.hw_offset = matched_model ? (uint16_t)matched_model->hw_offset() : 0;
      cfg.offset = matched_model ? (uint16_t)matched_model->vertical_offset() : 0;
      cfg.zero_offset = matched_model ? (uint16_t)matched_model->zero_offset() : 0;
      // Task 3: vfactor (DSO/ANALOG，原 MainWindow 路径 B 写入)。
      // Clamp to at least 1 — vfactor=0 is invalid (causes assertion failure
      // in dslDial::set_factor). When the current work mode is LOGIC, DSO
      // SignalModels may not have vfactor set, so the model value could be 0.
      uint64_t vf = matched_model ? (uint64_t)matched_model->vfactor() : 0;
      if (vf == 0) {
        pxv_warn("SignalConfigStore::save: ch[%d] vfactor==0 from model, clamping to 1",
                 cfg.index);
        vf = 1;
      }
      cfg.vfactor = vf;
    }

    // Task 3: DSO 触发电平原始值 (原 MainWindow 路径 trigValue)。
    if (mode == DSO) {
      cfg.trig_value = matched_model ? (uint8_t)matched_model->trig_value() : 0;
    }

    // Task 3: Analog 映射参数 (原 MainWindow 路径 mapUnit/mapMin/mapMax)。
    // Fork sr_channel.map_unit/min/max fields removed in upstream
    // libsigrok. SignalModel does not carry these (Analog mapping UI is
    // being deprecated) — stub to defaults.
    if (mode == ANALOG) {
      cfg.map_unit = "";
      cfg.map_min = 0.0;
      cfg.map_max = 0.0;
    }

    // R2: 保存 Logic 通道触发类型 (trig_type 存于 SignalModel，不在 sr_channel
    // 中)
    if (mode == LOGIC && matched_model) {
      cfg.trig_type = matched_model->trig_type();
    }

    // Task 3: 信号颜色（View 概念，过渡存放）。优先用 View 传入的 channel_colours
    // （与原 MainWindow 路径 B 一致，从 view::Signal::get_colour() 采集）；
    // 回退到 SignalModel::color()；再回退到 "default"。
    auto col_it = channel_colours.find(cfg.index);
    if (col_it != channel_colours.end()) {
      cfg.colour = col_it->second;
    } else if (matched_model && !matched_model->color().empty()) {
      cfg.colour = matched_model->color();
    } else {
      cfg.colour = "default";
    }

    // UI 布局状态：从 channel_layout 按 index 匹配写入；map 中无此 index
    // 时不从旧配置继承 view_index（防止过期值跨模式传播），只继承
    // v_offset / own_height（不影响排序）。view_index 写 -1，由
    // normalize_view_indices() 按类型 + index 赋正确默认值。
    auto layout_it = channel_layout.find(cfg.index);
    if (layout_it != channel_layout.end()) {
      cfg.view_index = layout_it->second.view_index;
      cfg.v_offset = layout_it->second.v_offset;
      cfg.own_height = layout_it->second.own_height;
    } else {
      auto old_it = old_channels.find(cfg.index);
      if (old_it != old_channels.end()) {
        // 只继承 v_offset / own_height，不继承 view_index
        cfg.view_index = -1;
        cfg.v_offset = old_it->second.v_offset;
        cfg.own_height = old_it->second.own_height;
      }
    }

    _signal_config.channels.push_back(cfg);
  }

  _signal_config.is_valid = true;
}

void SignalConfigStore::apply_signal_config() {
  IDeviceConfigPort *agent = _device_port;
  if (!agent || !agent->have_instance() || !_signal_config.is_valid) {
    return;
  }

  int cur_mode = agent->get_work_mode();
  if (_signal_config.work_mode != cur_mode) {
    // set_work_mode() handles both DSL/PXLogic (driver-side SR_CONF_DEVICE_MODE)
    // and demo/file/compat (app-layer cache). No need to guard with is_dsl_device().
    agent->set_work_mode(_signal_config.work_mode);
  }

  /* Task 10/Phase 3: write operation_mode/channel_mode as strings (driver
   * config_set uses std_str_idx). Skip empty strings — a .pxc saved from a
   * device that doesn't support these keys stores "", and passing "" to the
   * driver's std_str_idx validation returns SR_ERR_ARG (logged as a warning
   * by DeviceAgent::set_config). */
  if (!_signal_config.operation_mode.isEmpty())
    agent->set_config_string(SR_CONF_OPERATION_MODE,
                            _signal_config.operation_mode.toUtf8().constData());
  if (!_signal_config.channel_mode.isEmpty())
    agent->set_config_string(SR_CONF_CHANNEL_MODE,
                            _signal_config.channel_mode.toUtf8().constData());

  if (_signal_config.is_demo && !_signal_config.demo_operation_mode.isEmpty()) {
    agent->set_config_string(
        SR_CONF_PATTERN_MODE,
        _signal_config.demo_operation_mode.toLocal8Bit().data());
  }

  int mode = _signal_config.work_mode;
  for (const GSList *l = agent->get_channels(); l; l = l->next) {
    sr_channel *const probe = (sr_channel *)l->data;
    if (!probe)
      continue;
    // Task 3: 按 index 匹配 ChannelConfig（替代原 positional 匹配，更稳健；
    // 同设备的 sr_channel 顺序与 save 时一致，index 匹配等价且对顺序变化容错）。
    const ChannelConfig *cfg_ptr = nullptr;
    for (const auto &c : _signal_config.channels) {
      if (c.index == (int)probe->index) {
        cfg_ptr = &c;
        break;
      }
    }
    if (!cfg_ptr) {
      continue;
    }
    const ChannelConfig &cfg = *cfg_ptr;

    // Only restore the saved enabled state for channels whose type matches
    // the current work mode. Channels of a different type (e.g. logic channels
    // when in DSO mode) must remain disabled — otherwise demo_prepare_data()
    // detects has_enabled_other and skips the SR_DF_DSO path, leaving the DSO
    // view empty. This mirrors switch_work_mode()'s channel-type filtering.
    bool should_enable = cfg.enabled;
    if (mode == LOGIC && probe->type != SR_CHANNEL_LOGIC)
      should_enable = false;
    else if (mode == DSO && probe->type != SR_CHANNEL_DSO)
      should_enable = false;
    else if (mode == ANALOG && probe->type != SR_CHANNEL_ANALOG)
      should_enable = false;
    else if (mode == MSO && probe->type != SR_CHANNEL_LOGIC &&
             probe->type != SR_CHANNEL_ANALOG)
      should_enable = false;

    agent->enable_probe(probe, should_enable);

    // Task 3: 通道名（所有模式，原 MainWindow 路径 B 写 probe->name）。
    if (!cfg.name.empty()) {
      probe->name = g_strdup(cfg.name.c_str());
    }

    if (mode == ANALOG || mode == DSO) {
      // SR_CONF_PROBE_VDIV / SR_CONF_PROBE_COUPLING fork DSO keys deleted;
      // only map_default is restored (key retained in pxvdef.h).
      agent->set_config_bool(SR_CONF_PROBE_MAP_DEFAULT, cfg.map_default,
                             probe, nullptr);
      // Fork sr_channel fields (hw_offset/offset/zero_offset/vfactor) are not
      // present on upstream libsigrok's sr_channel. The set_config_* calls
      // above sync the driver state; the SignalModel-side state is restored
      // separately by SigSession when it rebuilds SignalModels from the
      // loaded config (see SigSession::load_config or equivalent).
    }

    // Task 3: DSO 触发电平原始值 (原 MainWindow 路径 B 写 probe->trig_value)。
    // Fork sr_channel.trig_value field removed in upstream libsigrok —
    // trigger value is restored to SignalModel via set_trig_value() by the
    // session restore path.
    if (mode == DSO) {
      // no-op: probe->trig_value write removed
    }

    // Task 3: Analog 映射参数 (原 MainWindow 路径 B 写 probe->map_unit/min/max)。
    // Fork sr_channel.map_unit/min/max fields removed in upstream
    // libsigrok — Analog mapping UI is being deprecated, stub to defaults.
    if (mode == ANALOG) {
      // no-op: probe->map_unit/min/max writes removed
    }
  }

  // Safety net: ensure at least one channel of the current work mode's type
  // remains enabled. The saved .pxc may carry stale enabled=false for all
  // channels of the active mode (e.g. demo2.pxc saved with all ANALOG
  // channels disabled). Without this, SigSession::reload() finds no enabled
  // channel of the active mode → "Unable to create any channel" →
  // clear_signals() → viewport disappears. switch_work_mode() had correctly
  // enabled the mode's channels, but apply_signal_config() then overwrote
  // them with the stale saved state. Force-enable the first channel of the
  // active mode type if none survived.
  {
    sr_channel *first_mode_ch = nullptr;
    bool any_mode_enabled = false;
    for (const GSList *l = agent->get_channels(); l; l = l->next) {
      sr_channel *const probe = (sr_channel *)l->data;
      if (!probe)
        continue;
      bool is_mode_type = false;
      switch (mode) {
      case LOGIC:  is_mode_type = (probe->type == SR_CHANNEL_LOGIC);  break;
      case DSO:    is_mode_type = (probe->type == SR_CHANNEL_DSO);    break;
      case ANALOG: is_mode_type = (probe->type == SR_CHANNEL_ANALOG); break;
      case MSO:    is_mode_type = (probe->type == SR_CHANNEL_LOGIC ||
                                   probe->type == SR_CHANNEL_ANALOG); break;
      default: break;
      }
      if (!is_mode_type)
        continue;
      if (!first_mode_ch)
        first_mode_ch = probe;
      if (probe->enabled) {
        any_mode_enabled = true;
        break;
      }
    }
    if (!any_mode_enabled && first_mode_ch) {
      agent->enable_probe(first_mode_ch, true);
      pxv_warn("apply_signal_config: all %d-mode channels were disabled "
               "in .pxc; force-enabling ch[%d] '%s' to avoid empty viewport",
               mode, first_mode_ch->index,
               first_mode_ch->name ? first_mode_ch->name : "(nullptr)");
    }
  }
}

void SignalConfigStore::apply_pending_config() {
  if (_pending_device_config.is_valid) {
    _signal_config = _pending_device_config;
    apply_signal_config();
    _pending_device_config = SignalConfig();
  }
}

bool SignalConfigStore::has_signal_config() const {
  return _signal_config.is_valid;
}

bool SignalConfigStore::has_pending_config() const {
  return _pending_device_config.is_valid;
}

} // namespace data
} // namespace pv
