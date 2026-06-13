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
set(DS_VERSION_MICRO 0)
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
#= User Options
#-------------------------------------------------------------------------------

set(DISABLE_WERROR TRUE) #Build without -Werror
set(ENABLE_SIGNALS TRUE) #Build with UNIX signals
set(ENABLE_COTIRE FALSE) #Enable cotire
set(ENABLE_TESTS  FALSE) #Enable unit tests
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
#= first search path
#-------------------------------------------------------------------------------
include_directories(
   ./PXView
   ./libsigrok
   ./libsigrokdecode
   ./common
)

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
include_directories(${GLIB_INCLUDE_DIRS})
link_directories(${GLIB_LIBDIR})

#===============================================================================
#= python3
#-------------------------------------------------------------------------------

if(POLICY CMP0148)
	cmake_policy(SET CMP0148 NEW)
endif()
find_package(Python3 COMPONENTS Interpreter Development)

if (Python3_FOUND)
	message("----- python3:")
	message(STATUS "	 includes:" ${Python3_INCLUDE_DIRS})
	message(STATUS "	 libraries:" ${Python3_LIBRARIES})
	include_directories(${Python3_INCLUDE_DIRS})
	set(PY_LIB ${Python3_LIBRARIES})
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
find_package(libusb-1.0)

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
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets Gui GuiPrivate Svg Concurrent WebSockets)
message("----- Qt6:")
message(STATUS "	 includes:" ${Qt6Core_INCLUDE_DIRS})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Qt6Widgets_EXECUTABLE_COMPILE_FLAGS}")
set(QT_INCLUDE_DIRS ${Qt6Gui_INCLUDE_DIRS} ${Qt6Widgets_INCLUDE_DIRS} ${Qt6Svg_INCLUDE_DIRS} ${Qt6Concurrent_INCLUDE_DIRS})
set(QT_LIBRARIES Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Svg Qt6::Concurrent Qt6::WebSockets)
add_definitions(${Qt6Gui_DEFINITIONS} ${Qt6Widgets_DEFINITIONS})
if(APPLE)
	find_package(Qt6DBus REQUIRED)
	set(QT_LIBRARIES ${QT_LIBRARIES} Qt6::DBus)
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
#= PXView sources
#-------------------------------------------------------------------------------

