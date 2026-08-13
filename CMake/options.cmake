#===============================================================================
#= User Options
#-------------------------------------------------------------------------------

set(DISABLE_WERROR TRUE) #Build without -Werror
set(ENABLE_SIGNALS TRUE) #Build with UNIX signals
set(ENABLE_COTIRE FALSE) #Enable cotire
option(CMAKE_UNITY_BUILD "Enable unity build (batch compilation) for faster builds" OFF)
option(ENABLE_PCH "Enable precompiled headers" ON)
set(ENABLE_TESTS  FALSE) #Enable unit tests (QTest + CTest). Use -DENABLE_TESTS=ON to enable.
set(STATIC_PKGDEPS_LIBS FALSE) #Statically link to (pkg-config) libraries

if(WIN32)
	# On Windows/MinGW we need to statically link to libraries.
	# This option is user configurable, but enable it by default on win32.
	set(STATIC_PKGDEPS_LIBS TRUE)

	# Windows does not support UNIX signals.
	set(ENABLE_SIGNALS FALSE)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
      "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
endif()

#===============================================================================
#= Sanitizer + Static Analysis Options (Debug mode)
#-------------------------------------------------------------------------------
# Usage:
#   cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON   ..
#   cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON   ..   (Linux/macOS only)
#   cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_UBSAN=ON  ..
#   cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_CLANG_TIDY=ON ..
#
# Combinations:
#   ASan + UBSan: OK (both are compatible)
#   ASan + TSan:  FATAL ERROR (mutually exclusive)
#   TSan + UBSan: OK
#   Clang-Tidy:   independent (static analysis, no runtime overhead)
#
# Platform constraints:
#   Windows/MinGW: ASan ✓, UBSan ✓, TSan ✗ (not supported)
#   Linux:         ASan ✓, UBSan ✓, TSan ✓
#   macOS:         ASan ✓, UBSan ✓, TSan ✓
#-------------------------------------------------------------------------------
option(ENABLE_ASAN      "Enable AddressSanitizer (UAF, buffer overflow, etc.)" OFF)
option(ENABLE_TSAN      "Enable ThreadSanitizer (data race detection)" OFF)
option(ENABLE_UBSAN     "Enable UndefinedBehaviorSanitizer (UB detection)" OFF)
option(ENABLE_CLANG_TIDY "Enable Clang-Tidy static analysis (requires clang-tidy in PATH)" OFF)

#===============================================================================
#= decoder_test option
#-------------------------------------------------------------------------------
option(BUILD_DECODER_TEST "Build the C decoder test program" OFF)

#===============================================================================
#= Tauri desktop wrapper option
#-------------------------------------------------------------------------------
# Builds the PXView Agent desktop wrapper (Tauri v2 + React/Vite).
# The Tauri binary wraps the web UI and spawns PXView --headless on startup.
# Requires Rust toolchain (rustc, cargo) and Node.js (npm) in PATH.
# Platform WebView runtime requirements:
#   Windows: WebView2 Runtime (preinstalled on Win10/11, or download bootstrapper)
#   Linux:   webkit2gtk-4.1, libgtk-3, librsvg
#   macOS:   WKWebView (system framework, no extra deps)
#-------------------------------------------------------------------------------
#
#= Windows platform compatibility
#-------------------------------------------------------------------------------
# PXView has deeply integrated Qt6, which uses WinRT API sets (Win8+) as hard
# imports. Win7 SP1 compatibility is therefore NOT supported and will not be
# pursued. The effective minimum is Windows 10 1809 (matching WebView2).
#
# Technical note: The MinGW-w64 UCRT64 toolchain links against msvcrt.dll
# (available since Win95), NOT ucrtbase.dll, so no UCRT runtime bundling is
# needed. The Win7 blocker is Qt6Core.dll's WinRT API imports, not UCRT.
#-------------------------------------------------------------------------------
option(ENABLE_TAURI "Build the Tauri desktop wrapper (PXView Agent)" ON)
