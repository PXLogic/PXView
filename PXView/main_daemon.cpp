/*
 * pxviewd -- PXView headless daemon entry point.
 *
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * pxviewd is the console-subsystem twin of the PXView GUI binary. It links the
 * exact same pxview-core library (session orchestration, capture, decode, MCP
 * transports) but NOT the Qt Widgets module, and it is built without the
 * Windows "-mwindows" linker flag. That gives it what the GUI binary can never
 * have when driven headlessly:
 *
 *   * stdout/stderr reach the terminal it was started from
 *     (the GUI binary's logs vanish into nowhere on Windows)
 *   * the shell actually waits for it, so `pxviewd &` + kill/$! work
 *   * Ctrl+C / Ctrl+Break deliver a clean shutdown through the Qt event loop
 *
 * The runtime itself is pv::run_headless() in pv/core/headless_run.cpp -- the
 * same function `PXView --headless` uses -- so the two entry points cannot
 * drift apart in behaviour.
 */

#include <getopt.h>
#include <cstdio>
#include <cstdlib>

#include "PXView/config.h"
#include "pv/core/headless_run.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef _WIN32
    pv::install_qt_message_handler();
    // Disable Qt Accessibility: without a GUI there is nothing for UIAutomation
    // to look at, and its probes can stall startup.
    qputenv("QT_ACCESSIBILITY", "0");
#endif

	pv::CommandLine cmdLine = pv::prepare_command_line(argc, argv);
	int argcFinal = cmdLine.argc;
	char **argvFinal = cmdLine.argv;

	pv::HeadlessOptions opt;

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
			opt.log_level = static_cast<int>(strtol(optarg, nullptr, 10));
			break;

		case 's': // the store log flag
			opt.store_log = true;
			break;

		case 1000: // --headless: implied by pxviewd; accepted so scripts that
		           // pass the GUI binary's option set verbatim keep working.
			break;

		case 1001: // MCP port
			opt.mcp_port = static_cast<int>(strtol(optarg, nullptr, 10));
			if (opt.mcp_port <= 0 || opt.mcp_port > 65535) {
				printf("Invalid MCP port: %s\n", optarg);
				return 1;
			}
			break;

		case 1002: // WebSocket port
			opt.ws_port = static_cast<int>(strtol(optarg, nullptr, 10));
			if (opt.ws_port <= 0 || opt.ws_port > 65535) {
				printf("Invalid WS port: %s\n", optarg);
				return 1;
			}
			break;

		case 'V': // version
		case 'v':
			printf("%s %s (headless daemon)\n", DS_TITLE, DS_VERSION_STRING);
			return 0;

		case 'h': // get help
		case '?':
			pv::print_headless_usage("pxviewd");
			return 0;
		}
	}

	if (argcFinal - optind > 1) {
		printf("Only one file can be opened.\n");
		return 1;
	}
	else if (argcFinal - optind == 1) {
		// There is no GUI here to open a document window; the MCP API
		// (load / session tools) is the supported way to feed pxviewd data.
		fprintf(stderr,
		        "pxviewd: positional file argument ignored -- opening documents "
		        "requires the GUI binary; use the MCP API instead.\n");
	}

	return pv::run_headless(argcFinal, argvFinal, opt);
}
