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

#include "pv/mainwindow/config_io.h"

#include "pv/mainwindow/mainwindow.h"
#include "pv/mainwindow/dock_manager.h"

#include "pv/data/stack/decoderstack.h"
#include "pv/data/document/sessiondocument.h"
#include "pv/dock/protocoldock.h"
#include "pv/dock/triggerdock.h"
#include "pv/dock/dsotriggerdock.h"
#include "pv/base/log.h"
#include "pv/session/sigsession.h"
#include "pv/session/storesession.h"
#include "pv/session/tabcontext.h"
#include "pv/toolbars/filebar.h"
#include "pv/toolbars/samplingbar.h"
#include "pv/ui/langresource.h"
#include "pv/ui/msgbox.h"
#include "pv/utility/encoding.h"
#include "pv/utility/path.h"
#include "pv/view/signal/analogsignal.h"
#include "pv/view/signal/dsosignal.h"
#include "pv/view/signal/logicsignal.h"
#include "pv/view/signal/signal.h"
#include "pv/view/view.h"
#include "pv/view/component/viewstatus.h"
#include "pv/base/ZipMaker.h"
#include "pv/config/appconfig.h"
#include "pv/session/deviceagent.h"
#include "pv/base/pxvdef.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMessageBox>
#include <QString>
#include <QTextStream>
#include <glib.h>
#include <cassert>
#include <string>

namespace {

/** Build a channel-index → ChannelLayoutState map from the View's signal list.
 * Duplicated from mainwindow.cpp's anonymous namespace (small helper, not
 * worth a shared header). */
std::map<int, pv::data::ChannelLayoutState>
build_channel_layout(pv::view::View *view) {
  std::map<int, pv::data::ChannelLayoutState> layout;
  if (view) {
    for (auto &sig : view->get_own_signals()) {
      pv::data::ChannelLayoutState s;
      s.view_index = sig->get_view_index();
      s.v_offset = sig->get_v_offset();
      s.own_height = sig->get_own_height();
      layout[sig->get_index()] = s;
    }
  }
  return layout;
}

/** Build a channel-index → colour-string map from the View's signal list. */
std::map<int, std::string>
build_channel_colours(pv::view::View *view) {
  std::map<int, std::string> colours;
  if (view) {
    for (auto &sig : view->get_own_signals()) {
      QColor c = sig->get_colour();
      colours[sig->get_index()] = c.isValid() ? c.name().toStdString() : "default";
    }
  }
  return colours;
}

} // namespace

