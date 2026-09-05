/*
 * This file is part of the PXView project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QByteArray>
#include <vector>

namespace pv {

/// Argument vector handed to QCoreApplication / QApplication.
///
/// On Windows the process command line is re-parsed from GetCommandLineW()
/// (UTF-16) instead of trusting the ANSI-code-page `char** argv`, so non-ASCII
/// file paths survive. On other platforms the original argv is passed through
/// untouched and `owned` is false.
struct CommandLine
{
    int    argc = 0;
    char **argv = nullptr;

    /// True when argv points into utf8_ptrs (internally-owned storage that
    /// must outlive every consumer of argv -- i.e. the whole process).
    bool owned = false;

    // Storage keeping the buffers alive; must outlive the consumers of argv.
    std::vector<QByteArray> utf8_storage;
    std::vector<char *>     utf8_ptrs;
};

/// Build the CommandLine for this process (see struct docs).
CommandLine prepare_command_line(int argc, char *argv[]);

/// Tunables for a headless run. Mirrors the command-line options both entry
/// points accept (`--loglevel`, `--storelog`, `--port`, `--ws-port`).
struct HeadlessOptions
{
    /// Log level to apply; -1 means "leave the compiled-in default alone".
    int  log_level = -1;
    /// Mirror log output to the on-disk log file.
    bool store_log = false;
    /// MCP (JSON-RPC over HTTP) listen port.
    int  mcp_port = 10110;
    /// WebSocket listen port.
    int  ws_port = 10430;
};

/// Run PXView without any GUI: start the MCP/WebSocket API transports and
/// block in the Qt event loop until the process is asked to quit.
///
/// This is the single implementation shared by the GUI binary (reached via
/// `PXView --headless`) and the console `pxviewd` binary. Keeping one copy is
/// what guarantees the two entry points cannot drift apart in behaviour.
///
/// Creates a plain QCoreApplication -- no QWidget is ever instantiated and, in
/// the pxviewd link, the Qt Widgets module is not even loaded.
///
/// @param argc/argv forwarded to QCoreApplication (must stay alive for the
///                  lifetime of the call, as Qt keeps references to them).
/// @param opt       resolved options (ports already validated by the caller).
/// @return process exit code.
int run_headless(int argc, char *argv[], const HeadlessOptions &opt);

/// Print the usage text for headless/daemon mode.
/// @param bin_name name the user actually invoked (PXView / pxviewd).
void print_headless_usage(const char *bin_name);

/// Install the Qt message handler that routes qWarning/qFatal to stderr and,
/// on Windows, dumps a backtrace on fatal/assert failures.
///
/// Lives here (rather than in each main()) so the GUI binary and pxviewd share
/// one copy. On non-Windows platforms this is a no-op: Qt's default handler
/// already writes to stderr there.
void install_qt_message_handler();

} // namespace pv