set(PXView_SOURCES
    PXView/main.cpp
    PXView/application.cpp
    PXView/pv/log.cpp
    PXView/pv/sigsession.cpp
    PXView/pv/sessionmanager.cpp
    PXView/pv/mainwindow.cpp
    PXView/pv/data/snapshot.cpp
    PXView/pv/data/signaldata.cpp
    PXView/pv/data/logicsnapshot.cpp
    PXView/pv/data/disk_buffer_manager.cpp
    PXView/pv/data/disk_write_thread.cpp
    PXView/pv/data/disk_read_cache.cpp
    PXView/pv/data/mmap_allocator.cpp
    PXView/pv/data/analogsnapshot.cpp
    PXView/pv/data/sessionsnapshot.cpp
    PXView/pv/data/sessiondocument.cpp
    PXView/pv/dialogs/deviceoptions.cpp
    PXView/pv/prop/property.cpp
    PXView/pv/prop/int.cpp
    PXView/pv/prop/enum.cpp
    PXView/pv/prop/double.cpp
    PXView/pv/prop/bool.cpp
    PXView/pv/prop/binding/binding.cpp
    PXView/pv/toolbars/samplingbar.cpp
    PXView/pv/view/viewport.cpp
    PXView/pv/view/edge_nav_button.cpp
    PXView/pv/view/view.cpp
    PXView/pv/view/timemarker.cpp
    PXView/pv/view/signal.cpp
    PXView/pv/view/ruler.cpp
    PXView/pv/view/logicsignal.cpp
    PXView/pv/view/header.cpp
    PXView/pv/view/cursor.cpp
    PXView/pv/view/analogsignal.cpp
    PXView/pv/prop/binding/deviceoptions.cpp
    PXView/pv/toolbars/trigbar.cpp
    PXView/pv/toolbars/filebar.cpp
    PXView/pv/dock/protocoldock.cpp
    PXView/pv/dock/triggerdock.cpp
    PXView/pv/dock/measuredock.cpp
    PXView/pv/dock/searchdock.cpp
    PXView/pv/dock/logdock.cpp
    PXView/pv/dock/deviceoptionsdock.cpp
    PXView/pv/dock/signalprocessingdock.cpp
    PXView/pv/dock/mcpcontroldock.cpp
    PXView/pv/toolbars/logobar.cpp
    PXView/pv/dialogs/about.cpp
    PXView/pv/dialogs/search.cpp
    PXView/pv/data/dsosnapshot.cpp
    PXView/pv/view/dsosignal.cpp
    PXView/pv/view/dsldial.cpp
    PXView/pv/dock/dsotriggerdock.cpp
    PXView/pv/view/trace.cpp
    PXView/pv/view/selectableitem.cpp
    PXView/pv/data/decoderstack.cpp
    PXView/pv/data/decode/rowdata.cpp
    PXView/pv/data/decode/row.cpp
    PXView/pv/data/decode/decoder.cpp
    PXView/pv/data/decode/annotation.cpp
    PXView/pv/view/decodetrace.cpp
    PXView/pv/prop/binding/decoderoptions.cpp
    PXView/pv/widgets/fakelineedit.cpp
    PXView/pv/widgets/searchpatterninput.cpp
    PXView/pv/widgets/decodermenu.cpp
    PXView/pv/widgets/decodergroupbox.cpp
    PXView/pv/prop/string.cpp
    PXView/pv/dialogs/storeprogress.cpp
    PXView/pv/storesession.cpp
    PXView/pv/view/devmode.cpp
    PXView/pv/dialogs/waitingdialog.cpp
    PXView/pv/dialogs/dsomeasure.cpp
    PXView/pv/dialogs/calibration.cpp
    PXView/pv/data/decodermodel.cpp
    PXView/pv/dialogs/protocollist.cpp
    PXView/pv/dialogs/protocolexp.cpp
    PXView/pv/dialogs/fftoptions.cpp
    PXView/pv/data/mathstack.cpp
    PXView/pv/view/mathtrace.cpp   
    PXView/pv/toolbars/titlebar.cpp
    PXView/pv/mainframe.cpp
    PXView/pv/submainframe.cpp
    PXView/pv/widgets/border.cpp
    PXView/pv/widgets/slidingdrawer.cpp
    PXView/pv/widgets/smoothscrollbar.cpp
    PXView/pv/widgets/smoothscrollarea.cpp
    PXView/pv/widgets/smoothtablehelper.cpp
    PXView/pv/widgets/sidebar.cpp
    PXView/pv/widgets/sidebarbutton.cpp
    PXView/pv/dialogs/dsmessagebox.cpp
    PXView/pv/dialogs/shadow.cpp
    PXView/pv/dialogs/dsdialog.cpp
    PXView/pv/dialogs/interval.cpp
    PXView/pv/prop/binding/probeoptions.cpp
    PXView/pv/view/viewstatus.cpp
    PXView/pv/dialogs/lissajousoptions.cpp
    PXView/pv/view/lissajoustrace.cpp
    PXView/pv/view/spectrumtrace.cpp
    PXView/pv/data/spectrumstack.cpp
    PXView/pv/dialogs/mathoptions.cpp
    PXView/pv/dialogs/regionoptions.cpp
    PXView/pv/view/xcursor.cpp
    PXView/pv/dock/protocoldock.cpp
    PXView/pv/data/decoderstack.cpp
    PXView/pv/data/decode/annotation.cpp
    PXView/pv/data/decode/decoder.cpp
    PXView/pv/data/decode/row.cpp
    PXView/pv/data/decode/rowdata.cpp
    PXView/pv/prop/binding/decoderoptions.cpp
    PXView/pv/view/decodetrace.cpp
    PXView/pv/widgets/decodergroupbox.cpp
    PXView/pv/widgets/decodermenu.cpp
    PXView/pv/config/appconfig.cpp
    PXView/pv/config/shortcutdefs.cpp
    PXView/pv/appcontrol.cpp
    PXView/pv/dstimer.cpp
    PXView/pv/eventobject.cpp
    PXView/pv/ZipMaker.cpp
    PXView/pv/data/decode/annotationrestable.cpp
    PXView/pv/data/decode/decoderstatus.cpp
    PXView/pv/dock/protocolitemlayer.cpp
    PXView/pv/ui/msgbox.cpp
    PXView/pv/ui/toast.cpp
    PXView/pv/ui/dscombobox.cpp
    PXView/pv/ui/dsspinbox.cpp
    PXView/pv/dsvdef.cpp
    PXView/pv/dialogs/applicationpardlg.cpp
    PXView/pv/dock/keywordlineedit.cpp
    PXView/pv/dock/searchcombobox.cpp
    PXView/pv/dialogs/decoderoptionsdlg.cpp
    PXView/pv/utility/encoding.cpp
    PXView/pv/utility/path.cpp
    PXView/pv/utility/array.cpp
    PXView/pv/deviceagent.cpp
    PXView/pv/ui/langresource.cpp
    PXView/pv/ui/fn.cpp
    PXView/pv/ui/xtoolbutton.cpp
    PXView/pv/ui/draggabletabbar.cpp
    PXView/pv/ui/draggabletabwidget.cpp
    PXView/pv/ui/uimanager.cpp
    PXView/pv/ui/popupdlglist.cpp
    PXView/pv/ui/iconcache.cpp
    PXView/pv/ui/widgetinspector.cpp
    PXView/pv/tabcontext.cpp
    PXView/pv/api/session_service.cpp
    PXView/pv/api/app_service.cpp
    PXView/pv/api/rpc_dispatcher.cpp
    PXView/pv/api/ws_transport.cpp
    PXView/pv/api/mcp_transport.cpp
)

set(UIS
)

if(APPLE)
	qt_wrap_ui(UI_HEADERS ${UIS})
else()
	if(Qt5Core_FOUND)
		qt5_wrap_ui(UI_HEADERS ${UIS})
	elseif(Qt6Core_FOUND)
		qt6_wrap_ui(UI_HEADERS ${UIS})
	endif()
endif()

