##
## This file is part of the PXView project.
##
## Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
## Copyright (C) 2012-2013 Alexandru Gagniuc <mr.nuke.me@gmail.com>
## Copyright (C) 2013-2022 DreamSourceLab <support@dreamsourcelab.com>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 2 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program.  If not, see <http://www.gnu.org/licenses/>.
##

cmake_minimum_required(VERSION 3.10)

project(PXView)

#===============================================================================
#= Config Header
#-------------------------------------------------------------------------------

set(DS_TITLE PXView)
set(DS_DESCRIPTION "A GUI for instruments of PXTOOL")

set(DS_VERSION_MAJOR 1)
set(DS_VERSION_MINOR 5)
set(DS_VERSION_MICRO 7)
set(DS_VERSION_STRING  ${DS_VERSION_MAJOR}.${DS_VERSION_MINOR}.${DS_VERSION_MICRO})

configure_file (
	${PROJECT_SOURCE_DIR}/PXView/config.h.in
	${PROJECT_BINARY_DIR}/PXView/config.h
)

#===============================================================================
#=pkg config
#-------------------------------------------------------------------------------
include(FindPkgConfig)
include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake")
#list(APPEND CMAKE_PREFIX_PATH "xxx.cmake find path")

find_package(PkgConfig)

if(NOT PKG_CONFIG_FOUND)
	message(FATAL_ERROR  "Please install pkg-config!")
endif()

#===============================================================================
#= Modular includes — split into cmake/*.cmake files (C2)
# Variable visibility is preserved across include() (same scope), so the
# split is purely mechanical: same options, sources, flags, install rules.
#-------------------------------------------------------------------------------

#===============================================================================
#= libusb — project-internal submodule (v1.0.30 with Windows event abstraction)
#= Built as a static library, linked into libsigrok. Replaces system libusb
#= 1.0.27 which lacks events_windows.c (the merged form of dickens/libusb's
#= event-abstraction-v4 branch). This eliminates the root cause of 24MHz
#= streaming failures on Windows (libusb_get_pollfds() returns NULL on WinUSB).
#= Conditionally built only when the submodule is initialized; otherwise the
#= system package is used (with a warning).
#= IMPORTANT: add_subdirectory(libusb) MUST run BEFORE include(deps.cmake) so
#= that the `usb-1.0` target exists when deps.cmake performs its
#= `if(TARGET usb-1.0)` check. Otherwise deps.cmake falls back to the system
#= libusb DLL (1.0.27), defeating the purpose of the submodule.
#-------------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/CMake/options.cmake)

#===============================================================================
#= libusb — project-internal submodule (v1.0.30 with Windows event abstraction)
#= Built as a static library, linked into libsigrok. Replaces system libusb
#= 1.0.27 which lacks events_windows.c (the merged form of dickens/libusb's
#= event-abstraction-v4 branch). This eliminates the root cause of 24MHz
#= streaming failures on Windows (libusb_get_pollfds() returns NULL on WinUSB).
#=
#= Windows-only: event-abstraction-v4 的核心改动仅在 Windows 后端
#= (events_windows.c / threads_windows.c / windows_hotplug.c)。Linux/macOS
#= 走 events_posix.c / threads_posix.c,与上游 libusb 1.0.x 无差异,直接用
#= 系统 libusb-1.0 包即可(参考 sigrok-build 工作流:Linux 用 apt
#= libusb-1.0-0-dev,macOS 用 brew libusb)。Linux/macOS 自建 submodule 反而
#= 需要 config.h 生成逻辑(libusb 源码无条件 #include <config.h>),而当前
#= libusb/CMakeLists.txt 只在 WIN32 下 configure_file 生成 config.h。
#=
#= Conditionally built only on Windows when the submodule is initialized;
#= otherwise (Linux/macOS) deps.cmake 的 find_package(libusb-1.0) 会
#= fallback 到系统包 (with a warning).
#=
#= IMPORTANT: add_subdirectory(libusb) MUST run BEFORE include(deps.cmake) so
#= that the `usb-1.0` target exists when deps.cmake performs its
#= `if(TARGET usb-1.0)` check. Otherwise deps.cmake falls back to the system
#= libusb DLL (1.0.27), defeating the purpose of the submodule — and the
#= system header lacks the libusb_endpoint_set_raw_io() declaration added
#= in 1.0.30, causing implicit-declaration compile errors in fx2lafw.
#-------------------------------------------------------------------------------
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/libusb/CMakeLists.txt" AND EXISTS "${CMAKE_SOURCE_DIR}/libusb/libusb/libusb.h")
	add_subdirectory(libusb)
