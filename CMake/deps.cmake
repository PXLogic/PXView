#===============================================================================
#= first search path
#-------------------------------------------------------------------------------
include_directories(
   ./PXView
   ./libsigrok/include
   ./libsigrokdecode
   ./common
)

#===============================================================================
#= UCRT64 prefix detection (needed by both glib path fix and Python3)
#-------------------------------------------------------------------------------
set(_ucrt64_prefix "")
# Detect UCRT64 prefix on Windows. CMake's EXISTS uses Windows paths,
# so /ucrt64 (MSYS2 internal path) won't work outside the MSYS2 shell.
if(EXISTS "/ucrt64/bin")
	# Inside MSYS2 shell — Unix-style path works
	set(_ucrt64_prefix "/ucrt64")
elseif(EXISTS "C:/msys64/ucrt64/bin")
	# Windows native — common MSYS2 installation path
	set(_ucrt64_prefix "C:/msys64/ucrt64")
else()
	# Try to derive from MSYSTEM_PREFIX or C compiler path
	if(DEFINED ENV{MSYSTEM_PREFIX} AND EXISTS "$ENV{MSYSTEM_PREFIX}/bin")
		set(_ucrt64_prefix "$ENV{MSYSTEM_PREFIX}")
	elseif(CMAKE_C_COMPILER)
		get_filename_component(_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
		get_filename_component(_cc_parent "${_cc_dir}" DIRECTORY)
		if(EXISTS "${_cc_parent}/bin/python3.exe")
			set(_ucrt64_prefix "${_cc_parent}")
		endif()
	endif()
endif()

#===============================================================================
#= glib-2.0
#-------------------------------------------------------------------------------
pkg_search_module(GLIB glib-2.0)

if(NOT GLIB_FOUND)
	message(FATAL_ERROR  "Please install glib!")
endif()

message("----- glib-2.0:")
message(STATUS "	 includes:" ${GLIB_INCLUDE_DIRS})
message(STATUS "	 libraries:" ${GLIB_LIBDIR}/libglib-2.0.*)
# FindPkgConfig (pkg_search_module) on this setup can emit MSYS-style paths
# (/ucrt64/...) which the native Windows compiler cannot resolve. Convert any
# such prefix back to the real MSYS2 install path when detected.
if(CMAKE_HOST_WIN32 AND _ucrt64_prefix AND GLIB_INCLUDE_DIRS MATCHES "^/ucrt64/")
	string(REPLACE "/ucrt64/" "${_ucrt64_prefix}/" GLIB_INCLUDE_DIRS "${GLIB_INCLUDE_DIRS}")
	string(REPLACE "/ucrt64/" "${_ucrt64_prefix}/" GLIB_LIBDIR "${GLIB_LIBDIR}")
	message(STATUS "	 [fixed] glib paths -> ${GLIB_INCLUDE_DIRS}")
endif()
include_directories(${GLIB_INCLUDE_DIRS})
link_directories(${GLIB_LIBDIR})

#===============================================================================
#= python3
#-------------------------------------------------------------------------------
# On MSYS2/MinGW (GitHub Actions windows-2022 runners pre-install python.org
# MSVC Python at C:/hostedtoolcache/), CMake's find_package(Python3) would find
# the MSVC build first (via registry + PATH), linking PXView.exe against
# python314.dll (MSVC naming). The MinGW build produces libpython3.14.dll
# instead. Mixing them causes "python314.dll not found" at runtime because
# package.sh only copies MinGW DLLs.
#
# Fix: detect UCRT64 prefix directly (not just $ENV{MSYSTEM}) so this works
# even when running CMake from PowerShell with UCRT64 tools in PATH.
# Use standard GIL Python (not free-threaded) — MSYS2's free-threaded Python
# (python3.14t) has broken sys.path and ABI conflicts in extension modules.

if(_ucrt64_prefix)
	set(Python3_FIND_REGISTRY NEVER)
	if(EXISTS "${_ucrt64_prefix}/bin/python3.exe")
		set(Python3_ROOT_DIR "${_ucrt64_prefix}")
		set(_py_source "ucrt64-standard")
		message(STATUS "UCRT64 standard Python detected")
	endif()
endif()

if(NOT Python3_FOUND)
	find_package(Python3 COMPONENTS Interpreter Development)
endif()

if (Python3_FOUND)
	message("----- python3:")
	message(STATUS "	 includes:" ${Python3_INCLUDE_DIRS})
	message(STATUS "	 libraries:" ${Python3_LIBRARIES})
	# Diagnostic: verify we got the MinGW Python, not python.org MSVC build
	if(_ucrt64_prefix)
		get_filename_component(_py_lib_dir "${Python3_LIBRARIES}" DIRECTORY)
		if(_py_lib_dir MATCHES "hostedtoolcache|Python3[0-9]+\\\\libs|Program Files")
			message(FATAL_ERROR "FATAL: CMake found python.org MSVC Python at ${Python3_LIBRARIES} "
				"instead of MinGW Python. PXView.exe would link against python314.dll "
				"(MSVC build) which is not available at runtime. "
				"Check Python3_ROOT_DIR=${Python3_ROOT_DIR}")
		endif()
		message(STATUS "	 [OK] MinGW Python confirmed (source: ${_py_source})")
	endif()
 include_directories(${Python3_INCLUDE_DIRS})
	set(PY_LIB ${Python3_LIBRARIES})

	# Detect free-threaded Python (PEP 703, Python 3.13t+).
	# Py_GIL_DISABLED is defined by Python.h when the GIL is disabled.
	# Our C code uses #ifdef Py_GIL_DISABLED to switch between explicit
	# locking (free-threaded) and GIL-reliant code paths.
	include(CheckCSourceCompiles)
	set(CMAKE_REQUIRED_INCLUDES "${Python3_INCLUDE_DIRS}")
	check_c_source_compiles("
		#include <Python.h>
		#if !defined(Py_GIL_DISABLED)
		#error GIL not disabled
		#endif
		int main(void) { return 0; }"
		SRD_FREE_THREADED_PYTHON)
	set(CMAKE_REQUIRED_INCLUDES)
	if(SRD_FREE_THREADED_PYTHON)
		message(STATUS "	 [OK] Free-threaded Python detected (PEP 703) — decoders run truly parallel")
		add_compile_definitions(SRD_FREE_THREADED_PYTHON=1)
	else()
		message(STATUS "	 [OK] Standard GIL Python — Python decoders serialised by GIL, C decoders parallel")
	endif()
else()
	message(FATAL_ERROR  "Please install lib python3!")
endif()

#===============================================================================
#= FFTW
#-------------------------------------------------------------------------------
find_package(FFTW)

if(NOT FFTW_FOUND)
	message(FATAL_ERROR  "Please install lib fftw!")
endif()

message("----- FFTW:")
message(STATUS "	 includes:" ${FFTW_INCLUDE_DIRS})
message(STATUS "	 libraries:" ${FFTW_LIBRARIES})
include_directories(${FFTW_INCLUDE_DIRS})

#===============================================================================
#= libusb-1.0
#-------------------------------------------------------------------------------
# Priority 1: project-internal libusb submodule. The root CMakeLists.txt
# calls add_subdirectory(libusb) BEFORE include(deps.cmake), so the usb-1.0
# target already exists here. When detected, set the variables directly and
# skip find_package to bypass the system-search fallback path in
# Findlibusb-1.0.cmake (which would warn about 24MHz streaming on Windows).
# Priority 2: fall back to find_package(libusb-1.0) (system libusb).
#-------------------------------------------------------------------------------
if(TARGET usb-1.0)
	set(LIBUSB_1_INTERNAL TRUE)
	set(LIBUSB_1_FOUND TRUE)
	set(LIBUSB_1_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/libusb/libusb")
	set(LIBUSB_1_LIBRARIES usb-1.0)
else()
	find_package(libusb-1.0)
endif()

if(NOT LIBUSB_1_FOUND)
	message(FATAL_ERROR  "Please install libusb!")
endif()

message("----- libusb-1.0:")
message(STATUS "	 includes:" ${LIBUSB_1_INCLUDE_DIRS})
message(STATUS "	 libraries:" ${LIBUSB_1_LIBRARIES})
include_directories(${LIBUSB_1_INCLUDE_DIRS})

#===============================================================================
#= zlib
#-------------------------------------------------------------------------------
find_package(ZLIB QUIET)

if(NOT ZLIB_FOUND)
	message(FATAL_ERROR  "Please install zlib!")
endif()

message("----- zlib:")
message(STATUS "	 includes:" ${ZLIB_INCLUDE_DIRS})
message(STATUS "	 libraries:" ${ZLIB_LIBRARIES})
include_directories(${ZLIB_INCLUDE_DIRS})

#===============================================================================
#= Qt6
#-------------------------------------------------------------------------------

set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets Gui GuiPrivate Svg Concurrent WebSockets Multimedia)
message("----- Qt6:")
message(STATUS "	 includes:" ${Qt6Core_INCLUDE_DIRS})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Qt6Widgets_EXECUTABLE_COMPILE_FLAGS}")
set(QT_INCLUDE_DIRS ${Qt6Gui_INCLUDE_DIRS} ${Qt6Widgets_INCLUDE_DIRS} ${Qt6Svg_INCLUDE_DIRS} ${Qt6Concurrent_INCLUDE_DIRS})
set(QT_LIBRARIES Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Svg Qt6::Concurrent Qt6::WebSockets)
# Qt libraries split by Core/View layer:
# - pxview-core (Core layer): QtCore + QtGui + QtNetwork + QtConcurrent + QtWebSockets (NO QtWidgets, NO QtSvg)
# - pxview executable (View layer): adds QtWidgets + QtSvg on top of QT_CORE_LIBS
set(QT_CORE_LIBS Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Network Qt6::Concurrent Qt6::WebSockets)
set(QT_GUI_LIBS Qt6::Widgets Qt6::Svg Qt6::Multimedia)
add_definitions(${Qt6Gui_DEFINITIONS} ${Qt6Widgets_DEFINITIONS})
if(APPLE)
	find_package(Qt6DBus REQUIRED)
	set(QT_LIBRARIES ${QT_LIBRARIES} Qt6::DBus)
	set(QT_CORE_LIBS ${QT_CORE_LIBS} Qt6::DBus)
