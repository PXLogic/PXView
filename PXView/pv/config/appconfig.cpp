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

#include "pv/config/appconfig.h" 
#include <QApplication>
#include <QSettings>
#include <QLocale>
#include <QDir> 
#include <cassert>
#include <algorithm>
#include <QStandardPaths>
#include "pv/base/log.h"

// P2-A: Bring setting key constants into scope.
// 'using namespace pv::config' makes keys::App::foo resolve to pv::config::keys::App::foo.
using namespace pv::config;
  
#define MAX_PROTOCOL_FORMAT_LIST 15

StringPair::StringPair(const std::string &key, const std::string &value)
{
    m_key = key;
    m_value = value;
}

//------------function
static QString FormatArrayToString(std::vector<StringPair> &protocolFormats)
{
    QString str;

    for (StringPair &o : protocolFormats){
         if (!str.isEmpty()){
             str += ";";
         } 
         str += o.m_key.c_str();
         str += "=";
         str += o.m_value.c_str(); 
    }

    return str;
}

static void StringToFormatArray(const QString &str, std::vector<StringPair> &protocolFormats)
{
    QStringList arr = str.split(";");
    for (int i=0; i<arr.size(); i++){
        QString line = arr[i];
        if (!line.isEmpty()){
            QStringList vs = line.split("=");
            if (vs.size() == 2){
                protocolFormats.push_back(StringPair(vs[0].toStdString(), vs[1].toStdString()));
            }
        }
    }
}

//----------------read write field
static void getFiled(const char *key, QSettings &st, QString &f, const char *dv)
{
    f = st.value(key, dv).toString();
}

static void setFiled(const char *key, QSettings &st, QString f)
{
    st.setValue(key, f);
    AppConfig::Instance().notify_setting_changed(st.group(), key, f);
}

static void getFiled(const char *key, QSettings &st, int &f, int dv)
{
    f = st.value(key, dv).toInt();
}

static void setFiled(const char *key, QSettings &st, int f)
{
    st.setValue(key, f);
    AppConfig::Instance().notify_setting_changed(st.group(), key, f);
}

static void getFiled(const char *key, QSettings &st, bool &f, bool dv)
{
    f = st.value(key, dv).toBool();
}

static void setFiled(const char *key, QSettings &st, bool f){
    st.setValue(key, f);
    AppConfig::Instance().notify_setting_changed(st.group(), key, f);
}

static void getFiled(const char *key, QSettings &st, float &f, float dv)
{
    f = st.value(key, dv).toInt();
}

static void setFiled(const char *key, QSettings &st, float f)
{
    st.setValue(key, f);
    AppConfig::Instance().notify_setting_changed(st.group(), key, f);
}

static void getFiled(const char *key, QSettings &st, double &f, double dv)
{
    f = st.value(key, dv).toDouble();
}

static void setFiled(const char *key, QSettings &st, double f)
{
    st.setValue(key, f);
    AppConfig::Instance().notify_setting_changed(st.group(), key, f);
}