endif()

include(${CMAKE_SOURCE_DIR}/CMake/deps.cmake)
include(${CMAKE_SOURCE_DIR}/CMake/flags.cmake)
include(${CMAKE_SOURCE_DIR}/CMake/core_sources.cmake)
include(${CMAKE_SOURCE_DIR}/CMake/gui_sources.cmake)

#===============================================================================
#= libsigrok — upstream libsigrok 0.6.0 as a STATIC library (100% unmodified
#= upstream source). Driver registration via upstream SR_REGISTER_DEV_DRIVER
#= section mechanism. PXView links with --whole-archive to retain all driver
#= objects in the __sr_driver_list section.
add_subdirectory(libsigrok)

#===============================================================================
#= pxview-interface: callback + event interfaces (headers only, INTERFACE lib)
#-------------------------------------------------------------------------------
add_subdirectory(PXView/pv/interface)

#===============================================================================
#= pxview-utility: leaf utilities (encoding, path, array) — STATIC lib
#-------------------------------------------------------------------------------
add_subdirectory(PXView/pv/utility)

#===============================================================================
#= pxview-config: app config + shortcut definitions — STATIC lib
#-------------------------------------------------------------------------------
add_subdirectory(PXView/pv/config)

#===============================================================================
#= pxview-data: data layer (snapshots, models, stacks, decode/) — STATIC lib
#-------------------------------------------------------------------------------
add_subdirectory(PXView/pv/data)

#===============================================================================
#= pxview-core static library — Core layer (no Qt Widgets dependency)
#-------------------------------------------------------------------------------
# Compiles the Core layer sources (SigSession, DeviceAgent, Data layer, API
# layer, utility, config) into a standalone static library that depends only
# on Qt6::Core/Gui/Network/Concurrent/WebSockets (NOT Qt6::Widgets/Svg).
# Q_OBJECT meta-object implementations (moc_*.cpp) for Core classes are compiled
# in the executable target below; the static library may carry unresolved
# meta-object symbols which get resolved at final executable link time.
add_library(pxview-core STATIC ${PXVIEW_CORE_SOURCES})
target_link_libraries(pxview-core PUBLIC
	${QT_CORE_LIBS}
	${PXVIEW_LINK_LIBS}
	nlohmann_json::nlohmann_json
	pxview-interface
	pxview-utility
	pxview-config
	pxview-data
)
# Expose libsigrok/include/ headers so sigsession.cpp / deviceagent.cpp
# (compiled into pxview-core) can include <libsigrok/libsigrok.h>.
# PUBLIC so the executable inherits the include path too.
target_include_directories(pxview-core PUBLIC
	${CMAKE_SOURCE_DIR}/libsigrok/include
)
# PCH for pxview-core: use the same PCH header directly (not REUSE_FROM,
# which would create a circular dependency since PXView links pxview-core).
target_precompile_headers(pxview-core PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/PXView/pv/base/common_pch.h
)

#===============================================================================
#= common + libsigrokdecode source lists
# These were lost when the original monolithic CMakeLists.txt (~2000 lines) was
# split into cmake/*.cmake modules. Restored here so common/log/xlog.c (pxv_log
# backend) and the libsigrokdecode engine (srd_session_*, srd_inst_*) compile
# into the executable. Without these the linker fails with undefined xlog_*
# and srd_* references from decoderstack.cpp / decoder.cpp.
#-------------------------------------------------------------------------------
set(common_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/common/minizip/zip.c
	${CMAKE_CURRENT_SOURCE_DIR}/common/minizip/unzip.c
	${CMAKE_CURRENT_SOURCE_DIR}/common/minizip/ioapi.c
	${CMAKE_CURRENT_SOURCE_DIR}/common/log/xlog.c
)

set(libsigrokdecode_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/decoder.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/instance.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/type_decoder.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/session.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/module_sigrokdecode.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/srd.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/log.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/dll_registry.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/version.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/util.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/exception.c
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/error.c
)

#===============================================================================
#= PXView executable — GUI/View layer linking pxview-core
#-------------------------------------------------------------------------------
add_executable(${PROJECT_NAME}
	${common_SOURCES}
	${PXVIEW_GUI_SOURCES}
	${PXView_HEADERS_MOC}
	${PXView_HEADERS_NO_MOC}
	${PXView_FORMS_HEADERS}
	${PXView_RESOURCES_RCC}
	${libsigrokdecode_SOURCES}
)

