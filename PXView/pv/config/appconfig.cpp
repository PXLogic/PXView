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

#include "appconfig.h" 
#include <QApplication>
#include <QSettings>
#include <QLocale>
#include <QDir> 
#include <assert.h>
#include <QStandardPaths>
#include "../log.h"
  
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
    getFiled("logLevel", st, o.logLevel, 3);
    getFiled("transDecoderDlg", st, o.transDecoderDlg, true);
    getFiled("trigPosDisplayInMid", st, o.trigPosDisplayInMid, true);
    getFiled("displayProfileInBar", st, o.displayProfileInBar, false);
    getFiled("swapBackBufferAlways", st, o.swapBackBufferAlways, false);
    getFiled("fontSize", st, o.fontSize, 9.0);
    getFiled("autoScrollLatestData", st, o.autoScrollLatestData, true);
    getFiled("version", st, o.version, 1);

    o.warnofMultiTrig = true;

    QString fmt;
    getFiled("protocalFormats", st, fmt, "");
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
    setFiled("autoScrollLatestData", st, o.autoScrollLatestData);
    setFiled("version", st, APP_CONFIG_VERSION);

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
    getFiled("deviceOptionsDoc", st, o.deviceOptionsDock, false);
    getFiled("signalProcessingDoc", st, o.signalProcessingDock, false);
    getFiled("logDoc", st, o.logDock, false);
    st.endGroup();
}

static void _saveDockOptions(DockOptions &o, QSettings &st, const char *group)
{
    st.beginGroup(group);
    setFiled("decodeDoc", st, o.decodeDock);
    setFiled("triggerDoc", st, o.triggerDock);
    setFiled("measureDoc", st, o.measureDock);
    setFiled("searchDoc", st, o.searchDock);
    setFiled("deviceOptionsDoc", st, o.deviceOptionsDock);
    setFiled("signalProcessingDoc", st, o.signalProcessingDock);
    setFiled("logDoc", st, o.logDock);
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
    getFiled("x", st, o.x, NO_POINT_VALUE);
    getFiled("y", st, o.y, NO_POINT_VALUE);
    getFiled("ox", st, o.ox, NO_POINT_VALUE);
    getFiled("oy", st, o.oy, NO_POINT_VALUE);
    getFiled("displayName", st, o.displayName, "");

    _loadDockOptions(o._logicDock, st, "LOGIC_DOCK");
    _loadDockOptions(o._analogDock, st, "ANALOG_DOCK");
    _loadDockOptions(o._dsoDock, st, "DSO_DOCK");

    o.windowState = st.value("windowState", QByteArray()).toByteArray();
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
    setFiled("x", st, o.x);
    setFiled("y", st, o.y);
    setFiled("ox", st, o.ox);
    setFiled("oy", st, o.oy);
    setFiled("displayName", st, o.displayName);

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

static void _loadShortcuts(ShortcutOptions &o, QSettings &st)
{
    st.beginGroup("Shortcuts");
    int count = st.beginReadArray("item");
    o.items.clear();
    for (int i = 0; i < count; i++) {
        st.setArrayIndex(i);
        ShortcutItem item;
        item.actionId = st.value("actionId", 0).toInt();
        item.keySequence = st.value("keySequence", "").toString();
        o.items.append(item);
    }
    st.endArray();
    st.endGroup();
}

static void _saveShortcuts(ShortcutOptions &o, QSettings &st)
{
    st.beginGroup("Shortcuts");
    st.beginWriteArray("item", o.items.size());
    for (int i = 0; i < o.items.size(); i++) {
        st.setArrayIndex(i);
        st.setValue("actionId", o.items[i].actionId);
        st.setValue("keySequence", o.items[i].keySequence);
    }
    st.endArray();
    st.endGroup();
}

static void _loadStyle(StyleOptions &o, QSettings &st)
{
    st.beginGroup("CustomStyle");
    int count = st.beginReadArray("token");
    o.items.clear();
    for (int i = 0; i < count; i++) {
        st.setArrayIndex(i);
        StyleTokenItem item;
        item.tokenName = st.value("name", "").toString();
        item.value = st.value("value", "").toString();
        o.items.append(item);
    }
    st.endArray();
    st.endGroup();
}

static void _saveStyle(StyleOptions &o, QSettings &st)
{
    st.beginGroup("CustomStyle");
    st.beginWriteArray("token", o.items.size());
    for (int i = 0; i < o.items.size(); i++) {
        st.setArrayIndex(i);
        st.setValue("name", o.items[i].tokenName);
        st.setValue("value", o.items[i].value);
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
    _loadShortcuts(shortcutOptions, st);
    _loadStyle(styleOptions, st);

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
    assert(false);
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