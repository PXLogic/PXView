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

#define LAN_CN  25
#define LAN_TRADITIONAL  26
#define LAN_EN  31

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