///------ app
static void _loadApp(AppOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::Application.toUtf8().constData()); 
    getFiled(keys::App::quickScroll.toUtf8().constData(), st, o.quickScroll, true);
    getFiled(keys::App::warnofMultiTrig.toUtf8().constData(), st, o.warnofMultiTrig, true);
    getFiled(keys::App::originalData.toUtf8().constData(), st, o.originalData, false);
    getFiled(keys::App::ableSaveLog.toUtf8().constData(), st, o.ableSaveLog, false);
    getFiled(keys::App::appendLogMode.toUtf8().constData(), st, o.appendLogMode, false);
    getFiled(keys::App::logLevel.toUtf8().constData(), st, o.logLevel, 3);
    getFiled(keys::App::transDecoderDlg.toUtf8().constData(), st, o.transDecoderDlg, true);
    getFiled(keys::App::trigPosDisplayInMid.toUtf8().constData(), st, o.trigPosDisplayInMid, true);
    getFiled(keys::App::displayProfileInBar.toUtf8().constData(), st, o.displayProfileInBar, false);
    getFiled(keys::App::swapBackBufferAlways.toUtf8().constData(), st, o.swapBackBufferAlways, false);
    getFiled(keys::App::fontSize.toUtf8().constData(), st, o.fontSize, 9.0);
    getFiled(keys::App::autoScrollLatestData.toUtf8().constData(), st, o.autoScrollLatestData, true);
    getFiled(keys::App::promptSaveOnExit.toUtf8().constData(), st, o.promptSaveOnExit, true);
    getFiled("tdmRealtimeDecode", st, o.tdmRealtimeDecode, false);

    getFiled("analogDisplayTriggerTdmValid", st, o.analogDisplayTriggerTdmValid, false);
    getFiled("analogDisplayTriggerTdmEnable", st, o.analogDisplayTriggerTdmEnable, false);
    getFiled("analogDisplayTriggerTdmMode", st, o.analogDisplayTriggerTdmMode, "auto");
    getFiled("analogDisplayTriggerTdmChannel", st, o.analogDisplayTriggerTdmChannel, 0);
    getFiled("analogDisplayTriggerTdmEdge", st, o.analogDisplayTriggerTdmEdge, "rising");
    getFiled("analogDisplayTriggerTdmLevel", st, o.analogDisplayTriggerTdmLevel, 0.0);
    getFiled("analogDisplayTriggerTdmPosition", st, o.analogDisplayTriggerTdmPosition, 50);

    getFiled("analogDisplayTriggerPwmValid", st, o.analogDisplayTriggerPwmValid, false);
    getFiled("analogDisplayTriggerPwmEnable", st, o.analogDisplayTriggerPwmEnable, false);
    getFiled("analogDisplayTriggerPwmMode", st, o.analogDisplayTriggerPwmMode, "auto");
    getFiled("analogDisplayTriggerPwmChannel", st, o.analogDisplayTriggerPwmChannel, 0);
    getFiled("analogDisplayTriggerPwmEdge", st, o.analogDisplayTriggerPwmEdge, "rising");
    getFiled("analogDisplayTriggerPwmLevel", st, o.analogDisplayTriggerPwmLevel, 50.0);
    getFiled("analogDisplayTriggerPwmPosition", st, o.analogDisplayTriggerPwmPosition, 50);

    getFiled(keys::App::version.toUtf8().constData(), st, o.version, 1);

    o.warnofMultiTrig = true;

    QString fmt;
    getFiled(keys::App::protocalFormats.toUtf8().constData(), st, fmt, "");
    if (fmt != ""){
        StringToFormatArray(fmt, o.m_protocolFormats);
    }

    float minSize = 0;
    float maxSize = 0;
    AppConfig::GetFontSizeRange(&minSize, &maxSize);

    if (o.version == 1 || o.fontSize < minSize || o.fontSize > maxSize)
    {
        o.fontSize = (maxSize + minSize) / 2;
    }
   
    st.endGroup();
}

static void _saveApp(AppOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::Application.toUtf8().constData());
    setFiled(keys::App::quickScroll.toUtf8().constData(), st, o.quickScroll);
    setFiled(keys::App::warnofMultiTrig.toUtf8().constData(), st, o.warnofMultiTrig);
    setFiled(keys::App::originalData.toUtf8().constData(), st, o.originalData);
    setFiled(keys::App::ableSaveLog.toUtf8().constData(), st, o.ableSaveLog);
    setFiled(keys::App::appendLogMode.toUtf8().constData(), st, o.appendLogMode);
    setFiled(keys::App::logLevel.toUtf8().constData(), st, o.logLevel);
    setFiled(keys::App::transDecoderDlg.toUtf8().constData(), st, o.transDecoderDlg);
    setFiled(keys::App::trigPosDisplayInMid.toUtf8().constData(), st, o.trigPosDisplayInMid);
    setFiled(keys::App::displayProfileInBar.toUtf8().constData(), st, o.displayProfileInBar);
    setFiled(keys::App::swapBackBufferAlways.toUtf8().constData(), st, o.swapBackBufferAlways);
    setFiled(keys::App::fontSize.toUtf8().constData(), st, o.fontSize);
    setFiled(keys::App::autoScrollLatestData.toUtf8().constData(), st, o.autoScrollLatestData);
    setFiled(keys::App::promptSaveOnExit.toUtf8().constData(), st, o.promptSaveOnExit);
    setFiled("tdmRealtimeDecode", st, o.tdmRealtimeDecode);

    setFiled("analogDisplayTriggerTdmValid", st, o.analogDisplayTriggerTdmValid);
    setFiled("analogDisplayTriggerTdmEnable", st, o.analogDisplayTriggerTdmEnable);
    setFiled("analogDisplayTriggerTdmMode", st, o.analogDisplayTriggerTdmMode);
    setFiled("analogDisplayTriggerTdmChannel", st, o.analogDisplayTriggerTdmChannel);
    setFiled("analogDisplayTriggerTdmEdge", st, o.analogDisplayTriggerTdmEdge);
    setFiled("analogDisplayTriggerTdmLevel", st, o.analogDisplayTriggerTdmLevel);
    setFiled("analogDisplayTriggerTdmPosition", st, o.analogDisplayTriggerTdmPosition);

    setFiled("analogDisplayTriggerPwmValid", st, o.analogDisplayTriggerPwmValid);
    setFiled("analogDisplayTriggerPwmEnable", st, o.analogDisplayTriggerPwmEnable);
    setFiled("analogDisplayTriggerPwmMode", st, o.analogDisplayTriggerPwmMode);
    setFiled("analogDisplayTriggerPwmChannel", st, o.analogDisplayTriggerPwmChannel);
    setFiled("analogDisplayTriggerPwmEdge", st, o.analogDisplayTriggerPwmEdge);
    setFiled("analogDisplayTriggerPwmLevel", st, o.analogDisplayTriggerPwmLevel);
    setFiled("analogDisplayTriggerPwmPosition", st, o.analogDisplayTriggerPwmPosition);

    setFiled(keys::App::version.toUtf8().constData(), st, APP_CONFIG_VERSION);

    QString fmt =  FormatArrayToString(o.m_protocolFormats);
    setFiled(keys::App::protocalFormats.toUtf8().constData(), st, fmt);
    st.endGroup();  
}