namespace pv {

// ===========================================================================
// Config file path
// ===========================================================================

QString MainWindowConfigIO::gen_config_file_path(bool isNewFormat) {
  AppConfig &app = AppConfig::Instance();

  QString file = GetProfileDir();
  QDir dir(file);
  if (dir.exists() == false) {
    dir.mkpath(file);
  }

  QString driver_name = _wnd->device_agent()->driver_name();
  QString mode_name = QString::number(_wnd->device_agent()->get_work_mode());
  QString lang_name;
  QString base_path = dir.absolutePath() + "/" + driver_name + mode_name;

  if (!isNewFormat) {
    lang_name = QString::number(app.frameOptions.language);
  }

  return base_path + ".ses" + lang_name + ".pxc";
}

// ===========================================================================
// Save (serialize to JSON)
// ===========================================================================

void MainWindowConfigIO::save_config() {
  pxv_info("save_config: ENTER, have_instance=%d, is_hardware=%d",
           _wnd->device_agent()->have_instance(),
           _wnd->device_agent()->is_hardware());
  if (_wnd->device_agent()->have_instance() == false) {
    pxv_info("There is no need to save the configuration");
    return;
  }

  AppConfig &app = AppConfig::Instance();

  // Always persist the last-used device driver name so the next launch
  // can prefer this device. Without this, switching to demo and exiting
  // would leave lastDeviceDriver stale (still pointing to the hardware
  // device), causing the app to jump back to hardware on restart.
  app.deviceOptions.lastDeviceDriver = _wnd->device_agent()->driver_name();
  app.SaveDevice();

  if (_wnd->device_agent()->is_hardware() && !_wnd->device_agent()->is_demo()) {
    // Persist connection ID for hardware devices to distinguish multiple
    // devices of the same model.
    struct sr_dev_inst *sdi = _wnd->device_agent()->inst();
    if (sdi) {
      const char *cid = sr_dev_inst_connid_get(sdi);
      if (cid)
        app.deviceOptions.lastDeviceConnId = QString::fromLocal8Bit(cid);
    }

    QString sessionFile = gen_config_file_path(true);
    save_config_to_file(sessionFile);
  } else if (_wnd->device_agent()->is_demo()) {
    // Demo device: save channel/trigger/decoder config to its own .pxc file
    // so demo setups (channel enable, trigger, decoders) persist across restarts.
    QDir dir(GetFirmwareDir());
    if (dir.exists()) {
      QString ses_name = dir.absolutePath() + "/" +
                         _wnd->device_agent()->driver_name() +
                         QString::number(_wnd->device_agent()->get_work_mode()) +
                         ".pxc";
      save_config_to_file(ses_name);
    }
  }

  app.frameOptions.windowState = _wnd->saveState();
  app.SaveFrame();
}

bool MainWindowConfigIO::gen_config_json(QJsonObject &sessionVar) {
  AppConfig &app = AppConfig::Instance();

  QString title = QApplication::applicationName() + " v" +
                  QApplication::applicationVersion();

  sessionVar["Version"] = QJsonValue::fromVariant(SESSION_FORMAT_VERSION);
  sessionVar["Device"] = QJsonValue::fromVariant(_wnd->device_agent()->driver_name());
  sessionVar["DeviceMode"] =
      QJsonValue::fromVariant(_wnd->device_agent()->get_work_mode());
  sessionVar["Language"] = QJsonValue::fromVariant(app.frameOptions.language);
  sessionVar["Title"] = QJsonValue::fromVariant(title);

  if (_wnd->device_agent()->is_hardware() && _wnd->device_agent()->get_work_mode() == LOGIC) {
    sessionVar["CollectMode"] = _wnd->session()->get_collect_mode();
  }

  // --- Device instance session config (sample rate, limit_samples, operation_mode, etc.) ---
  GVariant *gvar_opts =
      _wnd->device_agent()->get_config_list(nullptr, SR_CONF_DEVICE_SESSIONS);
  GVariant *gvar;
  gsize num_opts;

  pxv_info("gen_config_json: querying SR_CONF_DEVICE_SESSIONS, gvar_opts=%p", gvar_opts);

  if (gvar_opts != nullptr) {
    const int *const options = (const int32_t *)g_variant_get_fixed_array(
        gvar_opts, &num_opts, sizeof(int32_t));

    for (unsigned int i = 0; i < num_opts; i++) {
      const struct sr_config_info *const info =
          _wnd->device_agent()->get_config_info(options[i]);
      if (!info || !info->name)
        continue;
      gvar = _wnd->device_agent()->get_config(info->key);
      if (gvar != nullptr) {
        if (info->datatype == SR_T_BOOL)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_boolean(gvar));
        else if (info->datatype == SR_T_UINT64)
          sessionVar[info->name] = QJsonValue::fromVariant(
              QString::number(g_variant_get_uint64(gvar)));
        else if (info->datatype == SR_T_UINT8)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_byte(gvar));
        else if (info->datatype == SR_T_INT16)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_int16(gvar));
        else if (info->datatype == SR_T_FLOAT)
          sessionVar[info->name] = QJsonValue::fromVariant(
              QString::number(g_variant_get_double(gvar)));
        else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_string(gvar, nullptr));
        else if (info->datatype == SR_T_INT32)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_int32(gvar));
        else if (info->datatype == SR_T_UINT32)
          sessionVar[info->name] =
              QJsonValue::fromVariant((uint32_t)g_variant_get_uint32(gvar));
        else if (info->datatype == SR_T_LIST)
          sessionVar[info->name] =
              QJsonValue::fromVariant(g_variant_get_int16(gvar));
        else {
          pxv_err("Unknown config info type:%d", info->datatype);
        }
        g_variant_unref(gvar);
      }
    }
    g_variant_unref(gvar_opts);
  } else if (_wnd->device_agent()->is_hardware()) {
    pxv_info("gen_config_json: falling back to SR_CONF_DEVICE_OPTIONS");
    gvar_opts = _wnd->device_agent()->get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS);

    if (!gvar_opts) {
      pxv_warn("No SR_CONF_DEVICE_OPTIONS available, skipping per-device config section.");
    } else {
      const uint32_t *const options = (const uint32_t *)g_variant_get_fixed_array(
          gvar_opts, &num_opts, sizeof(uint32_t));

      for (unsigned int i = 0; i < num_opts; i++) {
        const int key = (int)(options[i] & 0x1fffffff);

        const struct sr_config_info *const info =
            _wnd->device_agent()->get_config_info(key);
        if (!info || !info->name)
          continue;

        gvar = _wnd->device_agent()->get_config(info->key);
        if (gvar != nullptr) {
          if (info->datatype == SR_T_BOOL)
            sessionVar[info->name] =
                QJsonValue::fromVariant(g_variant_get_boolean(gvar));
          else if (info->datatype == SR_T_UINT64)
            sessionVar[info->name] = QJsonValue::fromVariant(
                QString::number(g_variant_get_uint64(gvar)));
          else if (info->datatype == SR_T_UINT8)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_byte(gvar));
          else if (info->datatype == SR_T_INT16)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_int16(gvar));
          else if (info->datatype == SR_T_FLOAT)
            sessionVar[info->name] = QJsonValue::fromVariant(
                QString::number(g_variant_get_double(gvar)));
          else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING)
            sessionVar[info->name] =
                QJsonValue::fromVariant(g_variant_get_string(gvar, nullptr));
          else if (info->datatype == SR_T_INT32)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_int32(gvar));
          else if (info->datatype == SR_T_UINT32)
            sessionVar[info->name] = QJsonValue::fromVariant((uint32_t)g_variant_get_uint32(gvar));
          else if (info->datatype == SR_T_LIST)
            sessionVar[info->name] = QJsonValue::fromVariant(g_variant_get_int16(gvar));
          else {
            pxv_err("Unknown config info type:%d", info->datatype);
          }
          g_variant_unref(gvar);
        }
      }
      g_variant_unref(gvar_opts);
    }
  }

  // Task 3: channel serialization via SignalConfigStore (single path).
  pv::TabContext *ctx = _wnd->current_context();
  pv::data::SessionDocument *doc = ctx ? ctx->document() : nullptr;
  if (doc) {
    doc->save_signal_config(_wnd->session()->get_signal_models_snapshot(),
                            build_channel_layout(_wnd->current_view()),
                            build_channel_colours(_wnd->current_view()));
    QJsonObject sig_cfg = doc->signal_config_to_json();
    sessionVar["channel"] = sig_cfg["channels"].toArray();
  } else {
    pxv_warn("MainWindowConfigIO::gen_config_json: no active document, writing empty "
             "channel array");
    sessionVar["channel"] = QJsonArray();
  }

  if (_wnd->device_agent()->get_work_mode() == LOGIC) {
    sessionVar["trigger"] = _wnd->session()->trigger_config().to_json();
  }

  // Glitch filter config persistence.
  if (_wnd->session()->is_glitch_filter_active() ||
      _wnd->session()->glitch_filter_auto_apply() ||
      !_wnd->session()->glitch_filter_thresholds().empty()) {
    QJsonObject glitchObj;
    glitchObj["auto_apply"] = _wnd->session()->glitch_filter_auto_apply();
    glitchObj["show_overlay"] = _wnd->session()->show_glitch_filter_overlay();
    glitchObj["active"] = _wnd->session()->is_glitch_filter_active();
    QJsonArray thrArray;
    QJsonArray modeArray;
    const auto &thresholds = _wnd->session()->glitch_filter_thresholds();
    const auto &modes = _wnd->session()->glitch_filter_modes();
    for (const auto &kv : thresholds) {
      QJsonObject entry;
      entry["ch"] = kv.first;
      entry["threshold"] = (int)kv.second;
      thrArray.append(entry);
    }
    for (const auto &kv : modes) {
      QJsonObject entry;
      entry["ch"] = kv.first;
      entry["mode"] = (int)kv.second;
      modeArray.append(entry);
    }
    glitchObj["thresholds"] = thrArray;
    glitchObj["modes"] = modeArray;
    sessionVar["glitch_filter"] = glitchObj;
  }

  StoreSession ss(_wnd->session());
  QJsonArray decodeJson;
  ss.gen_decoders_json(decodeJson);
  sessionVar["decoder"] = decodeJson;

  if (_wnd->device_agent()->get_work_mode() == DSO) {
    sessionVar["measure"] = _wnd->current_view()->get_viewstatus()->get_session();
  }

  return true;
}