# Windows-specific QT source files
if(WIN32)
	list(APPEND PXView_SOURCES
		PXView/pv/winnativewidget.cpp
		PXView/pv/winshadow.cpp
		PXView/pv/wintaskbarprogress.cpp
	)
	list(APPEND PXView_HEADERS
		PXView/pv/wintaskbarprogress.h
	)
endif ()

set(PXView_HEADERS
    PXView/mystyle.h
    PXView/pv/log.h
    PXView/pv/sigsession.h
    PXView/pv/sessionmanager.h
    PXView/pv/interface/icontextaware.h
    PXView/pv/mainwindow.h
    PXView/pv/dialogs/deviceoptions.h
    PXView/pv/prop/property.h
    PXView/pv/prop/int.h
    PXView/pv/prop/enum.h
    PXView/pv/prop/double.h
    PXView/pv/prop/bool.h
    PXView/pv/toolbars/samplingbar.h
    PXView/pv/view/viewport.h
    PXView/pv/view/edge_nav_button.h
    PXView/pv/view/view.h
    PXView/pv/view/timemarker.h
    PXView/pv/view/ruler.h
    PXView/pv/view/header.h
    PXView/pv/view/cursor.h
    PXView/pv/toolbars/trigbar.h
    PXView/pv/toolbars/filebar.h
    PXView/pv/dock/protocoldock.h
    PXView/pv/dock/triggerdock.h
    PXView/pv/dock/measuredock.h
    PXView/pv/dock/searchdock.h
    PXView/pv/dock/logdock.h
    PXView/pv/dock/deviceoptionsdock.h
    PXView/pv/dock/signalprocessingdock.h
    PXView/pv/dock/mcpcontroldock.h
    PXView/pv/toolbars/logobar.h
    PXView/pv/dialogs/about.h
    PXView/pv/dialogs/search.h
    PXView/pv/dock/dsotriggerdock.h
    PXView/pv/view/trace.h
    PXView/pv/view/selectableitem.h
    PXView/pv/data/decoderstack.h
    PXView/pv/view/decodetrace.h
    PXView/pv/widgets/fakelineedit.h
    PXView/pv/widgets/searchpatterninput.h
    PXView/pv/widgets/decodermenu.h
    PXView/pv/widgets/decodergroupbox.h
    PXView/pv/prop/string.h
    PXView/pv/dialogs/storeprogress.h
    PXView/pv/storesession.h
    PXView/pv/view/devmode.h
    PXView/pv/dialogs/waitingdialog.h
    PXView/pv/dialogs/dsomeasure.h
    PXView/pv/dialogs/calibration.h
    PXView/pv/dialogs/protocollist.h
    PXView/pv/dialogs/protocolexp.h
    PXView/pv/dialogs/fftoptions.h
    PXView/pv/data/mathstack.h
    PXView/pv/view/mathtrace.h
    PXView/pv/view/viewstatus.h
    PXView/pv/toolbars/titlebar.h
    PXView/pv/mainframe.h
    PXView/pv/submainframe.h
    PXView/pv/widgets/border.h
    PXView/pv/widgets/slidingdrawer.h
    PXView/pv/widgets/smoothscrollbar.h
    PXView/pv/widgets/smoothscrollarea.h
    PXView/pv/widgets/smoothtablehelper.h
    PXView/pv/widgets/sidebar.h
    PXView/pv/widgets/sidebarbutton.h
    PXView/pv/dialogs/dsmessagebox.h
    PXView/pv/dialogs/shadow.h
    PXView/pv/dialogs/dsdialog.h
    PXView/pv/dialogs/interval.h
    PXView/pv/dialogs/lissajousoptions.h
    PXView/pv/view/lissajoustrace.h
    PXView/pv/view/spectrumtrace.h
    PXView/pv/data/spectrumstack.h
    PXView/pv/data/decodermodel.h
    PXView/pv/data/datasource.h
    PXView/pv/data/sessionsnapshot.h
    PXView/pv/data/sessiondocument.h
    PXView/pv/dialogs/mathoptions.h
    PXView/pv/dialogs/regionoptions.h
    PXView/pv/view/xcursor.h
    PXView/pv/view/signal.h
    PXView/pv/view/logicsignal.h
    PXView/pv/view/analogsignal.h
    PXView/pv/view/dsosignal.h 
    PXView/pv/dock/protocoldock.h
    PXView/pv/data/decoderstack.h
    PXView/pv/view/decodetrace.h
    PXView/pv/widgets/decodergroupbox.h
    PXView/pv/widgets/decodermenu.h
    PXView/pv/config/appconfig.h
    PXView/pv/appcontrol.h
    PXView/pv/dstimer.h
    PXView/pv/eventobject.h
    PXView/pv/ZipMaker.h
    PXView/pv/data/decode/annotationrestable.h
    PXView/pv/data/decode/decoderstatus.h
    PXView/pv/dock/protocolitemlayer.h
    PXView/pv/ui/msgbox.h
    PXView/pv/ui/toast.h
    PXView/pv/ui/dscombobox.h
    PXView/pv/ui/dsspinbox.h
    PXView/pv/dsvdef.h
    PXView/pv/dialogs/applicationpardlg.h
    PXView/pv/dock/keywordlineedit.h
    PXView/pv/dock/searchcombobox.h
    PXView/pv/dialogs/decoderoptionsdlg.h
    PXView/pv/utility/encoding.h
    PXView/pv/utility/path.h
    PXView/pv/utility/array.h
    PXView/pv/deviceagent.h
    PXView/pv/ui/fn.h
    PXView/pv/ui/xtoolbutton.h
    PXView/pv/ui/draggabletabbar.h
    PXView/pv/ui/draggabletabwidget.h
    PXView/pv/ui/iconcache.h
    PXView/pv/ui/widgetinspector.h
    PXView/pv/tabcontext.h
    PXView/pv/api/session_service.h
    PXView/pv/api/app_service.h
    PXView/pv/api/rpc_dispatcher.h
    PXView/pv/api/ws_transport.h
    PXView/pv/api/mcp_transport.h
    ${UI_HEADERS}
)
 