//-----frame

static void _loadDockOptions(DockOptions &o, QSettings &st, const char *group)
{
    st.beginGroup(group);
    getFiled(keys::Dock::decodeDock.toUtf8().constData(), st, o.decodeDock, false);
    getFiled(keys::Dock::triggerDock.toUtf8().constData(), st, o.triggerDock, false);
    getFiled(keys::Dock::measureDock.toUtf8().constData(), st, o.measureDock, false);
    getFiled(keys::Dock::searchDock.toUtf8().constData(), st, o.searchDock, false);
    getFiled(keys::Dock::deviceOptionsDock.toUtf8().constData(), st, o.deviceOptionsDock, false);
    getFiled(keys::Dock::logDock.toUtf8().constData(), st, o.logDock, false);
    st.endGroup();
}

static void _saveDockOptions(DockOptions &o, QSettings &st, const char *group)
{
    st.beginGroup(group);
    setFiled(keys::Dock::decodeDock.toUtf8().constData(), st, o.decodeDock);
    setFiled(keys::Dock::triggerDock.toUtf8().constData(), st, o.triggerDock);
    setFiled(keys::Dock::measureDock.toUtf8().constData(), st, o.measureDock);
    setFiled(keys::Dock::searchDock.toUtf8().constData(), st, o.searchDock);
    setFiled(keys::Dock::deviceOptionsDock.toUtf8().constData(), st, o.deviceOptionsDock);
    setFiled(keys::Dock::logDock.toUtf8().constData(), st, o.logDock);
    st.endGroup();
}

static void _loadFrame(FrameOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::MainFrame.toUtf8().constData()); 
    getFiled(keys::Frame::style.toUtf8().constData(), st, o.style, THEME_STYLE_DARK);
    getFiled(keys::Frame::language.toUtf8().constData(), st, o.language, -1);
    getFiled(keys::Frame::isMax.toUtf8().constData(), st, o.isMax, false);  
    getFiled(keys::Frame::left.toUtf8().constData(), st, o.left, 0);
    getFiled(keys::Frame::top.toUtf8().constData(), st, o.top, 0);
    getFiled(keys::Frame::right.toUtf8().constData(), st, o.right, 0);
    getFiled(keys::Frame::bottom.toUtf8().constData(), st, o.bottom, 0);
    getFiled(keys::Frame::x.toUtf8().constData(), st, o.x, NO_POINT_VALUE);
    getFiled(keys::Frame::y.toUtf8().constData(), st, o.y, NO_POINT_VALUE);
    getFiled(keys::Frame::ox.toUtf8().constData(), st, o.ox, NO_POINT_VALUE);
    getFiled(keys::Frame::oy.toUtf8().constData(), st, o.oy, NO_POINT_VALUE);
    getFiled(keys::Frame::displayName.toUtf8().constData(), st, o.displayName, "");

    _loadDockOptions(o._logicDock, st, "LOGIC_DOCK");
    _loadDockOptions(o._analogDock, st, "ANALOG_DOCK");
    _loadDockOptions(o._dsoDock, st, "DSO_DOCK");

    o.windowState = st.value(keys::Frame::windowState.toUtf8().constData(), QByteArray()).toByteArray();
    st.endGroup();

    if (o.language == -1 || (o.language != LAN_CN && o.language != LAN_EN)){
        //get local language
        QLocale locale;

        if (QLocale::languageToString(locale.language()) == "Chinese")
            o.language = LAN_CN;            
        else
            o.language = LAN_EN; 
    }
}

