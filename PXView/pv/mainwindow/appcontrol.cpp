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

#include "pv/mainwindow/appcontrol.h"

#include <libsigrok/libsigrok.h>
#include <libsigrokdecode.h>
#include <QDir>
#include <QCoreApplication>
#include <QProcess>
#include <QFile>
#include <QWidget>
#include <QThread>
#include <string>
#include <cstdio>
#include <cassert>
#include "pv/session/sigsession.h"
#include "pv/base/pxvdef.h"
#include "pv/config/appconfig.h"
#include "pv/base/log.h"
#include "pv/utility/path.h"
#include "pv/utility/encoding.h"
#include "pv/data/snapshot/leaf_block_pool.h"
#include "pv/api/iapp_service.h"
#include "pv/api/app_service.h"
#include "pv/api/rpc_dispatcher.h"
#include "pv/api/ws_transport.h"
#include "pv/api/mcp_transport.h"
#include "pv/api/direct_transport.h"

AppControl::AppControl()
{
    _topWindow = nullptr; 
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
    static AppControl *ins = nullptr;
    if (ins == nullptr){
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
    pxv_info("DBG: srd_log_set_context done");

#if defined(_WIN32)
    // Set Python home to application directory for embedded Python.
    //
    // Two layouts are supported:
    // 1. Legacy: python3XX.zip in the app directory (python.org embeddable)
    // 2. MSYS2: lib/python3.X/ stdlib copied from MinGW (no zip file)
    //
    // For layout 2, PYTHONHOME must point to the app directory itself
    // (the parent of lib/), so Python finds lib/python3.X/encodings/__init__.py
    QString pythonHome = QCoreApplication::applicationDirPath();
    QDir pydir(pythonHome);
    bool pyHomeSet = false;

    // Method 1: Check for python3XX.zip (legacy embeddable layout)
    QStringList zipFiles = pydir.entryList(QStringList() << "python*.zip", QDir::Files);
    if (!zipFiles.isEmpty()) {
        const wchar_t *pyhome = reinterpret_cast<const wchar_t*>(pythonHome.utf16());
        srd_set_python_home(pyhome);
        pxv_info("Set Python home to: %s (python*.zip detected)", pythonHome.toUtf8().data());
        pyHomeSet = true;
    }

    // Method 2: Check for lib/python3.X/ directory (MSYS2 MinGW stdlib layout)
    if (!pyHomeSet) {
        QDir libDir(pythonHome + "/lib");
        QStringList pyDirs = libDir.entryList(QStringList() << "python3.*", QDir::Dirs, QDir::Name);
        if (!pyDirs.isEmpty()) {
            // Verify encodings module exists (critical for Python startup)
            if (libDir.exists(pyDirs.first() + "/encodings")) {
                const wchar_t *pyhome = reinterpret_cast<const wchar_t*>(pythonHome.utf16());
                srd_set_python_home(pyhome);
                pxv_info("Set Python home to: %s (lib/%s/ detected, encodings OK)",
                         pythonHome.toUtf8().data(), pyDirs.first().toUtf8().data());
                pyHomeSet = true;
            } else {
                pxv_info("WARNING: lib/%s/ found but encodings module missing",
                         pyDirs.first().toUtf8().data());
            }
        }
    }

    if (!pyHomeSet) {
        pxv_info("WARNING: No Python stdlib found in app directory, using system Python");
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
#elif defined(__linux__)
    // Linux/AppImage: 把 Python home 指向 AppImage 内打包的标准库所在的 prefix 目录
    // Python 的 home(对应 PYTHONHOME) 应该是包含 lib/pythonX.Y 的目录(prefix),
    // 而不是 lib/pythonX.Y 本身。设错会导致 PyConfig_Read 找不到 os.py/encodings
    // 报 "memory allocation failed" 或 "ModuleNotFoundError: No module named 'encodings'"
    //
    // 注意 wchar_t 在 Linux 上是 4 字节(UTF-32),不能用 QString::utf16()(返回 char16_t*)
    // reinterpret_cast 强转 —— 那只是把 2 字节数据当 4 字节读,会得到垃圾字符。
    // 必须用 toWString() 做真正的编码转换。
    {
        QString appDir = QCoreApplication::applicationDirPath();
        // AppImage: applicationDirPath() = /tmp/.mount_XXX/usr/bin
        // Python stdlib 在 /tmp/.mount_XXX/usr/lib/python3.x
        // Python home 应该是 /tmp/.mount_XXX/usr (prefix,lib 的父目录)
        QDir libDir(appDir + "/../lib");
        QStringList pyDirs = libDir.entryList(QStringList() << "python3.*", QDir::Dirs, QDir::Name);
        if (!pyDirs.isEmpty()) {
            // home = lib 目录的父目录 (即 usr/),Python 会自动找 <home>/lib/pythonX.Y
            QString pyHome = libDir.absoluteFilePath(pyDirs.first() + "/../..");
            QDir homeDir(pyHome);
            pyHome = homeDir.absolutePath();
            // toWString() 在 Linux 上返回 std::wstring(4 字节 wchar_t),与 Python 兼容
            // .c_str() 返回的指针在临时对象销毁后失效,所以用 static 保持生命周期
            static std::wstring pyHomeW = pyHome.toStdWString();
            srd_set_python_home(pyHomeW.c_str());
            pxv_info("Set Python home to: %s", pyHome.toUtf8().data());
        } else {
            pxv_info("Python stdlib not bundled, using system Python");
        }
    }
#elif defined(Q_OS_DARWIN)
    // macOS: .app bundle 内的 Python.framework 由 macdeployqt 打包。
    // Homebrew Python.framework 结构:
    //   Python.framework/Versions/3.13/
    //     Python               ← 共享库 (libpython3.13.dylib)
    //     Resources/           ← Python.app + Info.plist (不是 stdlib!)
    //     lib/python3.13/      ← stdlib (encodings/, os.py, ...)
    // PYTHONHOME 应指向 Versions/Current (包含 lib/python3.X 的目录)。
    // 设错(如指向 Resources)会导致 "Failed to import encodings module"。
    // wchar_t 在 macOS 上是 4 字节(UTF-32),必须用 toStdWString()。
    {
        QString appDir = QCoreApplication::applicationDirPath();
        // appDir = .../PXView.app/Contents/MacOS
        // frameworkDir = .../PXView.app/Contents/Frameworks
        QString frameworkDir = appDir + "/../Frameworks";
        QDir fwDir(frameworkDir);

        // 查找 Python.framework (macdeployqt 打包为 Python.framework)
        if (fwDir.cd("Python.framework")) {
            // Python home = .../Python.framework/Versions/Current
            // (Current 是符号链接,指向如 3.13,包含 lib/python3.X/)
            QString pyHome = fwDir.absolutePath() + "/Versions/Current";
            QDir homeDir(pyHome);
            pyHome = homeDir.absolutePath();

            // 验证 stdlib 是否存在: lib/python3.X/encodings/
            // 用 glob 匹配 python3.* (与 Linux 代码相同的方式)
            QDir libCheck(pyHome + "/lib");
            QStringList pyDirs = libCheck.entryList(
                QStringList() << "python3.*", QDir::Dirs, QDir::Name);
            if (!pyDirs.isEmpty()) {
                if (QDir(pyHome + "/lib/" + pyDirs.first() + "/encodings").exists()) {
                    static std::wstring pyHomeW = pyHome.toStdWString();
                    srd_set_python_home(pyHomeW.c_str());
                    pxv_info("Set Python home to: %s", pyHome.toUtf8().data());
                } else {
                    pxv_warn("Python.framework found, encodings module missing, using system Python");
                }
            } else {
                // Current 符号链接可能不存在或已损坏,直接扫描 Versions/ 目录
                QDir versionsDir(fwDir.absolutePath() + "/Versions");
                QStringList verDirs = versionsDir.entryList(
                    QStringList() << "3.*", QDir::Dirs, QDir::Name);
                if (!verDirs.isEmpty()) {
                    QString realHome = versionsDir.absoluteFilePath(verDirs.first());
                    QDir realLibCheck(realHome + "/lib");
                    QStringList realPyDirs = realLibCheck.entryList(
                        QStringList() << "python3.*", QDir::Dirs, QDir::Name);
                    if (!realPyDirs.isEmpty() &&
                        QDir(realHome + "/lib/" + realPyDirs.first() + "/encodings").exists()) {
                        static std::wstring pyHomeW = realHome.toStdWString();
                        srd_set_python_home(pyHomeW.c_str());
                        pxv_info("Set Python home (%s) to: %s",
                                 verDirs.first().toUtf8().data(), realHome.toUtf8().data());
                    } else {
                        pxv_warn("Python.framework Versions/%s found but stdlib missing, using system Python",
                                 verDirs.first().toUtf8().data());
                    }
                } else {
                    pxv_warn("Python.framework found but no version dir with stdlib, using system Python");
                }
            }
        } else {
            // 开发模式: 没有 .app bundle,使用系统 Python (Homebrew)
            // 查找 Homebrew Python 的 prefix
            QString brewPython = "/opt/homebrew/bin/python3"; // Apple Silicon
            if (!QFile::exists(brewPython)) {
                brewPython = "/usr/local/bin/python3"; // Intel
            }
            if (QFile::exists(brewPython)) {
                QProcess proc;
                proc.start(brewPython, QStringList() << "-c"
                    << "import sys; print(sys.prefix)");
                if (proc.waitForFinished(5000)) {
                    QString prefix = proc.readAllStandardOutput().trimmed();
                    if (!prefix.isEmpty()) {
                        static std::wstring pyHomeW = prefix.toStdWString();
                        srd_set_python_home(pyHomeW.c_str());
                        pxv_info("Set Python home (Homebrew) to: %s", prefix.toUtf8().data());
                    }
                }
            } else {
                pxv_info("No bundled Python.framework, no Homebrew Python, using system default");
            }
        }
    }
#endif
    
    //the python script path of decoder
    char path[256] = {0};
    QString dir = GetDecodeScriptDir();   
    snprintf(path, sizeof(path), "%s", dir.toUtf8().constData());

    // Initialise libsigrokdecode
    pxv_info("DBG: before srd_init, path=%s", path);
    if (srd_init(path) != SRD_OK)
    {
        pxv_err("ERROR: libsigrokdecode init failed.");
        return false;
    }
    pxv_info("DBG: srd_init done");

    // Add C decoder search paths
    {
        QString cDecDir = GetAppDataDir();
        QDir cDecPath(cDecDir);
        if (cDecPath.cd("c_decoders") || cDecPath.cd("../libsigrokdecode/c_decoders")) {
            // Only add the directory if it actually contains decoder DLLs/SOs.
            // When launched from build.dir, GetAppDataDir() falls back to the
            // exe dir and "../libsigrokdecode/c_decoders" resolves to the SOURCE
            // tree (no DLLs) — adding it would poison the C decoder search path.
#ifdef _WIN32
            const QStringList filter = QStringList() << "*.dll";
#else
            const QStringList filter = QStringList() << "*.so";
#endif
            if (!cDecPath.entryList(filter).isEmpty()) {
                std::string cs = pv::path::ConvertPath(cDecPath.absolutePath());
                srd_c_decoder_path_add(cs.c_str());
                pxv_info("C decoder path: \"%s\"", cs.c_str());
            }
        }
    }

    // Load the protocol decoders
    pxv_info("DBG: before srd_decoder_load_all");
    if (srd_decoder_load_all() != SRD_OK)
    {
        pxv_err("ERROR: load the protocol decoders failed.");
        return false;
    }
    pxv_info("DBG: srd_decoder_load_all done");
 
    return true;
}

bool AppControl::Start()
{
    _session->Open();

    // Initialize API Service Layer
    _app_service = new pv::api::AppService(this);
    _app_service->initialize();

    _rpc_dispatcher = new pv::api::RpcDispatcher(_app_service);

    // Create dedicated IO thread for network transports.
    // CRITICAL: moveToThread must happen BEFORE start() is called, so that
    // QTcpServer/QWebSocketServer/QTimer are created on the IO thread and
    // new connections inherit the IO thread's affinity. start() is invoked
    // via post_to (QCoreApplication::postEvent) which is safe from the main
    // thread — the event is processed on the IO thread's event loop.
    _io_thread = new QThread();
    _io_thread->start();

    _ws_transport = new pv::api::WsTransport(_rpc_dispatcher, _ws_port);
    _mcp_transport = new pv::api::McpTransport(_rpc_dispatcher, _mcp_port);

    _ws_transport->moveToThread(_io_thread);
    _mcp_transport->moveToThread(_io_thread);

    // Start on IO thread — post_to queues the start() call on the IO
    // thread's event loop. The IO thread processes it when it pumps events.
    // Capture the transport pointer by value so the lambda does not re-read
    // the AppControl member on the IO thread (TSan race + use-after-free
    // hardening); the pointer value is published via the AsyncEvent
    // release/acquire handshake.
    pv::core::QtAsyncDispatcher::post_to(_ws_transport,
        [ws = _ws_transport]() { ws->start(); });
    pv::core::QtAsyncDispatcher::post_to(_mcp_transport,
        [mcp = _mcp_transport]() { mcp->start(); });

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
    // Stop transports on the IO thread (they live there).
    // post_to() queues stop() before quit(), so the IO thread processes
    // stop() first, then exits the event loop.
    if (_ws_transport)
        pv::core::QtAsyncDispatcher::post_to(_ws_transport,
            [ws = _ws_transport]() { ws->stop(); });
    if (_mcp_transport)
        pv::core::QtAsyncDispatcher::post_to(_mcp_transport,
            [mcp = _mcp_transport]() { mcp->stop(); });

    // Quit IO thread event loop and wait for it to finish.
    // The quit event is queued after the stop() events, so stop() runs
    // first, then the event loop exits.
    if (_io_thread) {
        _io_thread->quit();
        _io_thread->wait();
    }

    // Now safe to delete transports (IO thread is stopped — QObject
    // can be deleted from any thread after its thread is stopped).
    // The destructors call stop() again, but it's a no-op (already stopped).
    if (_ws_transport) { delete _ws_transport; _ws_transport = nullptr; }
    if (_mcp_transport) { delete _mcp_transport; _mcp_transport = nullptr; }
    if (_direct_transport) { delete _direct_transport; _direct_transport = nullptr; }
    if (_rpc_dispatcher) { delete _rpc_dispatcher; _rpc_dispatcher = nullptr; }
    if (_app_service) { _app_service->shutdown(); delete _app_service; _app_service = nullptr; }

    if (_io_thread) { delete _io_thread; _io_thread = nullptr; }

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
    if (_topWindow != nullptr){
        return _topWindow->isMaximized();
    }
    return false;
}

pv::api::IAppService* AppControl::GetAppService() {
    return _app_service;
}