# Windows-specific QT headers
if(WIN32)
	list(APPEND PXView_HEADERS
		PXView/pv/winnativewidget.h
		PXView/pv/winshadow.h
	)
endif ()

set(PXView_HEADERS_NO_MOC
    PXView/pv/log.h
    PXView/pv/sigsession.h
    PXView/pv/sessionmanager.h
    PXView/pv/interface/icontextaware.h
    PXView/pv/data/datasource.h
    PXView/pv/data/sessionsnapshot.h
    PXView/pv/data/sessiondocument.h
    PXView/pv/config/appconfig.h
    PXView/pv/appcontrol.h
    PXView/pv/ZipMaker.h
    PXView/pv/data/decode/annotationrestable.h
    PXView/pv/data/decode/decoderstatus.h
    PXView/pv/ui/msgbox.h
    PXView/pv/dsvdef.h
    PXView/pv/utility/encoding.h
    PXView/pv/utility/path.h
    PXView/pv/utility/array.h
    PXView/pv/deviceagent.h
    PXView/pv/ui/fn.h
    PXView/pv/ui/iconcache.h
    PXView/pv/tabcontext.h
)

if(WIN32)
    list(APPEND PXView_HEADERS_NO_MOC PXView/pv/winnativewidget.h)
endif()

list(REMOVE_ITEM PXView_HEADERS ${PXView_HEADERS_NO_MOC})

#===============================================================================
#= Compat drivers option
#-------------------------------------------------------------------------------
option(ENABLE_COMPAT_DRIVERS "Enable standard sigrok compatible drivers" OFF)
option(ENABLE_DRIVER_FX2LAFW "Enable fx2lafw driver" OFF)
option(ENABLE_DRIVER_SALEAE_LOGIC16 "Enable saleae-logic16 driver" OFF)
option(ENABLE_DRIVER_RASPBERRYPI_PICO "Enable raspberrypi-pico driver" OFF)

if(ENABLE_COMPAT_DRIVERS)
    add_definitions(-DHAVE_COMPAT_DRIVERS)

    # Find libserialport for serial compat layer
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSERIALPORT QUIET libserialport)
    endif()
    if(LIBSERIALPORT_FOUND)
        add_definitions(-DHAVE_LIBSERIALPORT)
        include_directories(${LIBSERIALPORT_INCLUDE_DIRS})
    endif()

    if(ENABLE_DRIVER_FX2LAFW)
        add_definitions(-DHAVE_DRIVER_FX2LAFW)
    endif()
    if(ENABLE_DRIVER_SALEAE_LOGIC16)
        add_definitions(-DHAVE_DRIVER_SALEAE_LOGIC16)
    endif()
    if(ENABLE_DRIVER_RASPBERRYPI_PICO)
        add_definitions(-DHAVE_DRIVER_RASPBERRYPI_PICO)
    endif()
endif()

#===============================================================================
#= libsigrok source
#-------------------------------------------------------------------------------
set(libsigrok_SOURCES
    libsigrok/version.c
    libsigrok/strutil.c
    libsigrok/std.c
    libsigrok/session_driver.c
    libsigrok/session.c
    libsigrok/log.c
    libsigrok/hwdriver.c
    libsigrok/error.c
    libsigrok/backend.c
    libsigrok/output/output.c
    libsigrok/input/input.c
    libsigrok/hardware/demo/demo.c
    libsigrok/input/in_binary.c
    libsigrok/input/in_vcd.c
    libsigrok/input/in_wav.c
    libsigrok/output/csv.c
    libsigrok/output/gnuplot.c
    libsigrok/output/srzip.c
    libsigrok/output/vcd.c
    libsigrok/hardware/DSL/dslogic.c
    libsigrok/hardware/common/usb.c
    libsigrok/hardware/common/ezusb.c
    libsigrok/trigger.c
    libsigrok/dsdevice.c
    libsigrok/hardware/DSL/dscope.c
    libsigrok/hardware/DSL/command.c
    libsigrok/hardware/DSL/dsl.c
    libsigrok/hardware/pxlogic/pxlogic.c
    libsigrok/hardware/pxlogic/usb_ctrl.c
    libsigrok/lib_main.c
)

