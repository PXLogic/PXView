/*
 * This file is part of the PXView project.
 *
 * P2-A: Centralised setting key constants.  All QSettings string keys
 * are defined here as `static const QString` to provide compile-time
 * spell-checking and IDE auto-completion.  New code should use these
 * constants instead of bare string literals.
 *
 * Usage:
 *   #include "pv/config/appconfig_keys.h"
 *   st.value(keys::App::quickScroll, true);
 *   st.setValue(keys::App::quickScroll, val);
 */

#pragma once

#include <QString>

namespace pv {
namespace config {
namespace keys {

// ---- Group names ----
namespace Group {
    static const QString Application = "Application";
    static const QString MainFrame  = "MainFrame";
    static const QString History    = "History";
    static const QString Font       = "Font";
    static const QString Shortcuts  = "Shortcuts";
    static const QString Style      = "Style";
    static const QString Device     = "Device";
}

// ---- Application group keys ----
namespace App {
    static const QString quickScroll         = "quickScroll";
    static const QString warnofMultiTrig     = "warnofMultiTrig";
    static const QString originalData        = "originalData";
    static const QString ableSaveLog         = "ableSaveLog";
    static const QString appendLogMode       = "appendLogMode";
    static const QString logLevel            = "logLevel";
    static const QString transDecoderDlg     = "transDecoderDlg";
    static const QString trigPosDisplayInMid = "trigPosDisplayInMid";
    static const QString displayProfileInBar = "displayProfileInBar";
    static const QString swapBackBufferAlways= "swapBackBufferAlways";
    static const QString fontSize            = "fontSize";
    static const QString autoScrollLatestData= "autoScrollLatestData";
    static const QString promptSaveOnExit    = "promptSaveOnExit";
    static const QString version             = "version";
    static const QString protocalFormats     = "protocalFormats";
}

// ---- Dock options keys (used with dynamic group prefix) ----
namespace Dock {
    static const QString decodeDock         = "decodeDoc";
    static const QString triggerDock        = "triggerDoc";
    static const QString measureDock        = "measureDoc";
    static const QString searchDock         = "searchDoc";
    static const QString deviceOptionsDock  = "deviceOptionsDoc";
    static const QString logDock            = "logDoc";
}

// ---- MainFrame group keys ----
namespace Frame {
    static const QString style        = "style";
    static const QString language     = "language";
    static const QString isMax        = "isMax";
    static const QString left         = "left";
    static const QString top          = "top";
    static const QString right        = "right";
    static const QString bottom       = "bottom";
    static const QString x            = "x";
    static const QString y            = "y";
    static const QString ox           = "ox";
    static const QString oy           = "oy";
    static const QString displayName  = "displayName";
    static const QString windowState  = "windowState";
}

// ---- History group keys ----
namespace History {
    static const QString exportDir           = "exportDir";
    static const QString saveDir             = "saveDir";
    static const QString showDocuments       = "showDocuments";
    static const QString screenShotPath      = "screenShotPath";
    static const QString sessionDir          = "sessionDir";
    static const QString openDir             = "openDir";
    static const QString protocolExportPath  = "protocolExportPath";
    static const QString exportFormat        = "exportFormat";
}

// ---- Font group keys ----
namespace Font {
    static const QString toolbar       = "toolbar";
    static const QString channelLabel  = "channelLabel";
    static const QString channelBody   = "channelBody";
    static const QString ruler         = "ruler";
    static const QString title         = "title";
    static const QString other         = "other";
    // Sub-keys for FontParam
    static const QString name          = "name";
    static const QString size          = "size";
}

// ---- Shortcuts group keys ----
namespace Shortcuts {
    static const QString items = "items";
    static const QString actionId = "actionId";
    static const QString keySequence = "keySequence";
}

// ---- Style group keys ----
namespace Style {
    static const QString items = "items";
    static const QString tokenName = "tokenName";
    static const QString value = "value";
}

// ---- Device group keys ----
namespace Device {
    static const QString streamMemBuff         = "streamMemBuff";
    static const QString streamBuff            = "streamBuff";
    static const QString diskCacheEnable       = "diskCacheEnable";
    static const QString diskCachePath         = "diskCachePath";
    static const QString lastDeviceDriver      = "lastDeviceDriver";
    static const QString lastDeviceConnId      = "lastDeviceConnId";
    static const QString glitchAutoApply       = "glitchAutoApply";
    static const QString glitchDefaultThreshold= "glitchDefaultThreshold";
    static const QString glitchShowOverlay     = "glitchShowOverlay";
}

} // namespace keys
} // namespace config
} // namespace pv