bool MainWindowConfigIO::save_config_to_file(QString name) {
  if (name == "") {
    pxv_err("Session file name is empty.");
    return false;
  }

  std::string file_name = pv::path::ToUnicodePath(name);
  pxv_info("Store session to file: \"%s\"", file_name.c_str());

  QFile sf(name);
  if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
    pxv_warn("Warning: Couldn't open profile to write!");
    return false;
  }

  QTextStream outStream(&sf);
  encoding::set_utf8(outStream);

  QJsonObject sessionVar;
  if (!gen_config_json(sessionVar)) {
    return false;
  }

  QJsonDocument sessionDoc(sessionVar);
  outStream << QString::fromUtf8(sessionDoc.toJson());
  sf.close();
  return true;
}

bool MainWindowConfigIO::genSessionData(std::string &str) {
  QJsonObject sessionVar;
  if (!gen_config_json(sessionVar)) {
    return false;
  }

  QJsonDocument sessionDoc(sessionVar);
  // 按长度写入原始 UTF-8 字节，避免 C 字符串转换在 NUL 处截断 JSON，
  // 否则 "session" 入口内容不完整会导致加载时解码器等配置无法恢复。
  QByteArray ba = sessionDoc.toJson();
  str.append(ba.constData(), ba.size());
  return true;
}