# Include dirs for common/ (xlog.h, ds_types.h) and libsigrokdecode/ (config.h,
# libsigrokdecode.h, internal headers) so the sources above find their headers.
target_include_directories(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/common
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode
)

# Executable links pxview-core (which transitively provides Qt Core/Gui/Network
# /WebSockets + external libs + nlohmann_json via PUBLIC linkage) and adds the
# Qt Widgets + Svg modules that the View layer needs.
# libsigrok is a STATIC library; using --whole-archive ensures all driver
# objects (registered via __attribute__((section("__sr_driver_list"))) are
# retained. Without --whole-archive the linker drops unreferenced objects and
# the driver section becomes empty, resulting in "Device list is empty".
target_link_libraries(${PROJECT_NAME}
	pxview-core
	${QT_GUI_LIBS}
)
if(WIN32)
	target_link_libraries(${PROJECT_NAME}
		"-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
elseif(APPLE)
	# macOS ld64 不支持 --whole-archive；用 -all_load 强制保留所有静态库 object，
	# 防止驱动段 __DATA,__sr_driver_list 被链接器丢弃导致设备列表为空。
	target_link_libraries(${PROJECT_NAME} "-Wl,-all_load" libsigrok)
else()
	# Linux: --whole-archive 强制保留所有 driver objects（通过
	# __attribute__((section("__sr_driver_list"))) 注册），否则链接器
	# 会丢弃未引用的驱动 object，导致 __sr_driver_list 段为空、设备列表为空。
	target_link_libraries(${PROJECT_NAME}
		"-Wl,--whole-archive" libsigrok "-Wl,--no-whole-archive")
endif()

if(NOT WIN32 AND NOT APPLE)
	target_link_libraries(${PROJECT_NAME} "-Wl,--export-dynamic")
endif()

#===============================================================================
#= Precompiled Headers (PCH) + Unity Build
#-------------------------------------------------------------------------------
# PCH: Precompiles the most frequently included headers (vector, QString,
#   log.h, pxvdef.h, etc.) into a single .gch file, eliminating redundant
#   parsing across ~130 compilation units. Expected 20-40% build speedup.
# C sources (common/*.c, libsigrokdecode/*.c) cannot use C++ PCH — exclude them.
set_source_files_properties(${common_SOURCES} ${libsigrokdecode_SOURCES}
	PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_precompile_headers(${PROJECT_NAME} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/PXView/pv/base/common_pch.h
)
if(CMAKE_UNITY_BUILD)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		UNITY_BUILD_BATCH_SIZE 8
	)
endif()

if(WIN32)
# Pass -mwindows so that no "DOS box" will open when PXView is started.
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "-mwindows")
endif()

if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUNDLE TRUE
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER com.pxlogic.${PROJECT_NAME}
        MACOSX_BUNDLE_BUNDLE_NAME ${PROJECT_NAME}
        MACOSX_BUNDLE_BUNDLE_VERSION ${DS_VERSION_STRING}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${DS_VERSION_STRING}
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/mac_appbundle_template.plist.in
        MACOSX_BUNDLE_ICON_FILE PXView.icns
        # Ensure the binary can find Qt frameworks bundled inside the .app
        # by macdeployqt. Without this, dyld fails with "Symbol not found"
        # because @rpath/QtCore.framework/... cannot be resolved.
        INSTALL_RPATH "@executable_path/../Frameworks"
        BUILD_RPATH "@executable_path/../Frameworks"
    )
endif()

if(ENABLE_COTIRE)
	include(cotire)
	cotire(${PROJECT_NAME})
endif()

message(STATUS "Output dir: ${CMAKE_CURRENT_SOURCE_DIR}/build.dir")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/build.dir")

#===============================================================================
#= Decoders (C decoder DLLs + irmp + decoder_test) — needs EXECUTABLE_OUTPUT_PATH
#-------------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/CMake/decoders.cmake)

#===============================================================================
#= Installation + CPack packaging
#-------------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/CMake/install_packaging.cmake)

#===============================================================================
#= Tests
#-------------------------------------------------------------------------------
option(ENABLE_TESTS "Build test executables" OFF)

if(ENABLE_TESTS)
	add_subdirectory(tests)
	enable_testing()
endif(ENABLE_TESTS)

#add_executable(test_font test_font.cpp)
#target_link_libraries(test_font Qt6::Gui Qt6::Widgets)