endif()


#===============================================================================
#= nlohmann/json (header-only JSON library for API layer)
#-------------------------------------------------------------------------------
# Try system package first, fall back to FetchContent
find_package(nlohmann_json 3.2.0 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

#===============================================================================
#= boost
#-------------------------------------------------------------------------------
if(POLICY CMP0167)
	cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.42 QUIET)

if(NOT Boost_FOUND)
	message(FATAL_ERROR  "Please install boost!")
endif()

message("----- boost:")
message(STATUS "	 includes:" ${Boost_INCLUDE_DIRS})
include_directories(${Boost_INCLUDE_DIRS})

#===============================================================================
#= Dependencies
#-------------------------------------------------------------------------------

find_package(Threads)

#===============================================================================
#= mimalloc (performance allocator)
#= Used by libsigrokdecode/ann_batch.c for per-session annotation heaps.
#=
#= macOS: mimalloc is NOT linked at all. Its global malloc-zone override
#= (installed automatically when linked, because Homebrew builds with
#= MI_OVERRIDE=ON) crashes the embedded Python 3.14 during decoder import —
#= libsystem malloc dispatches into the mimalloc zone and the allocation
#= faults. ann_batch.c and annotation_heap.cpp fall back to plain GLib/malloc
#= on macOS (see the __APPLE__ guards), so no mi_* symbols are referenced and
#= no mimalloc headers are needed.
#=
#= Discovery order (non-macOS):
#=   1. find_package(mimalloc CONFIG) — CMake config files (Homebrew, vcpkg, MSYS2)
#=      NOTE: target names differ across distributors:
#=        - MSYS2 mingw: "mimalloc" / "mimalloc-static" (no namespace)
#=        - Homebrew/vcpkg: "mimalloc::mimalloc" (namespaced)
#=      We detect whichever target actually exists after find_package.
#=   2. pkg_search_module — Linux apt libmimalloc-dev, MSYS2 fallback
#=   3. Homebrew prefix — macOS fallback when no cmake config installed
#=   4. Bare "mimalloc" — Linux/MSYS2 last resort (standard search paths)
#-------------------------------------------------------------------------------
set(MIMALLOC_LIB "")