if(ENABLE_COMPAT_DRIVERS)
    list(APPEND libsigrok_SOURCES
        libsigrok/hardware/compat/compat_helpers.c
        libsigrok/hardware/compat/compat_serial.c
    )
    if(ENABLE_DRIVER_FX2LAFW)
        list(APPEND libsigrok_SOURCES
            libsigrok/hardware/fx2lafw/api.c
            libsigrok/hardware/fx2lafw/protocol.c
        )
    endif()
    if(ENABLE_DRIVER_SALEAE_LOGIC16)
        list(APPEND libsigrok_SOURCES
            libsigrok/hardware/saleae-logic16/api.c
            libsigrok/hardware/saleae-logic16/protocol.c
        )
    endif()
    if(ENABLE_DRIVER_RASPBERRYPI_PICO)
        list(APPEND libsigrok_SOURCES
            libsigrok/hardware/raspberrypi-pico/api.c
            libsigrok/hardware/raspberrypi-pico/protocol.c
        )
    endif()
endif()

set(libsigrok_HEADERS
    libsigrok/version.h
    libsigrok/libsigrok-internal.h
    libsigrok/libsigrok.h
    libsigrok/config.h
    libsigrok/hardware/DSL/command.h
    libsigrok/hardware/DSL/dsl.h
)
 
#===============================================================================
#= libsigrokdecode source
#-------------------------------------------------------------------------------
set(libsigrokdecode_SOURCES
    libsigrokdecode/type_decoder.c
    libsigrokdecode/srd.c
    libsigrokdecode/module_sigrokdecode.c
    libsigrokdecode/decoder.c
    libsigrokdecode/dll_registry.c
    libsigrokdecode/error.c
    libsigrokdecode/exception.c
    libsigrokdecode/instance.c
    libsigrokdecode/log.c
    libsigrokdecode/session.c
    libsigrokdecode/util.c
    libsigrokdecode/version.c
    libsigrokdecode/c_decoder_api.c
)

set(libsigrokdecode_HEADERS
    libsigrokdecode/libsigrokdecode-internal.h
    libsigrokdecode/libsigrokdecode.h
    libsigrokdecode/config.h
    libsigrokdecode/version.h
)

#===============================================================================
#= common source
#-------------------------------------------------------------------------------

set(common_SOURCES
    common/minizip/zip.c
    common/minizip/unzip.c
    common/minizip/ioapi.c
    common/log/xlog.c
)

set(common_HEADERS
    common/minizip/zip.h
    common/minizip/unzip.h
    common/minizip/ioapi.h
    common/log/xlog.h
)

#===============================================================================
#= compile config
#-------------------------------------------------------------------------------

set(PXView_FORMS
)

set(PXView_RESOURCES
    PXView/PXView.qrc
    PXView/themes/breeze.qrc
    PXView/languages/language.qrc
)


if(WIN32)
	# Use the PXView icon for the PXView.exe executable.
	enable_language(RC)
	# app icon
        list(APPEND PXView_SOURCES applogo.rc)
	# Unicode support for Windows API
	add_definitions(-DUNICODE -D_UNICODE)
endif()

qt6_wrap_cpp(PXView_HEADERS_MOC ${PXView_HEADERS})
qt6_add_resources(PXView_RESOURCES_RCC ${PXView_RESOURCES})




add_definitions(${QT_DEFINITIONS})

# Compiler warnings - only for C/C++, not for RC
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-return-type -Wno-ignored-qualifiers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-return-type -Wno-ignored-qualifiers")

if(NOT DISABLE_WERROR)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
endif()

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c11")


include_directories(
	${CMAKE_CURRENT_BINARY_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}
	${Boost_INCLUDE_DIRS}
	${QT_INCLUDE_DIRS}
)


if(STATIC_PKGDEPS_LIBS)
	include_directories(${PKGDEPS_STATIC_INCLUDE_DIRS})
else()
	include_directories(${PKGDEPS_INCLUDE_DIRS})
endif()

#===============================================================================
#= Release flags
#-------------------------------------------------------------------------------
if(${CMAKE_BUILD_TYPE} STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "-Wall -Wextra")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    add_compile_options(-O3)
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-g")

#===============================================================================
#= Linker Configuration
#-------------------------------------------------------------------------------


set(PXVIEW_LINK_LIBS
	-lz
	-lglib-2.0
	${CMAKE_THREAD_LIBS_INIT}
	${QT_LIBRARIES}
	${LIBUSB_1_LIBRARIES}
	${FFTW_LIBRARIES}
	${PY_LIB}
)

if(STATIC_PKGDEPS_LIBS)
	link_directories(${PKGDEPS_STATIC_LIBRARY_DIRS})
	list(APPEND PXVIEW_LINK_LIBS ${PKGDEPS_STATIC_LIBRARIES})
if(WIN32)
	# Workaround for a MinGW linking issue.
	list(APPEND PULSEVIEW_LINK_LIBS "-llzma -llcms2")
endif()
else()
	link_directories(${PKGDEPS_LIBRARY_DIRS})
	list(APPEND PXVIEW_LINK_LIBS ${PKGDEPS_LIBRARIES})
endif()

# Add libserialport if found (for serial compat layer)
if(LIBSERIALPORT_FOUND)
	list(APPEND PXVIEW_LINK_LIBS ${LIBSERIALPORT_LIBRARIES})
	link_directories(${LIBSERIALPORT_LIBRARY_DIRS})
