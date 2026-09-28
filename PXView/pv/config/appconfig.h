/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 * 
 * Copyright (C) 2021 DreamSourceLab <support@dreamsourcelab.com>
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

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <QString>
#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QTimer>
#include <QVariant>

// P2-A: Setting key constants — use these instead of bare string literals.
#include "pv/config/appconfig_keys.h"

inline constexpr int LAN_CN = 25;
inline constexpr int LAN_TRADITIONAL = 26;
inline constexpr int LAN_EN = 31;

#define THEME_STYLE_DARK   "dark"
#define THEME_STYLE_LIGHT  "light"

#define APP_NAME  "PXView"
  
//--------------------api---
QString GetIconPath();
QString GetAppDataDir();
QString GetFirmwareDir();
QString GetUserDataDir();
QString GetDecodeScriptDir();
QString GetProfileDir();

//------------------class
  
class StringPair
{
public:
   StringPair(const std::string &key, const std::string &value);
   std::string m_key;
   std::string m_value;
};


#define APP_CONFIG_VERSION  3
#define NO_POINT_VALUE  -10000

struct AppOptions
{   
    int   version;
    bool  quickScroll;
    bool  warnofMultiTrig;
    bool  originalData;
    bool  ableSaveLog;
    bool  appendLogMode;
    int   logLevel;
    bool  transDecoderDlg;
    bool  trigPosDisplayInMid;
    bool  displayProfileInBar;
    bool  swapBackBufferAlways;
    bool  autoScrollLatestData;
    bool  promptSaveOnExit;

    // 逻辑通道波形左端是否绘制 H/L 电平标签（ATK / Saleae Logic 2 风格）。
    // 应用级视图偏好，默认开启。关闭时栅格化输出与旧版本逐像素一致。
    bool  showLogicHlLabels = true;

    // 视图刷新帧率上限（fps）。决定主重绘合并定时器与缩放/平移动画、
    // 进度动画帧定时器的节奏。运行时可热更新（SettingChangeListener）。
    // 取值范围 [kMinViewFps, kMaxViewFps]，默认 kDefaultViewFps。
    int   viewMaxFps = 60;

    // 全局通道高度（视图密度）：用户 Ctrl+滚轮缩放后的值。0 = 从未设置过，
    // 此时回退主题「新通道默认高度」（AppConfig::logic_channel_default_height）。
    // 只在"用户显式缩放"时写入；workspace.json 的 session.uiLayout 是 per-tab
    // 的覆盖值，两者分工见 ViewLayout 构造函数与 TabManager::restore_workspace()。
    int   logicChannelHeightScale = 0;

    // TDM realtime decode and analog display-trigger UI persistence.
    bool  tdmRealtimeDecode = false;

    bool    analogDisplayTriggerTdmValid = false;
    bool    analogDisplayTriggerTdmEnable = false;
    QString analogDisplayTriggerTdmMode = "auto";
    int     analogDisplayTriggerTdmChannel = 0;
    QString analogDisplayTriggerTdmEdge = "rising";
    double  analogDisplayTriggerTdmLevel = 0.0;
    int     analogDisplayTriggerTdmPosition = 50;

    bool    analogDisplayTriggerPwmValid = false;
    bool    analogDisplayTriggerPwmEnable = false;
    QString analogDisplayTriggerPwmMode = "auto";
    int     analogDisplayTriggerPwmChannel = 0;
    QString analogDisplayTriggerPwmEdge = "rising";
    double  analogDisplayTriggerPwmLevel = 50.0;
    int     analogDisplayTriggerPwmPosition = 50;

    float fontSize;

    std::vector<StringPair> m_protocolFormats;
};
 
 // The dock pannel open status.
 struct DockOptions
 {
  bool        decodeDock;
  bool        triggerDock;
  bool        measureDock;
  bool        searchDock;
  bool        deviceOptionsDock;
  bool        logDock;
};

struct FrameOptions
{ 
  QString     style;
  int         language; 
  int         left; //frame region
  int         top;
  int         right;
  int         bottom;
  int         x;
  int         y;
  int         ox;
  int         oy;
  bool        isMax;
  QString     displayName;
  QByteArray  windowState;

  DockOptions   _logicDock;
  DockOptions   _analogDock;
  DockOptions   _dsoDock;
};

struct UserHistory
{ 
  QString   exportDir;
  QString   saveDir;
  bool      showDocuments;
  QString   screenShotPath;
  QString   sessionDir;
  QString   openDir;
  QString   protocolExportPath;
  QString   exportFormat;
};

struct FontParam
{
  QString   name;
  float     size;
};

struct FontOptions
{
  FontParam toolbar;
  FontParam channelLabel;
  FontParam channelBody;
  FontParam ruler;
  FontParam title;
  FontParam other;
};