if(APPLE)
	message(STATUS "----- mimalloc:")
	message(STATUS "	 library: (disabled on macOS — malloc-zone override crashes embedded Python 3.14)")
else()
find_package(mimalloc CONFIG QUIET)
if(mimalloc_FOUND)
	# Detect the actual imported target name (varies by distributor)
	if(TARGET mimalloc::mimalloc)
		set(MIMALLOC_LIB mimalloc::mimalloc)
	elseif(TARGET mimalloc-static)
		# MSYS2: static import lib (links libmimalloc.a + system libs)
		set(MIMALLOC_LIB mimalloc-static)
	elseif(TARGET mimalloc)
		# MSYS2: shared import lib (links libmimalloc.dll.a)
		set(MIMALLOC_LIB mimalloc)
	else()
		# find_package set mimalloc_FOUND but no known target — fall through
		set(mimalloc_FOUND FALSE)
	endif()
endif()

if(NOT MIMALLOC_LIB)
	# Try pkg-config (works on Linux apt libmimalloc-dev and MSYS2 mingw package)
	pkg_search_module(MIMALLOC QUIET mimalloc)
	if(MIMALLOC_FOUND)
		set(MIMALLOC_LIB ${MIMALLOC_LIBRARIES})
		link_directories(${MIMALLOC_LIBDIR})
		include_directories(${MIMALLOC_INCLUDE_DIRS})
	else()
		# Linux / MSYS2: bare library name works with standard search paths
		set(MIMALLOC_LIB mimalloc)
	endif()
endif()

message("----- mimalloc:")
message(STATUS "	 library: ${MIMALLOC_LIB}")
endif()

#===============================================================================
#= Aggregated link libraries for pxview-core / PXView executable
# This was lost when the original monolithic CMakeLists.txt was split into
# cmake/*.cmake modules. Restored here so pxview-core (CMakeLists.txt:107)
# gets glib/FFTW/zlib/boost/Python3 link libs.
# NOTE: libusb-1.0 is NOT linked here — it is an internal dependency of
# libsigrok.dll. PXView code only uses LIBUSB_SPEED_* compile-time enum
# constants from <libusb-1.0/libusb.h> (include path is in
# include_directories above). Linking libusb here would cause PXView.exe
# to load a second libusb-1.0.dll instance, creating two independent
# libusb contexts that conflict with libsigrok's context (manifests as
# LIBUSB_ERROR_ACCESS during scan and "Failed to get libusb file
# descriptors" during capture).
#-------------------------------------------------------------------------------
set(PXVIEW_LINK_LIBS
	${GLIB_LIBRARIES}
	${FFTW_LIBRARIES}
	${ZLIB_LIBRARIES}
	${Boost_LIBRARIES}
	${PY_LIB}
	${CMAKE_THREAD_LIBS_INIT}
)
