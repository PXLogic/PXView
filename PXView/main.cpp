/*
 * This file is part of the PXView project.
 * PXView is based on DSView.
 * PXView is based on PulseView.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2013 DreamSourceLab <dreamsourcelab@dreamsourcelab.com>
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
 
#include <cstdint>
#include <getopt.h>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QStyle>
#include <QGuiApplication>
#include <QAccessible>
#include <QScreen>
#include "application.h"
#include "mystyle.h"
#include "pv/mainwindow/mainframe.h"
#include "pv/mainwindow/mainwindow.h"
#include "pv/config/appconfig.h"
#include "PXView/config.h"
#include "pv/core/appcontrol.h"
#include "pv/core/headless_run.h"
#include "pv/core/ui_hooks.h"
#include "pv/api/iapp_service.h"
#include "pv/api/isession_service.h"
#include "pv/base/log.h"
#include "pv/dock/logdock.h"
#include "pv/ui/msgbox.h"
#include "pv/core/langresource.h"
#include <QDateTime>
#include <string>
#include <ds_types.h>
#include <QFontDatabase>
#include <QFont>
#include <QTimer>

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>

#endif


void usage()
{
	printf(
		"Usage:\n"
		"  %s [OPTION...] [FILE] - %s\n"
		"\n"
		"Help Options:\n"
		"  -l, --loglevel                  Set log level, value between 0 to 5\n"
		"  -v, -V, --version               Show release version\n"
		"  -s, --storelog                  Save log to locale file\n"
		"  -h, -?, --help                  Show help option\n"
		"\n"
		"Application Options:\n"
		"      --headless                  Run in headless mode (no GUI, MCP/WS API only)\n"
		"      --port PORT                 MCP server port (default: 10110)\n"
		"      --ws-port PORT              WebSocket server port (default: 10430)\n"
		"\n"
		"NOTE:\n"
		"  For headless/CLI use prefer the pxviewd companion binary: it is a\n"
		"  console application, so its log output reaches the terminal and\n"
		"  Ctrl+C shuts it down cleanly. `PXView --headless` stays supported\n"
		"  for scripting that launches the GUI binary in the background.\n"
		"\n", DS_BIN_NAME, DS_DESCRIPTION);
}

int main(int argc, char *argv[])
{
#ifdef _WIN32
    pv::install_qt_message_handler();
    // Disable Qt Accessibility to prevent UIAutomation from stalling the main thread during high-frequency data updates
    qputenv("QT_ACCESSIBILITY", "0");
	    // Force FreeType font engine instead of DirectWrite/GDI.
    // ATK uses QML Software Scene Graph which inherently uses FreeType/Grayscale.
    // This perfectly aligns the QWidget text rendering with ATK, ensuring zero color fringes
    // and strict pixel alignment.
    qputenv("QT_QPA_PLATFORM", "windows:fontengine=freetype");

#endif

	int ret = 0;
	const char *open_file = nullptr;
	int logLevel = -1;
	bool bStoreLog = false;
	bool bHeadless = false;
	int mcpPort = 10110;
	int wsPort = 10430;

	//----------------------rebuild command param
	// On Windows this re-parses GetCommandLineW() (UTF-16) so non-ASCII paths
	// survive; see pv::prepare_command_line(). cmdLine storage must outlive
	// every consumer of argvFinal, so it stays a stack local of main().
	pv::CommandLine cmdLine = pv::prepare_command_line(argc, argv);
	int argcFinal = cmdLine.argc;
	char** argvFinal = cmdLine.argv;
 
	//----------------------command param parse
	while (1) {
		static const struct option long_options[] = {
			{"loglevel", required_argument, 0, 'l'},
			{"version", no_argument, 0, 'v'},
			{"storelog", no_argument, 0, 's'},
			{"help", no_argument, 0, 'h'},
		{"headless", no_argument, 0, 1000},
		{"port", required_argument, 0, 1001},
		{"ws-port", required_argument, 0, 1002},
		{0, 0, 0, 0}
		};

        const char *shortopts = "l:Vvhs?";
        const int c = getopt_long(argcFinal, argvFinal, shortopts, long_options, nullptr);
		if (c == -1)
			break;

		switch (c)
		{
		case 'l': // log level
			logLevel = static_cast<int>(strtol(optarg, nullptr, 10));
			break;

		case 's': // the store log flag
			bStoreLog = true;
			break;

		case 1000: // headless mode
			bHeadless = true;
			break;

		case 1001: // MCP port
			mcpPort = static_cast<int>(strtol(optarg, nullptr, 10));
			if (mcpPort <= 0 || mcpPort > 65535) {
				printf("Invalid MCP port: %s\n", optarg);
				return 1;
			}
			break;

		case 1002: // WebSocket port
			wsPort = static_cast<int>(strtol(optarg, nullptr, 10));
			if (wsPort <= 0 || wsPort > 65535) {
				printf("Invalid WS port: %s\n", optarg);
				return 1;
			}
			break;

		case 'V': // version
		case 'v':
			printf("%s %s\n", DS_TITLE, DS_VERSION_STRING);
			return 0;

		case 'h': // get help
		case '?':
			usage();
			return 0;
		}
	}

	if (argcFinal - optind > 1) {
		printf("Only one file can be openened.\n");
		return 1;
    } 
	else if (argcFinal - optind == 1){
        open_file = argvFinal[argcFinal - 1];		
	}

	//----------------------init app
	//
	// In headless mode we create a plain QCoreApplication (no GUI) so that
	// the MCP / WebSocket API layer can be driven by external clients
	// (LLM agents, web UIs, automation scripts) without instantiating any
	// QWidget or pulling in the Qt Widgets module at runtime.
	//
	if (bHeadless) {
		// Headless mode: no GUI, no fonts, no style, no accessibility.
		// QCoreApplication drives the event loop for the API transports.
		// The runtime itself lives in pv::run_headless() (core layer) and is
		// shared with the console pxviewd binary, so both entry points stay
		// behaviourally identical.
		pv::HeadlessOptions opt;
		opt.log_level = logLevel;
		opt.store_log = bStoreLog;
		opt.mcp_port  = mcpPort;
		opt.ws_port   = wsPort;
		return pv::run_headless(argcFinal, argvFinal, opt);
	}

	//----------------------GUI mode
    Application a(argcFinal, argvFinal);
#ifdef _WIN32
    QAccessible::setActive(false);
#endif

	// Bind the GUI services the Core layer may need (see pv/core/ui_hooks.h).
	// Headless processes never install these and get the documented no-UI
	// fallbacks (cancelled dialog / log-only messages).
	pv::set_ask_save_file_hook(
		[](const QString &caption, const QString &dir, const QString &filter,
		   QString *selected_filter) -> QString {
			return QFileDialog::getSaveFileName(nullptr, caption, dir, filter,
			                                    selected_filter);
		});
	pv::set_notify_user_hook([](const QString &message) {
		MsgBox::Show(message);
	});
    a.setStyle(new MyStyle);

    QFont font = a.font();
    QFontDatabase fontDb;
    int fontId = fontDb.addApplicationFont(":/fonts/SourceHanSansCN-Regular.otf");
    if (fontId != -1) {
        QStringList fontFamilies = fontDb.applicationFontFamilies(fontId);
        if (!fontFamilies.isEmpty()) {
            // Use PreferNoHinting with FreeType to ensure smooth grayscale antialiasing.
            // PreferVerticalHinting can cause FreeType to aggressively snap and disable antialiasing for some font sizes.
            font.setHintingPreference(QFont::PreferVerticalHinting);
            font.setStyleStrategy(QFont::PreferAntialias);
			font.setFamily(fontFamilies.at(0));
            font.setPixelSize(12); // ATK uses exactly 12px for its global base (like MenuBar)
            a.setFont(font);
        }
    }
    fontDb.addApplicationFont(":/fonts/OPPOSans-M.ttf");
    fontDb.addApplicationFont(":/fonts/SourceCodePro-Medium.ttf");

    // Set some application metadata
    QApplication::setApplicationVersion(DS_VERSION_STRING);
    // QApplication::setApplicationName("PXView");
	QApplication::setApplicationName(DS_TITLE);
    QApplication::setOrganizationName("PXlogicV20");
    QApplication::setOrganizationDomain("www.marrychip.com");

	//----------------------init log
	pxv_log_init(); // Don't call before QApplication be inited

	// Register the in-memory log buffer receiver early so that ALL startup
	// logs (device scan, config load, etc.) are captured in LogDock::_log_buffer
	// before the LogDock widget is constructed.  This avoids reading the log
	// file from disk at LogDock construction time.
	pv::dock::LogDock::init_log_receiver();


	if (bStoreLog && logLevel < XLOG_LEVEL_DBG){
		logLevel = XLOG_LEVEL_DBG;
	}
	if (logLevel != -1){
		pxv_log_level(logLevel);
	}

	#ifdef DEBUG_INFO
		if (XLOG_LEVEL_INFO > logLevel){
			pxv_log_level(XLOG_LEVEL_INFO); // on develop mode, set the default log level
			logLevel = XLOG_LEVEL_INFO;
		}
	#endif

	if (bStoreLog){
		pxv_log_enalbe_logfile(true);
	} 

	AppControl *control = AppControl::Instance();	
	AppConfig &app = AppConfig::Instance(); 
	app.LoadAll(); //load app config

	LangResource::Instance()->Load(app.frameOptions.language);

	if (app.appOptions.ableSaveLog){
		pxv_log_enalbe_logfile(app.appOptions.appendLogMode);

		if (app.appOptions.logLevel >= logLevel){
			pxv_log_level(app.appOptions.logLevel);
		}
	}

	//----------------------run
	pxv_info("----------------- version: %s-----------------", DS_VERSION_STRING);
	pxv_info("Qt:%s", QT_VERSION_STR);

	QDateTime dateTime = QDateTime::currentDateTime();
	std::string strTime = dateTime .toString("yyyy-MM-dd hh:mm:ss").toStdString();
	pxv_info("%s", strTime.c_str());

	int bit_width = sizeof(u64_t);
	if (bit_width != 8){
		pxv_err("Can only run on 64 bit systems");
		return 0;
	}
 
	//init core
	pxv_info("DBG: before control->Init()");
	if (!control->Init()){
		pxv_err("init error!");
		return 1;
	}
	pxv_info("DBG: control->Init() done");

	if (open_file != nullptr){
		control->_open_file_name = open_file;
	}	

	try
	{
		pxv_info("DBG: before MainFrame construction");
		pv::MainFrame w;
		pxv_info("DBG: MainFrame constructed");
		control->set_api_ports(mcpPort, wsPort);
		control->Start();
		pxv_info("DBG: control->Start() done");

		// Register MainWindow as an IServiceEventListener so that View
		// operation broadcasts (show_region, zoom_fit, zoom_in/out, cursors)
		// from SessionService are routed to the active View in GUI mode.
		{
			auto *app_svc = control->GetAppService();
			if (app_svc) {
				auto *session_svc = app_svc->get_active_session();
				if (session_svc) {
					auto *mw = static_cast<pv::MainWindow *>(w.GetMainWindow());
					session_svc->add_event_listener(mw);
				}
			}
		}

		w.ShowFormInit();
		w.ShowHelpDocAsync();  //to show the dailog for open help document


		ret = a.exec(); //Run the application

		// Unregister MainWindow before stopping
		{
			auto *app_svc = control->GetAppService();
			if (app_svc) {
				auto *session_svc = app_svc->get_active_session();
				if (session_svc) {
					auto *mw = static_cast<pv::MainWindow *>(w.GetMainWindow());
					session_svc->remove_event_listener(mw);
				}
			}
		}

		control->Stop();

		pxv_info("Main window closed.");
	}
	catch (const std::exception &e)
	{
        pxv_err("main() catch a except!");
		const char *exstr = e.what();
		pxv_err("%s", exstr);
	}

	control->UnInit();  //uninit
	control->Destroy();

	pxv_info("Uninit log.");

	pxv_log_uninit();
 
	return ret;
}