// App-layer device settings that need to persist across application restarts.
// These are global preferences (not per-device), stored in QSettings "Device" group.
struct DeviceOptions
{
    double  streamMemBuff = 16.0;       // GB, in-memory ring buffer size
    double  streamBuff = 16.0;          // GB, disk cache total depth
    bool    diskCacheEnable = false;    // enable disk cache for stream mode
    QString diskCachePath;              // disk cache storage path
    QString lastDeviceDriver;           // driver name of last used device
    QString lastDeviceConnId;           // connection ID of last used device (stable across reboots)
    // 毛刺滤波面板配置持久化（跨会话默认值，per-channel 阈值随 .pxl 保存）
    bool    glitchAutoApply = false;    // 采集后自动重新应用滤波
    int     glitchDefaultThreshold = 3; // 默认滤波阈值（周期数）
    bool    glitchShowOverlay = true;   // 显示波形轨道红色滤波提示叠加层
};

struct ShortcutItem {
    int     actionId;
    QString keySequence;
};

struct ShortcutOptions {
    QList<ShortcutItem> items;
};

struct StyleTokenItem {
    QString tokenName;
    QString value;
};

struct StyleOptions {
    QList<StyleTokenItem> items;
};

// P2-A: Setting change listener interface. Any class that needs to react
// to setting changes can implement this and register via
// AppConfig::register_setting_listener().  Listeners are notified after
// the setting value has been written to QSettings.
class SettingChangeListener
{
public:
    virtual ~SettingChangeListener() = default;
    virtual void on_setting_changed(const QString &group,
                                    const QString &key,
                                    const QVariant &value) = 0;
};

class AppConfig
{
private:
  AppConfig();
  ~AppConfig();
  AppConfig(AppConfig &o);

public:
  static AppConfig &Instance();

  void LoadAll();
  void SaveApp();  
  void SaveHistory();
  void SaveFrame();
  void SaveShortcuts();
  void SaveStyle();
  void SaveDevice();

  void flushPendingSaves();
  
  void SetProtocolFormat(const std::string &protocolName, const std::string &value);
  std::string GetProtocolFormat(const std::string &protocolName); 

  inline bool IsLangCn()
  {
    return frameOptions.language == LAN_CN || frameOptions.language == LAN_TRADITIONAL;
  }

  static void GetFontSizeRange(float *minSize, float *maxSize);

  bool IsDarkStyle();

  QColor GetStyleColor();

  void SetThemeTokens(const QHash<QString, QString> &tokens);
  QColor GetThemeColor(const QString &tokenName) const;
  QString GetThemeTokenValue(const QString &tokenName) const;

  // 主题「通道高度」token 的唯一解析入口（原逻辑在 logicsignal.cpp 与
  // ViewSignalSync::UpdateTheme() 各解析一遍）。语义：新创建通道的默认高度。
  // 已有通道的高度由 per-channel own_height 与 pxc 恢复的 signalHeightScale
  // 决定，不由此值影响。token 缺失/非法时回退 kDefaultLogicChannelHeight。
  static constexpr int kDefaultLogicChannelHeight = 24;
  int logic_channel_default_height() const;

  // 视图刷新帧率上限（fps）的取值范围与默认值。
  static constexpr int kMinViewFps = 15;
  static constexpr int kMaxViewFps = 144;
  static constexpr int kDefaultViewFps = 60;

  // 将 appOptions.viewMaxFps 换算为定时器间隔（ms）：clamp 到
  // [kMinViewFps, kMaxViewFps]，返回值恒 ≥ 1。View / Viewport 的绘制节奏
  // 定时器（重绘合并、缩放/平移动画、进度动画）统一取此值。
  int view_frame_interval_ms() const;

  // P2-A: Setting change listener registration.
  void register_setting_listener(SettingChangeListener *listener);
  void unregister_setting_listener(SettingChangeListener *listener);
  // P2-A: Called by setFiled() after a setting value is written to QSettings.
  // Notifies all registered listeners. Declared public so static helpers
  // in appconfig.cpp can call it.
  void notify_setting_changed(const QString &group,
                              const QString &key,
                              const QVariant &value);

  // limit_samples 应用层 fallback：当驱动返回 0（上游约定"不限制"）时使用此默认值
  // 默认 1000000 = SR_MHZ(1)（1M 采样点）
  uint64_t default_sample_limit() const { return default_sample_limit_; }
  void set_default_sample_limit(uint64_t v) { default_sample_limit_ = v; }

  // App-layer device settings (stream buffer sizes, disk cache, last device)
  DeviceOptions deviceOptions;

public:
  AppOptions    appOptions;
  UserHistory   userHistory;
  FrameOptions  frameOptions;
  ShortcutOptions  shortcutOptions;
  StyleOptions     styleOptions;

private:
  QHash<QString, QString> _themeTokens;

  // P2-A: Registered setting change listeners.
  std::vector<SettingChangeListener*> _setting_listeners;

  uint64_t default_sample_limit_ = 1000000ULL;  // SR_MHZ(1)

  QTimer *_saveFrameTimer;
  QTimer *_saveAppTimer;
  QTimer *_saveHistoryTimer;
  QTimer *_saveShortcutsTimer;
  QTimer *_saveStyleTimer;
  QTimer *_saveDeviceTimer;

  void doSaveFrame();
  void doSaveApp();
  void doSaveHistory();
  void doSaveShortcuts();
  void doSaveStyle();
  void doSaveDevice();
};
