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

#include "appcontrol.h"

#include <libsigrok/libsigrok.h>
#include <libsigrokdecode.h>
#include <QDir>
#include <QCoreApplication>
#include <QWidget>
#include <string>
#include <assert.h>
#include "sigsession.h"
#include "dsvdef.h"
#include "config/appconfig.h"
#include "log.h"
#include "utility/path.h"
#include "utility/encoding.h"
#include "data/leaf_block_pool.h"
#include "api/iapp_service.h"
#include "api/app_service.h"
#include "api/rpc_dispatcher.h"
#include "api/ws_transport.h"
#include "api/mcp_transport.h"
#include "api/direct_transport.h"

AppControl::AppControl()
{
    _topWindow = NULL; 
    _session = new pv::SigSession();
}

AppControl::AppControl(AppControl &o)
{
    (void)o;
}
 
AppControl::~AppControl()
{ 
   // DESTROY_OBJECT(_session);
}

AppControl* AppControl::Instance()
{
    static AppControl *ins = NULL;
    if (ins == NULL){
        ins = new AppControl();
    }
    return ins;
}

void AppControl::Destroy(){
    pv::data::LeafBlockPool::instance().drain();
} 

bool AppControl::Init()
{  
    pv::encoding::init();

    QString qs;
    std::string cs;

    qs = GetAppDataDir();
    cs = pv::path::ToUnicodePath(qs);
    pxv_info("GetAppDataDir:\"%s\"", cs.c_str());
    // Fork libsigrok's ds_set_user_data_dir is gone. Upstream libsigrok uses
    // sr_resource_set_hooks for resource path management. The pxlogic driver
    // resolves firmware paths internally; no explicit set needed here.
    cs = pv::path::ConvertPath(qs);

    qs = GetFirmwareDir();
    cs = pv::path::ToUnicodePath(qs);
    pxv_info("GetFirmwareDir:\"%s\"", cs.c_str());

    // Expose PXView/res as a libsigrok firmware search path. Upstream
    // libsigrok's sr_resource_open() searches SIGROK_FIRMWARE_PATH (a
    // path-separator-delimited list) plus the default sigrok-firmware dirs.
    // The pxlogic driver uses sr_resource_open(SR_RESOURCE_FIRMWARE, name)
    // with a bare filename (e.g. "SCI_LOGIC.bin"), so PXView's private
    // firmware dir (install.dir/share/PXView/res) MUST be on the search
    // path or FPGA/CPU firmware loading fails with "Firmware not found.",
    // which previously caused the device to be reset (rst usb) and drop
    // off the bus. Set the env var before _session->init() so the very
    // first sr_resourcepaths_get() call (during scan/dev_open) sees it.
    {
        // Preserve any pre-existing SIGROK_FIRMWARE_PATH entries.
        QString combined = QString::fromLocal8Bit(
            g_getenv("SIGROK_FIRMWARE_PATH"));
        if (!combined.isEmpty())
            combined += QString::fromLatin1(G_SEARCHPATH_SEPARATOR_S);
        combined += qs;
        g_setenv("SIGROK_FIRMWARE_PATH",
            combined.toUtf8().constData(), TRUE);
    }

    qs = GetUserDataDir();
    cs = pv::path::ToUnicodePath(qs);
    pxv_info("GetUserDataDir:\"%s\"", cs.c_str());

    qs = GetDecodeScriptDir();
    cs = pv::path::ToUnicodePath(qs);
    pxv_info("GetDecodeScriptDir:\"%s\"", cs.c_str());
    //---------------end print directorys.

    _session->init();

    srd_log_set_context(pxv_log_context());

#if defined(_WIN32)
    // Set Python home to application directory for embedded Python
    QString pythonHome = QCoreApplication::applicationDirPath();
    QDir pydir(pythonHome);
    QStringList zipFiles = pydir.entryList(QStringList() << "python*.zip", QDir::Files);
    if (!zipFiles.isEmpty()) {
        const wchar_t *pyhome = reinterpret_cast<const wchar_t*>(pythonHome.utf16());
        srd_set_python_home(pyhome);
        pxv_info("Set Python home to: %s", pythonHome.toUtf8().data());
    }
#if defined(DEBUG_INFO)
    //able run debug with qtcreator
    QString pythonHomeDebug = "c:/python";
    QDir pydirDebug;
    if (pydirDebug.exists(pythonHomeDebug)){
        const wchar_t *pyhome = reinterpret_cast<const wchar_t*>(pythonHomeDebug.utf16());
        srd_set_python_home(pyhome);
    }
#endif
#endif
    
    //the python script path of decoder
    char path[256] = {0};
    QString dir = GetDecodeScriptDir();   
    strcpy(path, dir.toUtf8().data());

    // Initialise libsigrokdecode
    if (srd_init(path) != SRD_OK)
    { 
        pxv_err("ERROR: libsigrokdecode init failed.");
        return false;
    }

    // Add C decoder search paths
    {
        QString cDecDir = GetAppDataDir();
        QDir cDecPath(cDecDir);
        if (cDecPath.cd("c_decoders") || cDecPath.cd("../libsigrokdecode/c_decoders")) {
            std::string cs = pv::path::ConvertPath(cDecPath.absolutePath());
            srd_c_decoder_path_add(cs.c_str());
            pxv_info("C decoder path: \"%s\"", cs.c_str());
        }
    }

    // Load the protocol decoders
    if (srd_decoder_load_all() != SRD_OK)
    {
        pxv_err("ERROR: load the protocol decoders failed.");
        return false;
    }
 
    return true;
}

bool AppControl::Start()
{
    _session->Open();

    // Initialize API Service Layer
    _app_service = new pv::api::AppService(this);
    _app_service->initialize();

    _rpc_dispatcher = new pv::api::RpcDispatcher(_app_service);

    _ws_transport = new pv::api::WsTransport(_rpc_dispatcher, 10430);
    _ws_transport->start();

    _mcp_transport = new pv::api::McpTransport(_rpc_dispatcher, 10110);
    _mcp_transport->start();

    auto* active_session = _app_service->get_active_session();
    if (active_session) {
        _direct_transport = new pv::api::DirectTransport(active_session);
        // Session-scoped events (CaptureStateChanged, SampleConfigChanged, ...).
        active_session->add_event_listener(_ws_transport);
        active_session->add_event_listener(_mcp_transport);
    }

    // App-scoped events (DeviceConfigChanged, DeviceDetached, ...).
    // Without this, AppService::_event_listeners stays empty and
    // AppService::notify_event never reaches any transport.
    _app_service->add_event_listener(_ws_transport);
    _app_service->add_event_listener(_mcp_transport);

    return true;
}

 void AppControl::Stop()
 {
    // Cleanup API Service Layer
    if (_ws_transport) { _ws_transport->stop(); delete _ws_transport; _ws_transport = nullptr; }
    if (_mcp_transport) { _mcp_transport->stop(); delete _mcp_transport; _mcp_transport = nullptr; }
    if (_direct_transport) { delete _direct_transport; _direct_transport = nullptr; }
    if (_rpc_dispatcher) { delete _rpc_dispatcher; _rpc_dispatcher = nullptr; }
    if (_app_service) { _app_service->shutdown(); delete _app_service; _app_service = nullptr; }

    _session->Close();
 }

void AppControl::UnInit()
{  
    // Destroy libsigrokdecode
    srd_exit();

    _session->uninit();
}

bool AppControl::TopWindowIsMaximized()
{
    if (_topWindow != NULL){
        return _topWindow->isMaximized();
    }
    return false;
}

pv::api::IAppService* AppControl::GetAppService() {
    return _app_service;
}