endif()

add_executable(${PROJECT_NAME}
	${common_SOURCES}
	${PXView_SOURCES}
	${PXView_HEADERS_MOC}
	${PXView_HEADERS_NO_MOC}
	${PXView_FORMS_HEADERS}
	${PXView_RESOURCES_RCC}
	${libsigrok_SOURCES}
	${libsigrokdecode_SOURCES}
)

target_link_libraries(${PROJECT_NAME} ${PXVIEW_LINK_LIBS} nlohmann_json::nlohmann_json)

if(NOT WIN32 AND NOT APPLE)
	target_link_libraries(${PROJECT_NAME} "-Wl,--export-dynamic")
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
    )
endif()

if(ENABLE_COTIRE)
	include(cotire)
	cotire(${PROJECT_NAME})
endif()

message(STATUS "Output dir: ${CMAKE_CURRENT_SOURCE_DIR}/build.dir")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/build.dir")

#===============================================================================
#= C decoder DLL/SO build rules
#-------------------------------------------------------------------------------

set(C_DECODER_OUTPUT_DIR "${EXECUTABLE_OUTPUT_PATH}/decoders/c_decoders")

set(C_DECODERS spi_c i2c_c uart_c can_c jtag_c swd_c onewire_c i2s_c lin_c hdlc_c microwire_c mdio_c ps2_c dmx512_c nrzi_c ir_nec_c ir_rc5_c dcf77_c cec_c spdif_c usb_signalling_c 4b5b_c can_fd_c iso7816_c lpc_c dali_c c2_c graycode_c counter_c lm75_c ds1307_c ds3231_c numbers_and_state_c seven_segment_c pwm_c wiegand_c ir_sirc_c edid_c i2c_packet_c i2cdemux_c i2cfilter_c ltc26x7_c ad5593r_c adxl345_c atsha204a_c bh1750_c eeprom24xx_c flexray_c mipi_rffe_c usb_power_delivery_c iebus_c spacewire_c qspi_c sdio_c spi_dual_quad_c uart_fast_c cjtag_c mlx90614_c mpu6050_c mxc6225xu_c nunchuk_c pca9571_c rtc8564_c ssd1306_c st25dv_c tcs3472x_c tpm_tis_i2c_c tmc_c sent_c sle44xx_c pjdl_c onewire_link_c ac97_c sdcard_sd_c emmc_sd_c swim_c rvswd_c a7105_c ad5626_c ad79x0_c ade77xx_c adf435x_c adb_c afsk_c am230x_c caliper_c carrera_c xfp_c hdcp_c hdmi_scdc_c tca6408a_c tmp102_c dcc_c delta_sigma_c dsi_c em4100_c em4305_c enc28j60_c ltc242x_c max6954_c max7219_c mrf24j40_c ieee488_c ir_irmp_c ir_ltto_c ir_rc6_c ir_recoil_c eth_an_c fsi_c gpib_c guess_bitrate_c iec_c adns5020_c as5047_c avr_isp_c cc1101_c cyrf6936_c mvb_c mcs48_c one_single_wire_c ook_c opentherm_c jitter_c lfast_c maple_bus_c miller_c morse_c nes_gamepad_c nrf24l01_c nrf905_c rfm12_c ssi32_c st25r39xx_spi_c sdcard_spi_c spiflash_c spi_tpm_c tpm_tis_spi_c x2444m_c rgb_led_spi_c sda2506_c signature_c sony_md_c st7735_c st7789_c parallel_c pcfx_ctrlr_c rinnai_control_panel_c rpm_c sae_j1850_vpw_c arm_itm_c arm_tpiu_c bluetooth_h4_c boost_c crsf_c bean_c ccd_c cjtag_oscan0_c rgb_led_ws281x_c stepper_motor_c j1708_c midi_c modbus_c pan1321_c pn532_c sbus_futaba_c scs_c ufcs_c amulet_ascii_c streletz_c z80_c adat_c arm_etmv3_c aud_c avr_pdi_c jtag_avr_c jtag_ejtag_c jtag_stm32_c mipi_dsi_c pxx1_c qi_c rc_encode_c sdq_c onewire_network_c ds2408_c ds243x_c ds28ea00_c eeprom93xx_c spi_fast_c swi_c t55xx_c tdm_audio_c timing_c tlc5620_c xy2_100_c cfp_c ps2_keyboard_c ps2_mouse_c usb_packet_c usb_request_c avclan_c ethernet_c arp_c ipv4_c udp_c ook_oregon_c ook_vis_c ltar_smartdevice_c ir_ltto_decode_c sony_md_decode_c sipi_c pjon_c tpm_fifo_tis_c tm1637_c tm1638_c ltar_smartdevice_decode_c)