static void _saveFrame(FrameOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::MainFrame.toUtf8().constData());
    setFiled(keys::Frame::style.toUtf8().constData(), st, o.style);
    setFiled(keys::Frame::language.toUtf8().constData(), st, o.language);
    setFiled(keys::Frame::isMax.toUtf8().constData(), st, o.isMax);  
    setFiled(keys::Frame::left.toUtf8().constData(), st, o.left);
    setFiled(keys::Frame::top.toUtf8().constData(), st, o.top);
    setFiled(keys::Frame::right.toUtf8().constData(), st, o.right);
    setFiled(keys::Frame::bottom.toUtf8().constData(), st, o.bottom);
    setFiled(keys::Frame::x.toUtf8().constData(), st, o.x);
    setFiled(keys::Frame::y.toUtf8().constData(), st, o.y);
    setFiled(keys::Frame::ox.toUtf8().constData(), st, o.ox);
    setFiled(keys::Frame::oy.toUtf8().constData(), st, o.oy);
    setFiled(keys::Frame::displayName.toUtf8().constData(), st, o.displayName);

    st.setValue(keys::Frame::windowState.toUtf8().constData(), o.windowState); 

    _saveDockOptions(o._logicDock, st, "LOGIC_DOCK");
    _saveDockOptions(o._analogDock, st, "ANALOG_DOCK");
    _saveDockOptions(o._dsoDock, st, "DSO_DOCK");
    
    st.endGroup();
}

//------history
static void _loadHistory(UserHistory &o, QSettings &st)
{
    st.beginGroup(keys::Group::History.toUtf8().constData());
    getFiled(keys::History::exportDir.toUtf8().constData(), st, o.exportDir, ""); 
    getFiled(keys::History::saveDir.toUtf8().constData(), st, o.saveDir, ""); 
    getFiled(keys::History::showDocuments.toUtf8().constData(), st, o.showDocuments, true);
    getFiled(keys::History::screenShotPath.toUtf8().constData(), st, o.screenShotPath, ""); 
    getFiled(keys::History::sessionDir.toUtf8().constData(), st, o.sessionDir, ""); 
    getFiled(keys::History::openDir.toUtf8().constData(), st, o.openDir, ""); 
    getFiled(keys::History::protocolExportPath.toUtf8().constData(), st, o.protocolExportPath, ""); 
    getFiled(keys::History::exportFormat.toUtf8().constData(), st, o.exportFormat, ""); 
    st.endGroup();
}
 
static void _saveHistory(UserHistory &o, QSettings &st)
{
    st.beginGroup(keys::Group::History.toUtf8().constData());
    setFiled(keys::History::exportDir.toUtf8().constData(), st, o.exportDir); 
    setFiled(keys::History::saveDir.toUtf8().constData(), st, o.saveDir); 
    setFiled(keys::History::showDocuments.toUtf8().constData(), st, o.showDocuments); 
    setFiled(keys::History::screenShotPath.toUtf8().constData(), st, o.screenShotPath); 
    setFiled(keys::History::sessionDir.toUtf8().constData(), st, o.sessionDir); 
    setFiled(keys::History::openDir.toUtf8().constData(), st, o.openDir); 
    setFiled(keys::History::protocolExportPath.toUtf8().constData(), st, o.protocolExportPath);
    setFiled(keys::History::exportFormat.toUtf8().constData(), st, o.exportFormat); 
    st.endGroup();
}

/*
//------font
static void _loadFont(FontOptions &o, QSettings &st)
{
    st.beginGroup("FontSetting");
    getFiled("toolbarName", st, o.toolbar.name, "");
    getFiled("toolbarSize", st, o.toolbar.size, 9);
    getFiled("channelLabelName", st, o.channelLabel.name, "");
    getFiled("channelLabelSize", st, o.channelLabel.size, 9);
    getFiled("channelBodyName", st, o.channelBody.name, "");
    getFiled("channelBodySize", st, o.channelBody.size, 9);
    getFiled("rulerName", st, o.ruler.name, "");
    getFiled("ruleSize", st, o.ruler.size, 9);
    getFiled("titleName", st, o.title.name, "");
    getFiled("titleSize", st, o.title.size, 9);
    getFiled("otherName", st, o.other.name, "");
    getFiled("otherSize", st, o.other.size, 9);

    st.endGroup();
}

static void _saveFont(FontOptions &o, QSettings &st)
{
    st.beginGroup("FontSetting");
    setFiled("toolbarName", st, o.toolbar.name);
    setFiled("toolbarSize", st, o.toolbar.size);
    setFiled("channelLabelName", st, o.channelLabel.name);
    setFiled("channelLabelSize", st, o.channelLabel.size);
    setFiled("channelBodyName", st, o.channelBody.name);
    setFiled("channelBodySize", st, o.channelBody.size);
    setFiled("rulerName", st, o.ruler.name);
    setFiled("ruleSize", st, o.ruler.size);
    setFiled("titleName", st, o.title.name);
    setFiled("titleSize", st, o.title.size);
    setFiled("otherName", st, o.other.name);
    setFiled("otherSize", st, o.other.size);

    st.endGroup();
}
*/