// ===========================================================================
// Load (deserialize from JSON)
// ===========================================================================

bool MainWindowConfigIO::load_config_from_file(QString file) {
  if (file == "") {
    pxv_err("File name is empty.");
    return false;
  }

  _wnd->dock_manager()->protocol_widget()->del_all_protocol();

  std::string file_name = pv::path::ToUnicodePath(file);
  pxv_info("Load device profile: \"%s\"", file_name.c_str());

  QFile sf(file);

  if (!sf.exists()) {
    pxv_warn("Warning: device profile is not exists: \"%s\"",
             file_name.c_str());
    return false;
  }

  if (!sf.open(QIODevice::ReadOnly)) {
    pxv_warn("Warning: Couldn't open device profile to load!");
    return false;
  }

  QString data = QString::fromUtf8(sf.readAll());
  QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8());
  sf.close();

  bool bDecoder = false;
  int ret = load_config_from_json(doc, bDecoder);

  if (ret && _wnd->device_agent()->get_work_mode() == DSO) {
    _wnd->dock_manager()->dso_trigger_widget()->update_view();
  }

  if (_wnd->device_agent()->is_hardware()) {
    _wnd->title_ext_string() = file;
    _wnd->update_title_bar_text();
  }

  return ret;
}

bool MainWindowConfigIO::load_config_from_json(QJsonDocument &doc, bool &haveDecoder) {
  haveDecoder = false;

  QJsonObject sessionObj = doc.object();

  int mode = _wnd->device_agent()->get_work_mode();

  // check config file version
  if (!sessionObj.contains("Version")) {
    pxv_dbg("Profile version is not exists!");
    return false;
  }

  int format_ver = sessionObj["Version"].toInt();

  if (format_ver < 2) {
    pxv_err("Profile version is error!");
    return false;
  }

  if (sessionObj.contains("CollectMode") && _wnd->device_agent()->is_hardware()) {
    int collect_mode = sessionObj["CollectMode"].toInt();
    _wnd->session()->set_collect_mode((DEVICE_COLLECT_MODE)collect_mode);
  }

  int conf_dev_mode = sessionObj["DeviceMode"].toInt();

  if (_wnd->device_agent()->is_hardware()) {
    QString driverName = _wnd->device_agent()->driver_name();
    QString sessionDevice = sessionObj["Device"].toString();
    // check device and mode
    if (driverName != sessionDevice || mode != conf_dev_mode) {
      MsgBox::Show(
          nullptr,
          L_S(STR_PAGE_MSG, S_ID(IDS_MSG_PROFILE_NOT_COMPATIBLE),
              "Profile is not compatible with current device or mode!"),
          _wnd);
      return false;
    }
  }

  // load device settings
  GVariant *gvar_opts =
      _wnd->device_agent()->get_config_list(nullptr, SR_CONF_DEVICE_SESSIONS);
  gsize num_opts;

  if (gvar_opts != nullptr) {
    const int *const options = (const int32_t *)g_variant_get_fixed_array(
        gvar_opts, &num_opts, sizeof(int32_t));

    for (unsigned int i = 0; i < num_opts; i++) {
      const int key = options[i];
      const struct sr_config_info *info =
          _wnd->device_agent()->get_config_info(key);

      if (!info || !info->name)
        continue;

      if (!sessionObj.contains(info->name))
        continue;

      GVariant *gvar = nullptr;
      int id = 0;

      if (info->datatype == SR_T_BOOL) {
        gvar = g_variant_new_boolean(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_UINT64) {
        gvar = g_variant_new_uint64(
            sessionObj[info->name].toString().toULongLong());
      } else if (info->datatype == SR_T_UINT8) {
        if (sessionObj[info->name].toString() != "")
          gvar = g_variant_new_byte(sessionObj[info->name].toString().toUInt());
        else
          gvar = g_variant_new_byte(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_INT16) {
        gvar = g_variant_new_int16(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_FLOAT) {
        if (sessionObj[info->name].toString() != "")
          gvar = g_variant_new_double(
              sessionObj[info->name].toString().toDouble());
        else
          gvar = g_variant_new_double(sessionObj[info->name].toDouble());
      } else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING) {
        gvar = g_variant_new_string(
            sessionObj[info->name].toString().toLocal8Bit().data());
      } else if (info->datatype == SR_T_INT32) {
        gvar = g_variant_new_int32(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_UINT32) {
        gvar = g_variant_new_uint32(sessionObj[info->name].toInt());
      } else if (info->datatype == SR_T_LIST) {
        id = 0;

        if (format_ver > 2) {
          id = sessionObj[info->name].toInt();
        } else {
          const char *fd_key =
              sessionObj[info->name].toString().toLocal8Bit().data();
          id = _wnd->device_agent()->option_value_to_code(conf_dev_mode, info->key, fd_key);
          if (id == -1) {
            pxv_err("Convert failed, key:\"%s\", value:\"%s\"", info->name,
                    fd_key);
            id = 0;
          } else {
            pxv_info("Convert success, key:\"%s\", value:\"%s\", get code:%d",
                     info->name, fd_key, id);
          }
        }
        gvar = g_variant_new_int16(id);
      }

      if (gvar == nullptr) {
        pxv_warn("Warning: Profile failed to parse key:'%s'", info->name);
        continue;
      }

      bool bFlag = _wnd->device_agent()->set_config(info->key, gvar);
      if (!bFlag) {
        pxv_dbg("load_config: key '%s' (id=%d) rejected SET, skipping",
                info->name, info->key);
      }
    }
    g_variant_unref(gvar_opts);
  } else if (_wnd->device_agent()->is_hardware()) {
    gvar_opts = _wnd->device_agent()->get_config_list(nullptr, SR_CONF_DEVICE_OPTIONS);

    if (!gvar_opts) {
      pxv_warn("No SR_CONF_DEVICE_OPTIONS available, skipping per-device config load.");
    } else {
      const uint32_t *const options = (const uint32_t *)g_variant_get_fixed_array(
          gvar_opts, &num_opts, sizeof(uint32_t));

      for (unsigned int i = 0; i < num_opts; i++) {
        const int key = (int)(options[i] & 0x1fffffff);
        if (!(options[i] & SR_CONF_SET))
          continue;
        const struct sr_config_info *info =
            _wnd->device_agent()->get_config_info(key);

        if (!info || !info->name)
          continue;

        if (!sessionObj.contains(info->name))
          continue;

        GVariant *gvar = nullptr;
        int id = 0;

        if (info->datatype == SR_T_BOOL) {
          gvar = g_variant_new_boolean(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_UINT64) {
          gvar = g_variant_new_uint64(
              sessionObj[info->name].toString().toULongLong());
        } else if (info->datatype == SR_T_UINT8) {
          if (sessionObj[info->name].toString() != "")
            gvar = g_variant_new_byte(sessionObj[info->name].toString().toUInt());
          else
            gvar = g_variant_new_byte(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_INT16) {
          gvar = g_variant_new_int16(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_FLOAT) {
          if (sessionObj[info->name].toString() != "")
            gvar = g_variant_new_double(
                sessionObj[info->name].toString().toDouble());
          else
            gvar = g_variant_new_double(sessionObj[info->name].toDouble());
        } else if (info->datatype == SR_T_CHAR || info->datatype == SR_T_STRING) {
          gvar = g_variant_new_string(
              sessionObj[info->name].toString().toLocal8Bit().data());
        } else if (info->datatype == SR_T_INT32) {
          gvar = g_variant_new_int32(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_UINT32) {
          gvar = g_variant_new_uint32(sessionObj[info->name].toInt());
        } else if (info->datatype == SR_T_LIST) {
          id = 0;

          if (format_ver > 2) {
            id = sessionObj[info->name].toInt();
          } else {
            const char *fd_key =
                sessionObj[info->name].toString().toLocal8Bit().data();
            id = _wnd->device_agent()->option_value_to_code(conf_dev_mode, info->key, fd_key);
            if (id == -1) {
              pxv_err("Convert failed, key:\"%s\", value:\"%s\"", info->name,
                      fd_key);
              id = 0;
            } else {
              pxv_info("Convert success, key:\"%s\", value:\"%s\", get code:%d",
                       info->name, fd_key, id);
            }
          }
          gvar = g_variant_new_int16(id);
        }

        if (gvar == nullptr) {
          pxv_warn("Warning: Profile failed to parse key:'%s'", info->name);
          continue;
        }

        bool bFlag = _wnd->device_agent()->set_config(info->key, gvar);
        if (!bFlag) {
          pxv_err("Set device config option failed, id:%d, code:%d", info->key,
                  id);
        }
      }
      g_variant_unref(gvar_opts);
    }
  }

  // load channel settings via SignalConfigStore
  if (sessionObj.contains("channel")) {
    pv::TabContext *ctx = _wnd->current_context();
    pv::data::SessionDocument *doc = ctx ? ctx->document() : nullptr;
    if (doc) {
      QJsonObject sig_obj;
      sig_obj["channels"] = sessionObj["channel"].toArray();
      doc->signal_config_from_json(sig_obj);
      auto &cfg = doc->signal_config_store()->get_signal_config();
      cfg.work_mode = _wnd->device_agent()->get_work_mode();
      if (_wnd->device_agent()->is_dsl_device()) {
        _wnd->device_agent()->get_config_string(SR_CONF_OPERATION_MODE, cfg.operation_mode);
        _wnd->device_agent()->get_config_string(SR_CONF_CHANNEL_MODE, cfg.channel_mode);
      }
      cfg.is_demo = _wnd->device_agent()->is_demo();
      doc->apply_signal_config();
    } else {
      pxv_warn("MainWindowConfigIO::load_config_from_json: no active document, "
               "skipping channel apply");
    }
  }

  _wnd->session()->reload();

  // Glitch filter config restore
  if (sessionObj.contains("glitch_filter")) {
    QJsonObject glitchObj = sessionObj["glitch_filter"].toObject();
    _wnd->session()->set_glitch_filter_auto_apply(glitchObj["auto_apply"].toBool(false));
    _wnd->session()->set_show_glitch_filter_overlay(glitchObj["show_overlay"].toBool(true));

    if (glitchObj["active"].toBool(false)) {
      std::map<int, uint32_t> thresholds;
      std::map<int, GlitchFilterMode> modes;
      QJsonArray thrArray = glitchObj["thresholds"].toArray();
      QJsonArray modeArray = glitchObj["modes"].toArray();
      for (const QJsonValue &v : thrArray) {
        QJsonObject e = v.toObject();
        thresholds[e["ch"].toInt()] = (uint32_t)e["threshold"].toInt();
      }
      for (const QJsonValue &v : modeArray) {
        QJsonObject e = v.toObject();
        modes[e["ch"].toInt()] = (GlitchFilterMode)e["mode"].toInt();
      }
      _wnd->session()->restore_glitch_filter_config(thresholds, modes);
    }
  }

  // load signal setting (view-layer colour/trigger/ratio)
  if (mode == DSO) {
    for (auto &s : _wnd->current_view()->get_own_signals()) {
      for (const QJsonValue &value : sessionObj["channel"].toArray()) {
        QJsonObject obj = value.toObject();

        if (s->get_name() == obj["name"].toString() &&
            s->get_type() == obj["type"].toDouble()) {
          QString colourStr = obj["colour"].toString();
          if (colourStr != "default")
            s->set_colour(QColor(colourStr));

          if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = (view::DsoSignal *)s.get();
            dsoSig->load_settings();
            double zr = obj["zero_offset"].toDouble();
            if (zr > 0.0 && zr < 1.0)
              dsoSig->set_zero_ratio(zr);
            double tr = obj["trig_value"].toDouble();
            if (tr > 0.0 && tr < 1.0)
              dsoSig->set_trig_ratio(tr);
            dsoSig->commit_settings();
          }
          break;
        }
      }
    }
  } else {
    for (auto &s : _wnd->current_view()->get_own_signals()) {
      for (const QJsonValue &value : sessionObj["channel"].toArray()) {
        QJsonObject obj = value.toObject();
        if ((s->get_index() == obj["index"].toInt()) &&
            (s->get_type() == obj["type"].toInt())) {
          QString chan_name = obj["name"].toString().trimmed();
          if (chan_name == "") {
            chan_name = QString::number(s->get_index());
          }

          QString colourStr = obj["colour"].toString();
          if (colourStr != "default")
            s->set_colour(QColor(colourStr));
          s->set_name(chan_name);

          view::LogicSignal *logicSig = nullptr;
          logicSig = dynamic_cast<view::LogicSignal *>(s.get());
          if (logicSig) {
            logicSig->set_trig(obj["trig_type"].toInt());
          }

          if (s->signal_type() == SR_CHANNEL_DSO) {
            view::DsoSignal *dsoSig = dynamic_cast<view::DsoSignal *>(s.get());
            dsoSig->load_settings();
            double zr = obj["zero_offset"].toDouble();
            if (zr > 0.0 && zr < 1.0)
              dsoSig->set_zero_ratio(zr);
            double tr = obj["trig_value"].toDouble();
            if (tr > 0.0 && tr < 1.0)
              dsoSig->set_trig_ratio(tr);
            dsoSig->commit_settings();
          }

          if (s->signal_type() == SR_CHANNEL_ANALOG) {
            view::AnalogSignal *analogSig = dynamic_cast<view::AnalogSignal *>(s.get());
            double zv = obj["zero_offset"].toDouble();
            double ratio_z;
            if (zv > 0.0 && zv < 1.0) {
              ratio_z = zv;
            } else if (zv == 0.0) {
              ratio_z = 0.5;
            } else {
              ratio_z = analogSig->value2ratio((int)zv);
            }
            analogSig->set_zero_ratio(ratio_z);
            analogSig->commit_settings();
          }

          break;
        }
      }
    }
  }

  // update UI settings
  _wnd->sampling_bar()->update_sample_rate_list();
  _wnd->dock_manager()->trigger_widget()->device_updated();
  _wnd->current_view()->header_updated();

  // load trigger settings
  if (sessionObj.contains("trigger")) {
    _wnd->session()->set_trigger_config(
        data::TriggerConfig::from_json(sessionObj["trigger"].toObject()));
    _wnd->dock_manager()->trigger_widget()->refresh_ui_from_core();
  }

  // load decoders
  if (sessionObj.contains("decoder")) {
    QJsonArray deArray = sessionObj["decoder"].toArray();
    if (deArray.empty() == false) {
      haveDecoder = true;
      StoreSession ss(_wnd->session());
      ss.load_decoders(_wnd->dock_manager()->protocol_widget(), deArray);
      _wnd->current_view()->update_all_trace_postion();
    }
  }

  // load measure
  if (sessionObj.contains("measure")) {
    auto *bottom_bar = _wnd->current_view()->get_viewstatus();
    bottom_bar->load_session(sessionObj["measure"].toArray(), format_ver);
  }

  return true;
}

void MainWindowConfigIO::load_device_config() {
  _wnd->title_ext_string() = "";
  int mode = _wnd->device_agent()->get_work_mode();
  QString file;

  if (_wnd->device_agent()->is_hardware() && !_wnd->device_agent()->is_demo()) {
    QString ses_name = gen_config_file_path(true);

    bool bExist = false;

    QFile sf(ses_name);
    if (!sf.exists()) {
      pxv_info("Try to load the low version profile.");
      ses_name = gen_config_file_path(false);
    } else {
      bExist = true;
    }

    if (!bExist) {
      QFile sf2(ses_name);
      if (!sf2.exists()) {
        pxv_info("Try to load the default profile.");
        ses_name = _wnd->file_bar()->genDefaultSessionFile();
      }
    }

    file = ses_name;
  } else if (_wnd->device_agent()->is_demo()) {
    QDir dir(GetFirmwareDir());
    if (dir.exists()) {
      QString ses_name = dir.absolutePath() + "/" +
                         _wnd->device_agent()->driver_name() + QString::number(mode) +
                         ".pxc";

      QFile sf(ses_name);
      if (sf.exists()) {
        file = ses_name;
      }
    }
  }

  if (file != "") {
    bool ret = load_config_from_file(file);
    if (ret && _wnd->device_agent()->is_hardware()) {
      _wnd->title_ext_string() = file;
    }
  }
}

void MainWindowConfigIO::check_config_file_version() {
  auto device_agent = _wnd->session()->get_device();
  if (device_agent->is_file() && device_agent->is_new_device()) {
    if (device_agent->get_work_mode() == LOGIC) {
      int version = -1;
      if (device_agent->get_config_int16(SR_CONF_FILE_VERSION, version)) {
        if (version == 1) {
          QString strMsg(
              L_S(STR_PAGE_DLG, S_ID(IDS_DLG_CHECK_SESSION_FILE_VERSION_ERROR),
                  "Current loading file has an old format. \nThis will lead to "
                  "a slow loading speed. \nPlease resave it after loaded."));
          MsgBox::Show(strMsg);
        }
      }
    }
  }
}

// ===========================================================================
// Data file embedded config
// ===========================================================================

QJsonDocument MainWindowConfigIO::get_config_json_from_data_file(QString file,
                                                                  bool &bSucesss) {
  QJsonDocument sessionDoc;
  QJsonParseError error;
  bSucesss = false;

  if (file == "") {
    pxv_err("File name is empty.");
    return sessionDoc;
  }

  auto f_name = pv::path::ConvertPath(file);
  ZipReader rd(f_name.c_str());
  auto *data = rd.GetInnterFileData("session");

  if (data != nullptr) {
    // 按长度拷贝原始字节，避免 QString(const char*) 在 NUL 处截断，
    // 导致 "session" 入口 JSON 解析失败（解码器等配置无法恢复）。
    QByteArray qbs = QByteArray(data->data(), (int)data->size());
    sessionDoc = QJsonDocument::fromJson(qbs, &error);

    if (error.error != QJsonParseError::NoError) {
      QString estr = error.errorString();
      pxv_err("File::get_session(), parse json error:\"%s\"!",
              estr.toUtf8().data());
    } else {
      bSucesss = true;
    }

    rd.ReleaseInnerFileData(data);
  }

  return sessionDoc;
}

QJsonArray MainWindowConfigIO::get_decoder_json_from_data_file(QString file,
                                                                bool &bSucesss) {
  QJsonArray dec_array;
  QJsonParseError error;

  bSucesss = false;

  if (file == "") {
    pxv_err("File name is empty.");
    return QJsonArray();
  }

  auto f_name = path::ConvertPath(file);
  ZipReader rd(f_name.c_str());
  auto *data = rd.GetInnterFileData("decoders");

  if (data != nullptr) {
    // 按长度拷贝原始字节，避免 QString(const char*) 在 NUL 处截断，
    // 导致 "decoders" 入口 JSON 解析失败（解码器设置无法恢复）。
    QByteArray qbs = QByteArray(data->data(), (int)data->size());
    QJsonDocument sessionDoc = QJsonDocument::fromJson(qbs, &error);

    if (error.error != QJsonParseError::NoError) {
      QString estr = error.errorString();
      pxv_err(
          "MainWindowConfigIO::get_decoder_json_from_file(), parse json error:\"%s\"!",
          estr.toUtf8().data());
    } else {
      bSucesss = true;
    }

    dec_array = sessionDoc.array();
    rd.ReleaseInnerFileData(data);
  }

  return dec_array;
}

// ===========================================================================
// Demo decoder config
// ===========================================================================

void MainWindowConfigIO::load_demo_decoder_config(QString optname) {
  QString file = GetAppDataDir() + "/demo/logic/" + optname + ".demo";
  bool bLoadSurccess = false;

  QJsonArray deArray = get_decoder_json_from_data_file(file, bLoadSurccess);

  if (bLoadSurccess) {
    StoreSession ss(_wnd->session());
    ss.load_decoders(_wnd->dock_manager()->protocol_widget(), deArray);
  }

  _wnd->current_view()->update_all_trace_postion();
}

} // namespace pv