foreach(dec ${C_DECODERS})
	add_library(decoder_${dec} MODULE
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/c_decoders/${dec}.c
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/c_decoder_api.c
	)

	target_include_directories(decoder_${dec} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode
		${CMAKE_CURRENT_SOURCE_DIR}/common
		${GLIB_INCLUDE_DIRS}
		${Python3_INCLUDE_DIRS}
	)

	target_compile_definitions(decoder_${dec} PRIVATE
		SRD_C_DECODER_DLL
	)

	target_link_libraries(decoder_${dec}
		-lglib-2.0
	)

	set_target_properties(decoder_${dec} PROPERTIES
		OUTPUT_NAME ${dec}
		LIBRARY_OUTPUT_DIRECTORY ${C_DECODER_OUTPUT_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${C_DECODER_OUTPUT_DIR}
	)
endforeach()

#===============================================================================
#= irmp shared library
#-------------------------------------------------------------------------------

add_library(irmp SHARED
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/irmp/irmp-main-sharedlib.c
)

target_include_directories(irmp PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/irmp
	${GLIB_INCLUDE_DIRS}
	${Python3_INCLUDE_DIRS}
)

target_compile_definitions(irmp PRIVATE
	IRMP_PROTOCOL_NAMES=1
)

if(WIN32)
	target_compile_definitions(irmp PRIVATE WIN32)
endif()

target_link_libraries(irmp
	Python3::Python
	-lglib-2.0
)

set_target_properties(irmp PROPERTIES
	PREFIX ""
	LIBRARY_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}
	RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}
)