//------------AppConfig

static void _loadShortcuts(ShortcutOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::Shortcuts.toUtf8().constData());
    int count = st.beginReadArray(keys::Shortcuts::items.toUtf8().constData());
    o.items.clear();
    for (int i = 0; i < count; i++) {
        st.setArrayIndex(i);
        ShortcutItem item;
        item.actionId = st.value(keys::Shortcuts::actionId.toUtf8().constData(), 0).toInt();
        item.keySequence = st.value(keys::Shortcuts::keySequence.toUtf8().constData(), "").toString();
        o.items.append(item);
    }
    st.endArray();
    st.endGroup();
}

static void _saveShortcuts(ShortcutOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::Shortcuts.toUtf8().constData());
    st.beginWriteArray(keys::Shortcuts::items.toUtf8().constData(), o.items.size());
    for (int i = 0; i < o.items.size(); i++) {
        st.setArrayIndex(i);
        st.setValue(keys::Shortcuts::actionId.toUtf8().constData(), o.items[i].actionId);
        st.setValue(keys::Shortcuts::keySequence.toUtf8().constData(), o.items[i].keySequence);
    }
    st.endArray();
    st.endGroup();
}

static void _loadStyle(StyleOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::Style.toUtf8().constData());
    int count = st.beginReadArray(keys::Style::items.toUtf8().constData());
    o.items.clear();
    for (int i = 0; i < count; i++) {
        st.setArrayIndex(i);
        StyleTokenItem item;
        item.tokenName = st.value(keys::Style::tokenName.toUtf8().constData(), "").toString();
        item.value = st.value(keys::Style::value.toUtf8().constData(), "").toString();
        o.items.append(item);
    }
    st.endArray();
    st.endGroup();
}

static void _saveStyle(StyleOptions &o, QSettings &st)
{
    st.beginGroup(keys::Group::Style.toUtf8().constData());
    st.beginWriteArray(keys::Style::items.toUtf8().constData(), o.items.size());
    for (int i = 0; i < o.items.size(); i++) {
        st.setArrayIndex(i);
        st.setValue(keys::Style::tokenName.toUtf8().constData(), o.items[i].tokenName);
        st.setValue(keys::Style::value.toUtf8().constData(), o.items[i].value);
    }
    st.endArray();
    st.endGroup();
}

AppConfig::AppConfig()
    : _saveFrameTimer(nullptr)
    , _saveAppTimer(nullptr)
    , _saveHistoryTimer(nullptr)
    , _saveShortcutsTimer(nullptr)
    , _saveStyleTimer(nullptr)
    , _saveDeviceTimer(nullptr)
{
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     [](){ AppConfig::Instance().flushPendingSaves(); });
}

AppConfig::AppConfig(AppConfig &o) 
{
    (void)o;
}

AppConfig::~AppConfig()
{
}

// P2-A: Setting change listener registration / notification.
void AppConfig::register_setting_listener(SettingChangeListener *listener)
{
    if (listener)
        _setting_listeners.push_back(listener);
}

void AppConfig::unregister_setting_listener(SettingChangeListener *listener)
{
    auto it = std::find(_setting_listeners.begin(),
                        _setting_listeners.end(), listener);
    if (it != _setting_listeners.end())
        _setting_listeners.erase(it);
}

void AppConfig::notify_setting_changed(const QString &group,
                                       const QString &key,
                                       const QVariant &value)
{
    for (auto *l : _setting_listeners)
        l->on_setting_changed(group, key, value);
}

AppConfig& AppConfig::Instance()
 {
     static AppConfig *ins = nullptr;
     if (ins == nullptr){
         ins = new AppConfig();
     }
     return *ins;
 }

void AppConfig::LoadAll()
{   
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _loadApp(appOptions, st);
    _loadHistory(userHistory, st);
    _loadFrame(frameOptions, st);
    _loadShortcuts(shortcutOptions, st);
    _loadStyle(styleOptions, st);

    st.beginGroup(keys::Group::Device.toUtf8().constData());
    default_sample_limit_ = st.value("defaultSampleLimit", 1000000ULL).toULongLong();
    deviceOptions.streamMemBuff = st.value(keys::Device::streamMemBuff.toUtf8().constData(), 16.0).toDouble();
    deviceOptions.streamBuff = st.value(keys::Device::streamBuff.toUtf8().constData(), 16.0).toDouble();
    deviceOptions.diskCacheEnable = st.value(keys::Device::diskCacheEnable.toUtf8().constData(), false).toBool();
    deviceOptions.diskCachePath = st.value(keys::Device::diskCachePath.toUtf8().constData(), "").toString();
    deviceOptions.lastDeviceDriver = st.value(keys::Device::lastDeviceDriver.toUtf8().constData(), "").toString();
    deviceOptions.lastDeviceConnId = st.value(keys::Device::lastDeviceConnId.toUtf8().constData(), "").toString();
    deviceOptions.glitchAutoApply = st.value(keys::Device::glitchAutoApply.toUtf8().constData(), false).toBool();
    deviceOptions.glitchDefaultThreshold = st.value(keys::Device::glitchDefaultThreshold.toUtf8().constData(), 3).toInt();
    deviceOptions.glitchShowOverlay = st.value(keys::Device::glitchShowOverlay.toUtf8().constData(), true).toBool();
    st.endGroup();

    //pxv_dbg("Config file path:\"%s\"", st.fileName().toUtf8().data());
}

