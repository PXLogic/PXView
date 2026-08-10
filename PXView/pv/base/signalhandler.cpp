/*
 * This file is part of the PXView project.
 * PXView is based on PulseView.
 *
 * Crash signal handler implementation.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "signalhandler.h"
#include "pv/base/log.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#include <unistd.h>
#endif

namespace pv {
namespace base {
namespace signalhandler {

static std::mutex g_crash_mutex;
static std::string g_crash_info;
static bool g_installed = false;

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (segmentation fault)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGFPE:  return "SIGFPE (floating-point exception)";
    case SIGILL:  return "SIGILL (illegal instruction)";
#ifdef SIGBUS
    case SIGBUS:  return "SIGBUS (bus error)";
#endif
    default:      return "unknown signal";
    }
}

#ifdef _WIN32
static void write_stack_trace_win(int sig)
{
    std::lock_guard<std::mutex> lock(g_crash_mutex);

    char buf[512];
    snprintf(buf, sizeof(buf), "PXView crashed: %s (signal %d)\n",
             signal_name(sig), sig);
    g_crash_info = buf;
    fputs(buf, stderr);

    // Use SymCapture / CaptureStackBackTrace for a basic backtrace
    void *stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);

    if (frames == 0) {
        fputs("  (no stack frames captured)\n", stderr);
    } else {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);

        SYMBOL_INFO *symbol = (SYMBOL_INFO *)malloc(sizeof(SYMBOL_INFO) + 256);
        if (symbol) {
            symbol->MaxNameLen = 255;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

            for (USHORT i = 0; i < frames; i++) {
                DWORD64 address = (DWORD64)stack[i];
                DWORD displacement = 0;
                IMAGEHLP_LINE64 line = {};
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

                if (SymFromAddr(process, address, nullptr, symbol)) {
                    if (SymGetLineFromAddr64(process, address, &displacement, &line)) {
                        snprintf(buf, sizeof(buf), "  [%3d] %s (%s:%lu+0x%lx)\n",
                                 i, symbol->Name, line.FileName,
                                 (unsigned long)line.LineNumber,
                                 (unsigned long)displacement);
                    } else {
                        snprintf(buf, sizeof(buf), "  [%3d] %s (0x%llx)\n",
                                 i, symbol->Name,
                                 (unsigned long long)address);
                    }
                } else {
                    snprintf(buf, sizeof(buf), "  [%3d] 0x%llx\n",
                             i, (unsigned long long)address);
                }
                g_crash_info += buf;
                fputs(buf, stderr);
            }
            free(symbol);
        }
    }
}
#else
static void write_stack_trace_unix(int sig)
{
    std::lock_guard<std::mutex> lock(g_crash_mutex);

    char buf[512];
    snprintf(buf, sizeof(buf), "PXView crashed: %s (signal %d)\n",
             signal_name(sig), sig);
    g_crash_info = buf;
    fputs(buf, stderr);

    void *stack[64];
    int frames = backtrace(stack, 64);
    if (frames > 0) {
        char **symbols = backtrace_symbols(stack, frames);
        if (symbols) {
            for (int i = 0; i < frames; i++) {
                snprintf(buf, sizeof(buf), "  [%3d] %s\n", i, symbols[i]);
                g_crash_info += buf;
                fputs(buf, stderr);
            }
            free(symbols);
        }
    } else {
        fputs("  (no stack frames captured)\n", stderr);
    }
}
#endif

static void crash_handler(int sig)
{
#ifdef _WIN32
    write_stack_trace_win(sig);
#else
    write_stack_trace_unix(sig);
#endif

    // Re-raise the signal with default handler so the OS can produce
    // a core dump / crash report if enabled.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void install()
{
    if (g_installed)
        return;
    g_installed = true;

    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    std::signal(SIGFPE,  crash_handler);
    std::signal(SIGILL,  crash_handler);
#ifdef SIGBUS
    std::signal(SIGBUS,  crash_handler);
#endif
}

QString last_crash_info()
{
    std::lock_guard<std::mutex> lock(g_crash_mutex);
    return QString::fromStdString(g_crash_info);
}

} // namespace signalhandler
} // namespace base
} // namespace pv