#===============================================================================
#= Copy language files to output directory (for development)
#-------------------------------------------------------------------------------
if(WIN32)
    # Create lang directories in output directory
    file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/lang/cn)
    file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/lang/cn/dec)
    file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/lang/en)

    # Copy language files
    file(GLOB CN_LANG_FILES ${CMAKE_CURRENT_SOURCE_DIR}/lang/cn/*.json)
    file(GLOB CN_DEC_LANG_FILES ${CMAKE_CURRENT_SOURCE_DIR}/lang/cn/dec/*.json)
    file(GLOB EN_LANG_FILES ${CMAKE_CURRENT_SOURCE_DIR}/lang/en/*.json)

    foreach(file ${CN_LANG_FILES})
        file(COPY ${file} DESTINATION ${EXECUTABLE_OUTPUT_PATH}/lang/cn)
    endforeach()

    foreach(file ${CN_DEC_LANG_FILES})
        file(COPY ${file} DESTINATION ${EXECUTABLE_OUTPUT_PATH}/lang/cn/dec)
    endforeach()

    foreach(file ${EN_LANG_FILES})
        file(COPY ${file} DESTINATION ${EXECUTABLE_OUTPUT_PATH}/lang/en)
    endforeach()

    message(STATUS "Language files copied to: ${EXECUTABLE_OUTPUT_PATH}/lang")

    # Copy decoder files to output directory
    file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/decoders)
    file(GLOB DECODER_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/package/decoders/*)
    foreach(dir ${DECODER_DIRS})
        if(IS_DIRECTORY ${dir})
            get_filename_component(dir_name ${dir} NAME)
            file(COPY ${dir} DESTINATION ${EXECUTABLE_OUTPUT_PATH}/decoders)
        endif()
    endforeach()

    message(STATUS "Decoder files copied to: ${EXECUTABLE_OUTPUT_PATH}/decoders")

    file(MAKE_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}/decoders/c_decoders)

    message(STATUS "C decoder output directory: ${EXECUTABLE_OUTPUT_PATH}/decoders/c_decoders")
endif()

#===============================================================================
#= Vite web client build (optional, run: cmake --build . --target webui)
#-------------------------------------------------------------------------------

find_program(NPM_EXECUTABLE npm)

if(NPM_EXECUTABLE)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html
        COMMAND ${NPM_EXECUTABLE} install
        COMMAND ${NPM_EXECUTABLE} run build
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
        COMMENT "Building Vite web client..."
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json
    )

    add_custom_target(webui
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html
        COMMENT "Build the MCP web client (Vite)"
    )

    add_custom_target(install-webui
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/web/dist
            ${CMAKE_INSTALL_PREFIX}/bin/webui
        COMMENT "Copy web client to install directory"
        DEPENDS webui
    )

    message(STATUS "Web UI build target available: cmake --build . --target webui")
    message(STATUS "Web UI install target available: cmake --build . --target install-webui")
else()
    message(STATUS "npm not found, web UI build targets not available")
endif()

#===============================================================================
#= Installation
#-------------------------------------------------------------------------------

# Install the executable.
if(APPLE)
    install(TARGETS ${PROJECT_NAME} BUNDLE DESTINATION .)

    # Normally resources in macOS app bundle are packed at Contents/Resources
    set(MAC_RES_PREFIX ${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app/Contents/Resources/)

    # Adding icon via add_executable / target_sources does not work, hack around this
    install(FILES PXView.icns DESTINATION ${MAC_RES_PREFIX})
else()
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
endif()
install(DIRECTORY PXView/res DESTINATION ${MAC_RES_PREFIX}share/PXView)
install(DIRECTORY PXView/demo DESTINATION ${MAC_RES_PREFIX}share/PXView)
install(FILES PXView/icons/logo.svg DESTINATION ${MAC_RES_PREFIX}share/PXView RENAME logo.svg)
install(FILES PXView/icons/logo.svg DESTINATION ${MAC_RES_PREFIX}share/icons/hicolor/scalable/apps RENAME pxview.svg)
install(FILES PXView/icons/logo.svg DESTINATION ${MAC_RES_PREFIX}share/pixmaps RENAME pxview.svg)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
	install(FILES PXView/PXView.desktop DESTINATION ${MAC_RES_PREFIX}share/applications RENAME pxview.desktop)

	#add_compile_definitions(_DEFAULT_SOURCE)

	# udev rules: install to system path for system prefixes, local prefix otherwise
	if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
		if(IS_DIRECTORY /usr/lib/udev/rules.d)
			install(FILES PXView/px.rules DESTINATION /usr/lib/udev/rules.d RENAME 60-px.rules)
		elseif(IS_DIRECTORY /lib/udev/rules.d)
			install(FILES PXView/px.rules DESTINATION /lib/udev/rules.d RENAME 60-px.rules)
		elseif(IS_DIRECTORY /etc/udev/rules.d)
			install(FILES PXView/px.rules DESTINATION /etc/udev/rules.d RENAME 60-px.rules)
		endif()
	else()
		install(FILES PXView/px.rules DESTINATION ${MAC_RES_PREFIX}lib/udev/rules.d RENAME 60-px.rules)
	endif()

endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/doc/ DESTINATION ${MAC_RES_PREFIX}share/PXView)

install(DIRECTORY libsigrokdecode/decoders DESTINATION ${MAC_RES_PREFIX}share/libsigrokdecode)

foreach(dec ${C_DECODERS})
	install(TARGETS decoder_${dec}
		DESTINATION ${MAC_RES_PREFIX}share/libsigrokdecode/c_decoders
	)
endforeach()

install(TARGETS irmp DESTINATION ${MAC_RES_PREFIX}bin)

install(DIRECTORY lang DESTINATION ${MAC_RES_PREFIX}share/PXView)

# Install web client if it has been built
install(CODE "
    if(EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/web/dist/index.html\")
        file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${MAC_RES_PREFIX}bin/webui\"
             TYPE DIRECTORY FILES \"${CMAKE_CURRENT_SOURCE_DIR}/web/dist/\")
        message(STATUS \"Installing web client to: \${CMAKE_INSTALL_PREFIX}/${MAC_RES_PREFIX}bin/webui\")
    else()
        message(STATUS \"Web client not built, skipping. Run: ninja webui\")
    endif()
")

#===============================================================================
#= Packaging (handled by CPack)
#-------------------------------------------------------------------------------

set(CPACK_PACKAGE_VERSION_MAJOR ${DS_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${DS_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${DS_VERSION_MICRO})
set(CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/PXView/README)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/PXView/COPYING)
set(CPACK_SOURCE_IGNORE_FILES ${CMAKE_CURRENT_BINARY_DIR} ".gitignore" ".git")
set(CPACK_SOURCE_PACKAGE_FILE_NAME
	"${CMAKE_PROJECT_NAME}-${DS_VERSION_MAJOR}.${DS_VERSION_MINOR}.${DS_VERSION_MICRO}")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_PACKAGE_CONTACT "913461865@qq.com")
include(CPack)

#===============================================================================
#= Tests
#-------------------------------------------------------------------------------

if(ENABLE_TESTS)
	add_subdirectory(test)
	enable_testing()
	add_test(test ${CMAKE_CURRENT_BINARY_DIR}/PXView/test/PXView-test)
endif(ENABLE_TESTS)

#===============================================================================
#= decoder_test - C decoder test harness
#-------------------------------------------------------------------------------

option(BUILD_DECODER_TEST "Build the C decoder test program" OFF)

if(BUILD_DECODER_TEST)
	# Static library for libsigrokdecode so decoder_test can link against it.
	# The main PXView executable compiles these sources directly; this static
	# library exists solely to provide a linkable target for decoder_test.
	add_library(sigrokdecode_static STATIC
		${libsigrokdecode_SOURCES}
		${CMAKE_CURRENT_SOURCE_DIR}/common/log/xlog.c
	)

	target_include_directories(sigrokdecode_static PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode
		${CMAKE_CURRENT_SOURCE_DIR}/common
		${GLIB_INCLUDE_DIRS}
		${Python3_INCLUDE_DIRS}
	)

	target_link_libraries(sigrokdecode_static
		-lglib-2.0
		${PY_LIB}
	)

	# decoder_test executable
	add_executable(decoder_test
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/tests/decoder_test.c
	)

	target_include_directories(decoder_test PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}                        # for <libsigrokdecode/libsigrokdecode.h>
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode        # for internal headers via sigrokdecode_static
		${CMAKE_CURRENT_SOURCE_DIR}/libsigrokdecode/tests  # for "cJSON.h"
		${GLIB_INCLUDE_DIRS}
	)

	# CJSON_IMPLEMENTATION is needed by the cJSON single-header library.
	# It is also defined in decoder_test.c itself; the CMake definition
	# ensures it is set regardless of source-level changes.
	target_compile_definitions(decoder_test PRIVATE
		CJSON_IMPLEMENTATION
	)

	target_link_libraries(decoder_test
		sigrokdecode_static
		-lglib-2.0
	)

	set_target_properties(decoder_test PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY ${EXECUTABLE_OUTPUT_PATH}
	)
endif()



#add_executable(test_font test_font.cpp)
#target_link_libraries(test_font Qt6::Gui Qt6::Widgets)