void AppConfig::SaveApp()
{
    if (!_saveAppTimer) {
        _saveAppTimer = new QTimer();
        _saveAppTimer->setSingleShot(true);
        QObject::connect(_saveAppTimer, &QTimer::timeout, [this](){ doSaveApp(); });
    }
    _saveAppTimer->start(2000);
}

void AppConfig::doSaveApp()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveApp(appOptions, st);

    st.beginGroup(keys::Group::Device.toUtf8().constData());
    st.setValue("defaultSampleLimit", (qulonglong)default_sample_limit_);
    st.endGroup();
}

void AppConfig::SaveDevice()
{
    if (!_saveDeviceTimer) {
        _saveDeviceTimer = new QTimer();
        _saveDeviceTimer->setSingleShot(true);
        QObject::connect(_saveDeviceTimer, &QTimer::timeout, [this](){ doSaveDevice(); });
    }
    _saveDeviceTimer->start(2000);
}

void AppConfig::doSaveDevice()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    st.beginGroup(keys::Group::Device.toUtf8().constData());
    st.setValue(keys::Device::streamMemBuff.toUtf8().constData(), deviceOptions.streamMemBuff);
    st.setValue(keys::Device::streamBuff.toUtf8().constData(), deviceOptions.streamBuff);
    st.setValue(keys::Device::diskCacheEnable.toUtf8().constData(), deviceOptions.diskCacheEnable);
    st.setValue(keys::Device::diskCachePath.toUtf8().constData(), deviceOptions.diskCachePath);
    st.setValue(keys::Device::lastDeviceDriver.toUtf8().constData(), deviceOptions.lastDeviceDriver);
    st.setValue(keys::Device::lastDeviceConnId.toUtf8().constData(), deviceOptions.lastDeviceConnId);
    st.setValue(keys::Device::glitchAutoApply.toUtf8().constData(), deviceOptions.glitchAutoApply);
    st.setValue(keys::Device::glitchDefaultThreshold.toUtf8().constData(), deviceOptions.glitchDefaultThreshold);
    st.setValue(keys::Device::glitchShowOverlay.toUtf8().constData(), deviceOptions.glitchShowOverlay);
    st.endGroup();
}

void AppConfig::SaveHistory()
{
    if (!_saveHistoryTimer) {
        _saveHistoryTimer = new QTimer();
        _saveHistoryTimer->setSingleShot(true);
        QObject::connect(_saveHistoryTimer, &QTimer::timeout, [this](){ doSaveHistory(); });
    }
    _saveHistoryTimer->start(2000);
}

void AppConfig::doSaveHistory()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveHistory(userHistory, st);
}

void AppConfig::SaveFrame()
{
    if (!_saveFrameTimer) {
        _saveFrameTimer = new QTimer();
        _saveFrameTimer->setSingleShot(true);
        QObject::connect(_saveFrameTimer, &QTimer::timeout, [this](){ doSaveFrame(); });
    }
    _saveFrameTimer->start(2000);
}

void AppConfig::doSaveFrame()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveFrame(frameOptions, st);
}

void AppConfig::SaveShortcuts()
{
    if (!_saveShortcutsTimer) {
        _saveShortcutsTimer = new QTimer();
        _saveShortcutsTimer->setSingleShot(true);
        QObject::connect(_saveShortcutsTimer, &QTimer::timeout, [this](){ doSaveShortcuts(); });
    }
    _saveShortcutsTimer->start(2000);
}

void AppConfig::doSaveShortcuts()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveShortcuts(shortcutOptions, st);
}

void AppConfig::SaveStyle()
{
    if (!_saveStyleTimer) {
        _saveStyleTimer = new QTimer();
        _saveStyleTimer->setSingleShot(true);
        QObject::connect(_saveStyleTimer, &QTimer::timeout, [this](){ doSaveStyle(); });
    }
    _saveStyleTimer->start(2000);
}

void AppConfig::doSaveStyle()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveStyle(styleOptions, st);

    // P2-A: Notify listeners that settings were reloaded from disk.
    // Pass empty group/key to signal a bulk reload.
    notify_setting_changed(QString(), QString(), QVariant());
}

