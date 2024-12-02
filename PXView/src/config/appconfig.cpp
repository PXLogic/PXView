/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 * 
 * Copyright (C) 2021 
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

#include "appconfig.h" 
#include <QApplication>
#include <QSettings>
#include <QLocale>
#include <QDir> 
#include <assert.h>
#include <QStandardPaths>
#include "../log.h"

#define MAX_PROTOCOL_FORMAT_LIST 15

namespace dsv {
namespace config {

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
}

static void getFiled(const char *key, QSettings &st, int &f, int dv)
{
    f = st.value(key, dv).toInt();
}

static void setFiled(const char *key, QSettings &st, int f)
{
    st.setValue(key, f);
}

static void getFiled(const char *key, QSettings &st, bool &f, bool dv)
{
    f = st.value(key, dv).toBool();
}

static void setFiled(const char *key, QSettings &st, bool f){
    st.setValue(key, f);
}

static void getFiled(const char *key, QSettings &st, float &f, float dv)
{
    f = st.value(key, dv).toInt();
}

static void setFiled(const char *key, QSettings &st, float f)
{
    st.setValue(key, f);
}

///------ app
static void _loadApp(AppOptions &o, QSettings &st)
{
    st.beginGroup("Application"); 
    getFiled("quickScroll", st, o.quickScroll, true);
    getFiled("warnofMultiTrig", st, o.warnofMultiTrig, true);
    getFiled("originalData", st, o.originalData, false);
    getFiled("ableSaveLog", st, o.ableSaveLog, false);
    getFiled("appendLogMode", st, o.appendLogMode, false);
    getFiled("logLevel", st, o.logLevel, 5);
    getFiled("transDecoderDlg", st, o.transDecoderDlg, true);
    getFiled("trigPosDisplayInMid", st, o.trigPosDisplayInMid, true);
    getFiled("displayProfileInBar", st, o.displayProfileInBar, false);
    getFiled("swapBackBufferAlways", st, o.swapBackBufferAlways, false);
    getFiled("fontSize", st, o.fontSize, 9.0);

    o.warnofMultiTrig = true;

    QString fmt;
    getFiled("protocalFormats", st, fmt, "");
    if (fmt != ""){
         StringToFormatArray(fmt, o.m_protocolFormats);
    }
   
    st.endGroup();
}

static void _saveApp(AppOptions &o, QSettings &st)
{
    st.beginGroup("Application");
    setFiled("quickScroll", st, o.quickScroll);
    setFiled("warnofMultiTrig", st, o.warnofMultiTrig);
    setFiled("originalData", st, o.originalData);
    setFiled("ableSaveLog", st, o.ableSaveLog);
    setFiled("appendLogMode", st, o.appendLogMode);
    setFiled("logLevel", st, o.logLevel);
    setFiled("transDecoderDlg", st, o.transDecoderDlg);
    setFiled("trigPosDisplayInMid", st, o.trigPosDisplayInMid);
    setFiled("displayProfileInBar", st, o.displayProfileInBar);
    setFiled("swapBackBufferAlways", st, o.swapBackBufferAlways);
    setFiled("fontSize", st, o.fontSize);

    QString fmt =  FormatArrayToString(o.m_protocolFormats);
    setFiled("protocalFormats", st, fmt);
    st.endGroup();  
}

//-----frame

static void _loadDockOptions(DockOptions &o, QSettings &st, const char *group)
{
    st.beginGroup(group);
    getFiled("decodeDoc", st, o.decodeDock, false);
    getFiled("triggerDoc", st, o.triggerDock, false);
    getFiled("measureDoc", st, o.measureDock, false);
    getFiled("searchDoc", st, o.searchDock, false);
    st.endGroup();
}

static void _saveDockOptions(DockOptions &o, QSettings &st, const char *group)
{
    st.beginGroup(group);
    setFiled("decodeDoc", st, o.decodeDock);
    setFiled("triggerDoc", st, o.triggerDock);
    setFiled("measureDoc", st, o.measureDock);
    setFiled("searchDoc", st, o.searchDock);
    st.endGroup();
}

static void _loadFrame(FrameOptions &o, QSettings &st)
{
    st.beginGroup("MainFrame"); 
    getFiled("style", st, o.style, THEME_STYLE_DARK);
    getFiled("language", st, o.language, -1);
    getFiled("isMax", st, o.isMax, false);  
    getFiled("left", st, o.left, 0);
    getFiled("top", st, o.top, 0);
    getFiled("right", st, o.right, 0);
    getFiled("bottom", st, o.bottom, 0);

    _loadDockOptions(o._logicDock, st, "LOGIC_DOCK");
    _loadDockOptions(o._analogDock, st, "ANALOG_DOCK");
    _loadDockOptions(o._dsoDock, st, "DSO_DOCK");

    o.windowState = st.value("windowState", QByteArray()).toByteArray();
    // if(o.windowState.isEmpty()){
    //     o.windowState = QByteArray("\0\0\0\xff\0\0\0\0\xfd\0\0\0\x1\0\0\0\x1\0\0\0\xdf\0\0\x2\xc0\xfc\x2\0\0\0\x6\xfb\0\0\0\x1a\0p\0r\0o\0t\0o\0\x63\0o\0l\0_\0\x64\0o\0\x63\0k\0\0\0\0+\0\0\x2\xc0\0\0\0Z\0\xff\xff\xff\xfb\0\0\0\x18\0t\0r\0i\0g\0g\0\x65\0r\0_\0\x64\0o\0\x63\0k\0\0\0\0\0\xff\xff\xff\xff\0\0\0Z\0\xff\xff\xff\xfb\0\0\0 \0\x64\0s\0o\0_\0t\0r\0i\0g\0g\0\x65\0r\0_\0\x64\0o\0\x63\0k\0\0\0\0\0\xff\xff\xff\xff\0\0\0Z\0\xff\xff\xff\xfb\0\0\0\x18\0m\0\x65\0\x61\0s\0u\0r\0\x65\0_\0\x64\0o\0\x63\0k\0\0\0\0+\0\0\x2\xc0\0\0\0Z\0\xff\xff\xff\xfb\0\0\0\x14\0s\0\x65\0\x61\0r\0\x63\0_\0\x64\0o\0\x63\0k\0\0\0\x1\x3\0\0\0\xe1\0\0\0\x44\0\xff\xff\xff\xfb\0\0\0\x16\0s\0\x65\0\x61\0r\0\x63\0h\0_\0\x64\0o\0\x63\0k\0\0\0\0+\0\0\x2\xc0\0\0\0\0\0\0\0\0\0\0\x4\x7f\0\0\x2\xa9\0\0\0\x4\0\0\0\x4\0\0\0\b\0\0\0\b\xfc\0\0\0\x3\0\0\0\0\0\0\0\x1\0\0\0\x18\0s\0\x61\0m\0p\0l\0i\0n\0g\0_\0\x62\0\x61\0r\x3\0\0\0\0\xff\xff\xff\xff\0\0\0\0\0\0\0\0\0\0\0\x1\0\0\0\x1\xff\xff\xff\xff\x3\0\0\0\0\xff\xff\xff\xff\0\0\0\0\0\0\0\0\0\0\0\x2\0\0\0\x3\0\0\0\x10\0t\0r\0i\0g\0_\0\x62\0\x61\0r\x1\0\0\0\0\xff\xff\xff\xff\0\0\0\0\0\0\0\0\0\0\0\x10\0\x66\0i\0l\0\x65\0_\0\x62\0\x61\0r\x1\0\0\0\r\xff\xff\xff\xff\0\0\0\0\0\0\0\0\0\0\0\x10\0l\0o\0g\0o\0_\0\x62\0\x61\0r\x1\0\0\0\r\xff\xff\xff\xff\0\0\0\0\0\0\0\0");
    //     dsv_err("o.windowState.isEmpty()");
    // }
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
    st.beginGroup("MainFrame");
    setFiled("style", st, o.style);
    setFiled("language", st, o.language);
    setFiled("isMax", st, o.isMax);  
    setFiled("left", st, o.left);
    setFiled("top", st, o.top);
    setFiled("right", st, o.right);
    setFiled("bottom", st, o.bottom);
    st.setValue("windowState", o.windowState); 

    _saveDockOptions(o._logicDock, st, "LOGIC_DOCK");
    _saveDockOptions(o._analogDock, st, "ANALOG_DOCK");
    _saveDockOptions(o._dsoDock, st, "DSO_DOCK");
    
    st.endGroup();
}

//------history
static void _loadHistory(UserHistory &o, QSettings &st)
{
    st.beginGroup("UserHistory");
    getFiled("exportDir", st, o.exportDir, ""); 
    getFiled("saveDir", st, o.saveDir, ""); 
    getFiled("showDocuments", st, o.showDocuments, true);
    getFiled("screenShotPath", st, o.screenShotPath, ""); 
    getFiled("sessionDir", st, o.sessionDir, ""); 
    getFiled("openDir", st, o.openDir, ""); 
    getFiled("protocolExportPath", st, o.protocolExportPath, ""); 
    getFiled("exportFormat", st, o.exportFormat, ""); 
    st.endGroup();
}
 
static void _saveHistory(UserHistory &o, QSettings &st)
{
    st.beginGroup("UserHistory");
    setFiled("exportDir", st, o.exportDir); 
    setFiled("saveDir", st, o.saveDir); 
    setFiled("showDocuments", st, o.showDocuments); 
    setFiled("screenShotPath", st, o.screenShotPath); 
    setFiled("sessionDir", st, o.sessionDir); 
    setFiled("openDir", st, o.openDir); 
    setFiled("protocolExportPath", st, o.protocolExportPath);
    setFiled("exportFormat", st, o.exportFormat); 
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

AppConfig::AppConfig()
{ 
}

AppConfig::AppConfig(AppConfig &o) 
{
    (void)o;
}

AppConfig::~AppConfig()
{
}

 AppConfig& AppConfig::Instance()
 {
     static AppConfig *ins = NULL;
     if (ins == NULL){
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

    dsv_dbg("Config file path:\"%s\"", st.fileName().toUtf8().data());
}

void AppConfig::SaveApp()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveApp(appOptions, st);
}

void AppConfig::SaveHistory()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveHistory(userHistory, st);
}

void AppConfig::SaveFrame()
{
    QSettings st(QApplication::organizationName(), QApplication::applicationName());
    _saveFrame(frameOptions, st);
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

//-------------api
QString AppConfig::GetIconPath()
{   
    QString style = AppConfig::Instance().frameOptions.style;
    if (style == ""){
        style = THEME_STYLE_DARK;
    }
    return ":/icons/" + style;
}

QString AppConfig::GetAppDataDir()
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

    dsv_err("Data directory is not exists: ../share/PXView");
    assert(false);   
#else

#ifdef Q_OS_DARWIN
    QDir dir1(QCoreApplication::applicationDirPath());
    // "../Resources/share/PXView"
    if (dir1.cd("..") && dir1.cd("Resources") && dir1.cd("share") && dir1.cd("PXView")){
        return dir1.absolutePath();
    }
#endif

    // The bin location
    return QCoreApplication::applicationDirPath();
#endif
}

QString AppConfig::GetFirmwareDir()
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

#ifdef Q_OS_MAC
    // macOS bundle (../Resources/share/PXView/res)
    if (dir.cd("..") && dir.cd("Resources") && dir.cd("share") && dir.cd("PXView") && dir.cd("res"))
    {
         return dir.absolutePath();
    }
#endif
 
