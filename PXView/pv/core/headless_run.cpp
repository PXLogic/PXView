/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "pv/core/headless_run.h"

#include <QCoreApplication>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include <ds_types.h>

#include "PXView/config.h"
#include "pv/base/log.h"
#include "pv/config/appconfig.h"
#include "pv/core/appcontrol.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace pv {

#ifdef _WIN32
// Routes every Qt log message to stderr and, on fatal/assert, dumps a raw
// backtrace. Without this, qFatal() in a GUI-subsystem build would abort with
// no diagnostic at all on Windows.
static void pxvMessageHandler(QtMsgType type, const QMessageLogContext &context,
                              const QString &msg)
{
    // Keep the QByteArray alive across the fprintf/strstr calls: msg.toUtf8()
    // returns a temporary, and constData() points into it -- using it after the
    // temporary is destroyed is a use-after-free (page heap ASAN turns it into
    // a startup SIGSEGV). Bind to a named QByteArray first.
    const QByteArray utf8 = msg.toUtf8();
    const char *msg_str = utf8.constData();
    fprintf(stderr, "QtMsg: %s (file: %s, line: %d, function: %s)\n",
            msg_str ? msg_str : "",
            context.file ? context.file : "",
            context.line,
            context.function ? context.function : "");
    fflush(stderr);

    if (type == QtFatalMsg || (msg_str && std::strstr(msg_str, "ASSERT failure"))) {
        fprintf(stderr, "=== FATAL/ASSERT BACKTRACE ===\n");
        fprintf(stderr, "Base of main module: %p\n", GetModuleHandleW(nullptr));
        fprintf(stderr, "Base of Qt6Core.dll: %p\n", GetModuleHandleA("Qt6Core.dll"));

        void *backtrace[64];
        USHORT frames = CaptureStackBackTrace(0, 64, backtrace, nullptr);
        for (USHORT i = 0; i < frames; ++i) {
            fprintf(stderr, "  #%d: %p\n", i, backtrace[i]);
        }
        fprintf(stderr, "==============================\n");
        fflush(stderr);
    }
}
#endif

void install_qt_message_handler()
{
#ifdef _WIN32
    qInstallMessageHandler(pxvMessageHandler);
#endif
}

CommandLine prepare_command_line(int argc, char *argv[])
{
    CommandLine cl;
#ifdef _WIN32
    // Under Windows, manually retrieve the command-line arguments and convert
    // them from UTF-16 to UTF-8. This prevents data loss if there are any
    // characters that wouldn't fit in the local ANSI code page.
    int argcUTF16 = 0;
    LPWSTR *argvUTF16 = CommandLineToArgvW(GetCommandLineW(), &argcUTF16);
    if (argvUTF16 == nullptr) {
        // Extremely unlikely; fall back to the lossy ANSI argv rather than dying.
        cl.argc = argc;
        cl.argv = argv;
        return cl;
    }

    cl.utf8_storage.reserve(static_cast<size_t>(argcUTF16));
    std::for_each(argvUTF16, argvUTF16 + argcUTF16, [&cl](const LPWSTR &arg) {
        cl.utf8_storage.emplace_back(
            QString::fromUtf16(reinterpret_cast<const char16_t *>(arg), -1).toUtf8());
    });
    LocalFree(reinterpret_cast<HLOCAL>(argvUTF16));

    cl.utf8_ptrs.reserve(cl.utf8_storage.size());
    for (auto &arg : cl.utf8_storage) {
        cl.utf8_ptrs.push_back(arg.data());
    }

    cl.argc = argcUTF16;
    cl.argv = cl.utf8_ptrs.data();
    cl.owned = true;
#else
    cl.argc = argc;
    cl.argv = argv;
#endif
    return cl;
}

void print_headless_usage(const char *bin_name)
{
    std::printf(
        "Usage:\n"
        "  %s [OPTION...] - %s\n"
        "\n"
        "Help Options:\n"
        "  -l, --loglevel                  Set log level, value between 0 to 5\n"
        "  -v, -V, --version               Show release version\n"
        "  -s, --storelog                  Save log to locale file\n"
        "  -h, -?, --help                  Show help option\n"
        "\n"
        "Application Options:\n"
        "      --port PORT                 MCP server port (default: 10110)\n"
        "      --ws-port PORT              WebSocket server port (default: 10430)\n"
        "\n",
        bin_name, DS_DESCRIPTION);
}

int run_headless(int argc, char *argv[], const HeadlessOptions &opt)
{
    // Headless mode: no GUI, no fonts, no style, no accessibility.
    // QCoreApplication drives the event loop for the API transports.
    QCoreApplication a(argc, argv);
    QCoreApplication::setApplicationVersion(DS_VERSION_STRING);
    QCoreApplication::setApplicationName(DS_TITLE);
    QCoreApplication::setOrganizationName("PXlogicV20");
    QCoreApplication::setOrganizationDomain("www.marrychip.com");

    //----------------------init log
    pxv_log_init();

    int logLevel = opt.log_level;
    const bool bStoreLog = opt.store_log;

    if (bStoreLog && logLevel < XLOG_LEVEL_DBG) {
        logLevel = XLOG_LEVEL_DBG;
    }
    if (logLevel != -1) {
        pxv_log_level(logLevel);
    }

    if (bStoreLog) {
        pxv_log_enalbe_logfile(true);
    }

    AppControl *control = AppControl::Instance();
    AppConfig &app = AppConfig::Instance();
    app.LoadAll();

    if (app.appOptions.ableSaveLog) {
        pxv_log_enalbe_logfile(app.appOptions.appendLogMode);
        if (app.appOptions.logLevel >= logLevel) {
            pxv_log_level(app.appOptions.logLevel);
        }
    }

    pxv_info("----------------- version: %s (headless)-----------------", DS_VERSION_STRING);
    pxv_info("Qt:%s", QT_VERSION_STR);

    int bit_width = sizeof(u64_t);
    if (bit_width != 8) {
        pxv_err("Can only run on 64 bit systems");
        return 0;
    }

    // init core
    if (!control->Init()) {
        pxv_err("init error!");
        return 1;
    }

    // Set custom API ports before starting services
    control->set_api_ports(opt.mcp_port, opt.ws_port);

    // Start API services
    control->Start();

    pxv_info("Headless mode started. MCP port %d, WS port %d.", opt.mcp_port, opt.ws_port);

    const int ret = a.exec();

    control->Stop();
    control->UnInit();
    control->Destroy();

    pxv_info("Headless mode stopped.");
    pxv_log_uninit();
    return ret;
}

} // namespace pv