void AppConfig::flushPendingSaves()
{
    if (_saveFrameTimer && _saveFrameTimer->isActive()) {
        _saveFrameTimer->stop();
        doSaveFrame();
    }
    if (_saveAppTimer && _saveAppTimer->isActive()) {
        _saveAppTimer->stop();
        doSaveApp();
    }
    if (_saveHistoryTimer && _saveHistoryTimer->isActive()) {
        _saveHistoryTimer->stop();
        doSaveHistory();
    }
    if (_saveShortcutsTimer && _saveShortcutsTimer->isActive()) {
        _saveShortcutsTimer->stop();
        doSaveShortcuts();
    }
    if (_saveStyleTimer && _saveStyleTimer->isActive()) {
        _saveStyleTimer->stop();
        doSaveStyle();
    }
    if (_saveDeviceTimer && _saveDeviceTimer->isActive()) {
        _saveDeviceTimer->stop();
        doSaveDevice();
    }
}

void AppConfig::SetProtocolFormat(const std::string &protocolName, const std::string &value)
{
    bool bChange = false;
    for (StringPair &o : appOptions.m_protocolFormats){
        if (o.m_key == protocolName){
            o.m_value = value;
            bChange = true;
            break;
        }    
    }

    if (!bChange)
    {
        if (appOptions.m_protocolFormats.size() > MAX_PROTOCOL_FORMAT_LIST)
        {
            while (appOptions.m_protocolFormats.size() < MAX_PROTOCOL_FORMAT_LIST)
            {
                appOptions.m_protocolFormats.erase(appOptions.m_protocolFormats.begin());
            }
        }
        appOptions.m_protocolFormats.push_back(StringPair(protocolName, value));
        bChange = true;
    }

    if (bChange){
        SaveApp();
    }
}

std::string AppConfig::GetProtocolFormat(const std::string &protocolName)
{
     for (StringPair &o : appOptions.m_protocolFormats){
        if (o.m_key == protocolName){ 
            return o.m_value;
        }
    }
    return "";
}

void AppConfig::GetFontSizeRange(float *minSize, float *maxSize)
{
    if (!minSize || !maxSize) {
        pxv_warn("%s", "AppConfig::GetFontSizeRange: minSize or maxSize is nullptr");
        return;
    }
    assert(minSize);
    assert(maxSize);

#ifdef _WIN32
        *minSize = 7;
        *maxSize = 12;
#endif

#ifdef Q_OS_LINUX
        *minSize = 8;
        *maxSize = 14;
#endif

#ifdef Q_OS_DARWIN
        *minSize = 9;
        *maxSize = 15;
#endif
}

bool AppConfig::IsDarkStyle()
{
    if (frameOptions.style == THEME_STYLE_DARK){
        return true;
    }
    if (frameOptions.style == THEME_STYLE_LIGHT){
        return false;
    }
    // For custom themes, determine by bg-base luminance
    QColor bg = GetThemeColor("@bg-base");
    if (bg.isValid()){
        return bg.lightnessF() < 0.5;
    }
    return true; // default to dark
}

QColor AppConfig::GetStyleColor()
{
    QColor c = GetThemeColor("@bg-base");
    if (c.isValid())
        return c;
    if (IsDarkStyle()){
        return QColor(38, 38, 38);
    }
    else{
        return QColor(248, 248, 248);
    }
}

void AppConfig::SetThemeTokens(const QHash<QString, QString> &tokens)
{
    _themeTokens = tokens;
}

QString AppConfig::GetThemeTokenValue(const QString &tokenName) const
{
    return _themeTokens.value(tokenName, QString());
}

QColor AppConfig::GetThemeColor(const QString &tokenName) const
{
    QString val = GetThemeTokenValue(tokenName);
    if (val.isEmpty())
        return QColor();

    val = val.trimmed();

    if (val.startsWith("rgba(")) {
        QString inner = val.mid(5, val.length() - 6);
        QStringList parts = inner.split(',');
        if (parts.size() == 4) {
            bool ok1, ok2, ok3, ok4;
            int r = parts[0].trimmed().toInt(&ok1);
            int g = parts[1].trimmed().toInt(&ok2);
            int b = parts[2].trimmed().toInt(&ok3);
            int a = parts[3].trimmed().toInt(&ok4);
            if (ok1 && ok2 && ok3 && ok4)
                return QColor(r, g, b, a);
        }
        return QColor();
    }

    if (val.startsWith("rgb(")) {
        QString inner = val.mid(4, val.length() - 5);
        QStringList parts = inner.split(',');
        if (parts.size() >= 3) {
            bool ok1, ok2, ok3;
            int r = parts[0].trimmed().toInt(&ok1);
            int g = parts[1].trimmed().toInt(&ok2);
            int b = parts[2].trimmed().toInt(&ok3);
            if (ok1 && ok2 && ok3) {
                if (parts.size() == 4) {
                    bool ok4;
                    int a = parts[3].trimmed().toInt(&ok4);
                    if (ok4)
                        return QColor(r, g, b, a);
                }
                return QColor(r, g, b);
            }
        }
        return QColor();
    }

    QColor c(val);
    return c;
}