    dsv_err("%s%s", "Resource directory is not exists:", dir1.absolutePath().toUtf8().data());
    return dir1.absolutePath();
}

QString AppConfig::GetUserDataDir()
{
#if QT_VERSION >= 0x050400
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#else
    return QStandardPaths::writableLocation(QStandardPaths::DataLocation);
#endif
}

QString AppConfig::GetDecodeScriptDir()
{
    QString path = GetAppDataDir() + "/decoders";

    QDir dir1;
    // ./decoders
    if (dir1.exists(path))
    {
         return path;     
    }

#ifdef Q_OS_MAC
    dir1.cd(QCoreApplication::applicationDirPath());
    if (dir1.cd("..") && dir1.cd("Resources") && dir1.cd("share") && dir1.cd("PXView") &&
        dir1.cd("libsigrokdecode") && dir1.cd("decoders"))
    {
         return dir1.absolutePath();
    }
#elif defined(Q_OS_UNIX)
    QDir dir(QCoreApplication::applicationDirPath());
    // ../share/PXView/libsigrokdecode/decoders
    if (dir.cd("..") && dir.cd("share")&& dir.cd("PXView")  && dir.cd("libsigrokdecode") && dir.cd("decoders"))
    {
        return dir.absolutePath();        
    }
#endif
    return "";
}

QString AppConfig::GetProfileDir()
{
#if QT_VERSION >= 0x050400
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#else
    return QStandardPaths::writableLocation(QStandardPaths::DataLocation);
#endif
}

} //config
} //dsv