//-------------api
QString GetIconPath()
{
    QString style = AppConfig::Instance().frameOptions.style;
    if (style == ""){
        style = THEME_STYLE_DARK;
    }
    // Custom themes (atom, ayu, etc.) don't have their own icon directories;
    // fall back to dark or light based on theme type
    if (style != THEME_STYLE_DARK && style != THEME_STYLE_LIGHT){
        style = AppConfig::Instance().IsDarkStyle() ? THEME_STYLE_DARK : THEME_STYLE_LIGHT;
    }
    return ":/icons/" + style;
}

QString GetAppDataDir()
{
//applicationDirPath not end with '/'
#ifdef Q_OS_LINUX
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.cd("..") && dir.cd("share") && dir.cd("PXView"))
    {
         return dir.absolutePath();        
    }
    QDir dir1("/usr/local/share/PXView");
    if (dir1.exists()){
        return dir1.absolutePath();
    }

    pxv_err("Data directory is not exists: ../share/PXView");
    return QString();
#else

#ifdef Q_OS_DARWIN
    QDir dir1(QCoreApplication::applicationDirPath());
    // "../Resources/share/PXView"
    if (dir1.cd("..") && dir1.cd("Resources") && dir1.cd("share") && dir1.cd("PXView")){
        return dir1.absolutePath();
    }

#endif

#ifdef Q_OS_WIN
    // On Windows, try ../share/PXView first (install directory structure)
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.cd("..") && dir.cd("share") && dir.cd("PXView"))
    {
         return dir.absolutePath();        
    }
#endif

    // The bin location
    return QCoreApplication::applicationDirPath();
#endif
    return QString();
}

QString GetFirmwareDir()
{
    QDir dir1 =  GetAppDataDir() + "/res";
    // ./res
    if (dir1.exists()){
        return dir1.absolutePath();
    }

    QDir dir(QCoreApplication::applicationDirPath());
    // ../share/PXView/res
    if (dir.cd("..") && dir.cd("share") && dir.cd("PXView") && dir.cd("res"))
    {
         return dir.absolutePath();
    }
 
#ifdef Q_OS_DARWIN
    // macOS bundle (../Resources/share/PXView/res)
    if (dir.cd("..") && dir.cd("Resources") && dir.cd("share") && dir.cd("PXView") && dir.cd("res"))
    {
         return dir.absolutePath();
    }
#endif

    pxv_err("%s%s", "Resource directory is not exists:", dir1.absolutePath().toUtf8().data());
    return dir1.absolutePath();
}

QString GetUserDataDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString GetDecodeScriptDir()
{
    QString path = GetAppDataDir() + "/decoders";

    QDir dir1;
    // ./decoders
    if (dir1.exists(path))
    {
         return path;
    }

    // QDir dir(QCoreApplication::applicationDirPath());
    // if (dir.cd("..") && dir.cd("share") && dir.cd("libsigrokdecode") && dir.cd("decoders"))
    // {
    //      return dir.absolutePath();
    // }

#ifdef Q_OS_DARWIN
    dir1.cd(QCoreApplication::applicationDirPath());
    //if (dir1.cd("..") && dir1.cd("Resources") && dir1.cd("share") && dir1.cd("PXView") &&
    if (dir1.cd("..") && dir1.cd("Resources") && dir1.cd("share") &&
        dir1.cd("libsigrokdecode") && dir1.cd("decoders"))
    {
         return dir1.absolutePath();
    }

#elif defined(Q_OS_UNIX)
    QDir dir(QCoreApplication::applicationDirPath());
    // ../share/PXView/libsigrokdecode/decoders
    //if (dir.cd("..") && dir.cd("share")&& dir.cd("PXView")  && dir.cd("libsigrokdecode") && dir.cd("decoders"))
    if (dir.cd("..") && dir.cd("share") && dir.cd("libsigrokdecode") && dir.cd("decoders"))
    {
        return dir.absolutePath();
    }

#elif defined(Q_OS_WIN)
    // On Windows, try ../share/libsigrokdecode/decoders (install directory structure)
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.cd("..") && dir.cd("share") && dir.cd("libsigrokdecode") && dir.cd("decoders"))
    {
        return dir.absolutePath();
    }
#endif
    return "";
}

QString GetProfileDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}