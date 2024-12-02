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

cmake_minimum_required(VERSION 3.10.0)

project(PXView)

#===============================================================================
#= Config Header
#-------------------------------------------------------------------------------

set(PX_TITLE PXView)

set(PX_DESCRIPTION "A GUI for instruments of PX")

set(PX_VERSION_MAJOR 1)
set(PX_VERSION_MINOR 3)
set(PX_VERSION_MICRO 7)
set(PX_VERSION_STRING  ${PX_VERSION_MAJOR}.${PX_VERSION_MINOR}.${PX_VERSION_MICRO} )

configure_file (
	${PROJECT_SOURCE_DIR}/PXView/src/config.h.in
	${PROJECT_BINARY_DIR}/PXView/src/config.h
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

find_package(Python3 COMPONENTS Development QUIET)

if (Python3_FOUND)
	message("----- python3:")
	message(STATUS "	 includes:" ${Python3_INCLUDE_DIRS})
	message(STATUS "	 libraries:" ${Python3_LIBRARIES})
	include_directories(${Python3_INCLUDE_DIRS})
	set(PY_LIB ${Python3_LIBRARIES})
else()
     find_package(PythonLibs 3 QUIET)

     if(PYTHONLIBS_FOUND)
     	message("----- python(${PYTHONLIBS_VERSION_STRING}):")
	message(STATUS "	 includes:" ${PYTHON_INCLUDE_DIRS})
	message(STATUS "	 libraries:" ${PYTHON_LIBRARIES})
	include_directories(${PYTHON_INCLUDE_DIRS})
	set(PY_LIB ${PYTHON_LIBRARIES})
     else()
     	message(FATAL_ERROR  "Please install lib python3!")
     endif()
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
#= Qt5 or Qt6
#-------------------------------------------------------------------------------

find_package(Qt5Core QUIET)

if(Qt5Core_FOUND)
	message("----- Qt5:")
	message(STATUS "	 includes:" ${Qt5Core_INCLUDE_DIRS})
	#find_package(Qt5WinExtras REQUIRED)
	find_package(Qt5Widgets REQUIRED)
	find_package(Qt5Gui REQUIRED)
	find_package(Qt5Svg REQUIRED)
	# win32
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Qt5Widgets_EXECUTABLE_COMPILE_FLAGS}")
	if (WIN32)
		find_package(Qt5WinExtras REQUIRED)
		message(STATUS "	 libraries:" ${Qt5WinExtras_LIBRARIES})
		set(QT_INCLUDE_DIRS ${Qt5Gui_INCLUDE_DIRS} ${Qt5Widgets_INCLUDE_DIRS} ${Qt5WinExtras_INCLUDE_DIRS} ${Qt5Svg_INCLUDE_DIRS})
		set(QT_LIBRARIES Qt5::Gui Qt5::Widgets Qt5::WinExtras Qt5::Svg)
		add_definitions(${Qt5Gui_DEFINITIONS} ${Qt5Widgets_DEFINITIONS} ${Qt5WinExtras_DEFINITIONS}  ${Qt5Svg_DEFINITIONS} )
	else()
		set(QT_INCLUDE_DIRS ${Qt5Gui_INCLUDE_DIRS} ${Qt5Widgets_INCLUDE_DIRS})
		set(QT_LIBRARIES Qt5::Gui Qt5::Widgets)
		add_definitions(${Qt5Gui_DEFINITIONS} ${Qt5Widgets_DEFINITIONS})
	endif()
else()
	find_package(Qt6Core QUIET)
endif()

if(Qt6Core_FOUND)
    	message("----- Qt6:")
	message(STATUS "	 includes:" ${Qt6Core_INCLUDE_DIRS})
	find_package(Qt6Widgets REQUIRED)
	find_package(Qt6Gui REQUIRED)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Qt6Widgets_EXECUTABLE_COMPILE_FLAGS}")
	set(QT_INCLUDE_DIRS ${Qt6Gui_INCLUDE_DIRS} ${Qt6Widgets_INCLUDE_DIRS})
	set(QT_LIBRARIES Qt6::Gui Qt6::Widgets)
	add_definitions(${Qt6Gui_DEFINITIONS} ${Qt6Widgets_DEFINITIONS})
endif()

if(NOT Qt5Core_FOUND AND NOT Qt6Core_FOUND)
	message("Error!The qt5 or qt6 can't find, if them has been installed, please append the install path to CMAKE_PREFIX_PATH, the command as:")
	message("list(APPEND CMAKE_PREFIX_PATH \"xxx install path\")" )
	message(FATAL_ERROR  "Can't find Qt5 or Qt6!")
endif()

#===============================================================================
#= boost
#-------------------------------------------------------------------------------
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
#= SARibbonBar
#-------------------------------------------------------------------------------
#if (WIN32)
#set(SARibbonBar_DIR "./window/bin_qt5.15.10_GNU_x64/lib/cmake/SARibbonBar")
#endif()
#find_package(SARibbonBar REQUIRED)

#===============================================================================
#= PXView sources
#-------------------------------------------------------------------------------

set(PXView_SOURCES
	PXView/src/log.cpp
	PXView/src/main.cpp
	PXView/src/utility/path.cpp
	PXView/src/utility/array.cpp
	PXView/src/utility/encoding.cpp
	PXView/src/config/appconfig.cpp
	PXView/src/decode/rowdata.cpp
	PXView/src/decode/annotationrestable.cpp
	PXView/src/decode/decoder.cpp
	PXView/src/decode/row.cpp
	PXView/src/decode/annotation.cpp
	PXView/src/decode/decoderstatus.cpp
	PXView/src/prop/property.cpp
	PXView/src/prop/double.cpp
	PXView/src/prop/int.cpp
	PXView/src/prop/bool.cpp
	PXView/src/prop/enum.cpp
	PXView/src/prop/string.cpp
	PXView/src/prop/binding/deviceoptions.cpp
	PXView/src/prop/binding/decoderoptions.cpp
	PXView/src/prop/binding/probeoptions.cpp
	PXView/src/prop/binding/binding.cpp
	PXView/src/dialogs/protocollist.cpp
	PXView/src/dialogs/interval.cpp
	PXView/src/dialogs/search.cpp
	PXView/src/dialogs/fftoptions.cpp
	PXView/src/dialogs/waitingdialog.cpp
	PXView/src/dialogs/regionoptions.cpp
	PXView/src/dialogs/deviceoptions.cpp
	PXView/src/dialogs/decoderoptionsdlg.cpp
	PXView/src/dialogs/applicationpardlg.cpp
	PXView/src/dialogs/storeprogress.cpp
	PXView/src/dialogs/shadow.cpp
	PXView/src/dialogs/protocolexp.cpp
	PXView/src/dialogs/calibration.cpp
	PXView/src/dialogs/dsdialog.cpp
	PXView/src/dialogs/about.cpp
	PXView/src/dialogs/mathoptions.cpp
	PXView/src/dialogs/dsmessagebox.cpp
	PXView/src/dialogs/lissajousoptions.cpp
	PXView/src/dialogs/dsomeasure.cpp
	PXView/src/view/viewport.cpp
	PXView/src/view/view.cpp
	PXView/src/view/groupsignal.cpp
	PXView/src/view/devmode.cpp
	PXView/src/view/dsosignal.cpp
	PXView/src/view/timemarker.cpp
	PXView/src/view/viewstatus.cpp
	PXView/src/view/mathtrace.cpp
	PXView/src/view/trace.cpp
	PXView/src/view/logicsignal.cpp
	PXView/src/view/dsldial.cpp
	PXView/src/view/spectrumtrace.cpp
	PXView/src/view/analogsignal.cpp
	PXView/src/view/signal.cpp
	PXView/src/view/cursor.cpp
	PXView/src/view/header.cpp
	PXView/src/view/ruler.cpp
	PXView/src/view/decodetrace.cpp
	PXView/src/view/lissajoustrace.cpp
	PXView/src/view/xcursor.cpp
	PXView/src/appcore/mainframe.cpp
	PXView/src/appcore/appcontrol.cpp
	PXView/src/appcore/deviceagent.cpp
	PXView/src/appcore/mainwindow.cpp
	PXView/src/appcore/eventobject.cpp
	PXView/src/appcore/sigsession.cpp
	PXView/src/com/storesession.cpp
	PXView/src/com/zipmaker.cpp
	PXView/src/com/dstimer.cpp
	PXView/src/widgets/decodergroupbox.cpp
	PXView/src/widgets/decodermenu.cpp
	PXView/src/widgets/border.cpp
	PXView/src/widgets/fakelineedit.cpp
	PXView/src/ui/dscombobox.cpp
	PXView/src/ui/langresource.cpp
	PXView/src/ui/fn.cpp
	PXView/src/ui/msgbox.cpp
	PXView/src/dock/triggerdock.cpp
	PXView/src/dock/keywordlineedit.cpp
	PXView/src/dock/searchcombobox.cpp
	PXView/src/dock/measuredock.cpp
	PXView/src/dock/dsotriggerdock.cpp
	PXView/src/dock/searchdock.cpp
	PXView/src/dock/protocolitemlayer.cpp
	PXView/src/dock/protocoldock.cpp
	PXView/src/toolbars/samplingbar.cpp
	PXView/src/toolbars/logobar.cpp
	PXView/src/toolbars/trigbar.cpp
	PXView/src/toolbars/titlebar.cpp
	PXView/src/toolbars/filebar.cpp
	PXView/src/data/analogsnapshot.cpp
	PXView/src/data/decodermodel.cpp
	PXView/src/data/signaldata.cpp
	PXView/src/data/snapshot.cpp
	PXView/src/data/dsosnapshot.cpp
	PXView/src/data/mathstack.cpp
	PXView/src/data/logicsnapshot.cpp
	PXView/src/data/decoderstack.cpp
	PXView/src/data/spectrumstack.cpp
	PXView/src/QRibbon/QRibbon.cpp
	PXView/src/QRibbon/qribbon.ui
)

set(UIS
	PXView/src/QRibbon/qribbon.ui
)

qt5_wrap_ui(UI_HEADERS ${UIS})

set(PXView_HEADERS
	PXView/src/mystyle.h
	PXView/src/prop/double.h
	PXView/src/prop/bool.h
	PXView/src/prop/enum.h
	PXView/src/prop/property.h
	PXView/src/prop/int.h
	PXView/src/prop/string.h
	PXView/src/dialogs/deviceoptions.h
	PXView/src/dialogs/calibration.h
	PXView/src/dialogs/protocollist.h
	PXView/src/dialogs/waitingdialog.h
	PXView/src/dialogs/search.h
	PXView/src/dialogs/regionoptions.h
	PXView/src/dialogs/fftoptions.h
	PXView/src/dialogs/dsmessagebox.h
	PXView/src/dialogs/dsdialog.h
	PXView/src/dialogs/dsomeasure.h
	PXView/src/dialogs/lissajousoptions.h
	PXView/src/dialogs/about.h
	PXView/src/dialogs/shadow.h
	PXView/src/dialogs/decoderoptionsdlg.h
	PXView/src/dialogs/interval.h
	PXView/src/dialogs/protocolexp.h
	PXView/src/dialogs/storeprogress.h
	PXView/src/dialogs/mathoptions.h
	PXView/src/view/viewport.h
	PXView/src/view/mathtrace.h
	PXView/src/view/xcursor.h
	PXView/src/view/spectrumtrace.h
	PXView/src/view/decodetrace.h
	PXView/src/view/devmode.h
	PXView/src/view/ruler.h
	PXView/src/view/logicsignal.h
	PXView/src/view/lissajoustrace.h
	PXView/src/view/signal.h
	PXView/src/view/cursor.h
	PXView/src/view/viewstatus.h
	PXView/src/view/timemarker.h
	PXView/src/view/trace.h
	PXView/src/view/analogsignal.h
	PXView/src/view/dsosignal.h
	PXView/src/view/header.h
	PXView/src/view/view.h
	PXView/src/appcore/mainframe.h
	PXView/src/appcore/eventobject.h
	PXView/src/appcore/mainwindow.h
	PXView/src/com/dstimer.h
	PXView/src/com/storesession.h
	PXView/src/widgets/fakelineedit.h
	PXView/src/widgets/decodermenu.h
	PXView/src/widgets/decodergroupbox.h
	PXView/src/widgets/border.h
	PXView/src/ui/dscombobox.h
	PXView/src/dock/searchdock.h
	PXView/src/dock/protocolitemlayer.h
	PXView/src/dock/searchcombobox.h
	PXView/src/dock/triggerdock.h
	PXView/src/dock/keywordlineedit.h
	PXView/src/dock/protocoldock.h
	PXView/src/dock/measuredock.h
	PXView/src/dock/dsotriggerdock.h
	PXView/src/toolbars/samplingbar.h
	PXView/src/toolbars/trigbar.h
	PXView/src/toolbars/titlebar.h
	PXView/src/toolbars/filebar.h
	PXView/src/toolbars/logobar.h
	PXView/src/data/signaldata.h
	PXView/src/data/decoderstack.h
	PXView/src/data/spectrumstack.h
	PXView/src/data/mathstack.h
	PXView/src/QRibbon/QRibbon.h
	${UI_HEADERS}
)

#===============================================================================
#= libsigrok source
#-------------------------------------------------------------------------------
set(libsigrok4PX_SOURCES
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

#===============================================================================
#= libsigrokdecode source
#-------------------------------------------------------------------------------
set(libsigrokdecode_SOURCES
	libsigrokdecode/type_decoder.c
	libsigrokdecode/srd.c
	libsigrokdecode/module_sigrokdecode.c
	libsigrokdecode/decoder.c
	libsigrokdecode/error.c
	libsigrokdecode/exception.c
	libsigrokdecode/instance.c
	libsigrokdecode/log.c
	libsigrokdecode/session.c
	libsigrokdecode/util.c
	libsigrokdecode/version.c
)

#===============================================================================
#= common source
#-------------------------------------------------------------------------------

set(common_SOURCES
	common/minizip/zip.c
	common/minizip/unzip.c
	common/minizip/ioapi.c
	common/log/xlog.c
	common/mem/alloc.c
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
	PXView/src/QRibbon/QRibbon.qrc
)


if(WIN32)
	# Use the PXView icon for the PXView.exe executable.
	set(CMAKE_RC_COMPILE_OBJECT "${CMAKE_RC_COMPILER} -O coff -I${CMAKE_CURRENT_SOURCE_DIR} <SOURCE> <OBJECT>")
	enable_language(RC)
	# app icon
        list(APPEND PXView_SOURCES applogo.rc)
endif()

if(Qt5Core_FOUND)
	qt5_wrap_cpp(PXView_HEADERS_MOC ${PXView_HEADERS})
	qt5_wrap_ui(PXView_FORMS_HEADERS ${PXView_FORMS})
	qt5_add_resources(PXView_RESOURCES_RCC ${PXView_RESOURCES})
endif()

if(Qt6Core_FOUND)
	qt6_wrap_cpp(PXView_HEADERS_MOC ${PXView_HEADERS})
	qt6_wrap_ui(PXView_FORMS_HEADERS ${PXView_FORMS})
	qt6_add_resources(PXView_RESOURCES_RCC ${PXView_RESOURCES})
endif()


add_definitions(${QT_DEFINITIONS})
add_definitions(-Wall -Wextra -Wno-return-type -Wno-ignored-qualifiers)

if(NOT DISABLE_WERROR)
        add_definitions(-Werror)
endif()

set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99")


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
set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
add_compile_options(-O3)

#===============================================================================
#= Linker Configuration
#-------------------------------------------------------------------------------


set(PXView_LINK_LIBS
	-lz
	-lglib-2.0
	${CMAKE_THREAD_LIBS_INIT}
	${QT_LIBRARIES}
	${LIBUSB_1_LIBRARIES}
	${FFTW_LIBRARIES}
	${PY_LIB}
	#SARibbonBar
)

if(STATIC_PKGDEPS_LIBS)
	link_directories(${PKGDEPS_STATIC_LIBRARY_DIRS})
	list(APPEND PXView_LINK_LIBS ${PKGDEPS_STATIC_LIBRARIES})
if(WIN32)
	# Workaround for a MinGW linking issue.
	list(APPEND PULSEVIEW_LINK_LIBS "-llzma -llcms2")
endif()
else()
	link_directories(${PKGDEPS_LIBRARY_DIRS})
	list(APPEND PXView_LINK_LIBS ${PKGDEPS_LIBRARIES})
endif()

add_executable(${PROJECT_NAME}
	${common_SOURCES}
	${PXView_SOURCES}
	${PXView_HEADERS_MOC}
	${PXView_FORMS_HEADERS}
	${PXView_RESOURCES_RCC}
	${libsigrok4PX_SOURCES}
	${libsigrokdecode_SOURCES}
)

target_link_libraries(${PROJECT_NAME} ${PXView_LINK_LIBS})

if(WIN32)
# Pass -mwindows so that no "DOS box" will open when PulseView is started.
set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "-mwindows")
endif()

if(ENABLE_COTIRE)
	include(cotire)
	cotire(${PROJECT_NAME})
endif()

message(STATUS "Output dir: ${CMAKE_CURRENT_SOURCE_DIR}/build.dir")
set(EXECUTABLE_OUTPUT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/build.dir")

#===============================================================================
#= Installation
#-------------------------------------------------------------------------------

# Install the executable.
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(DIRECTORY PXView/res DESTINATION share/PXView)
install(DIRECTORY PXView/demo DESTINATION share/PXView)
install(FILES PXView/icons/logo.svg DESTINATION share/PXView RENAME logo.svg)
install(FILES PXView/icons/logo.svg DESTINATION share/icons/hicolor/scalable/apps RENAME pxview.svg)
install(FILES PXView/icons/logo.svg DESTINATION share/pixmaps RENAME pxview.svg)

# if(CMAKE_SYSTEM_NAME MATCHES "Linux")
# 	install(FILES PXView/PXView.desktop DESTINATION /usr/share/applications RENAME pxview.desktop)

# 	if(IS_DIRECTORY /usr/lib/udev/rules.d)
# 		install(FILES PXView/px.rules DESTINATION /usr/lib/udev/rules.d RENAME 60-px.rules)
# 	elseif(IS_DIRECTORY /lib/udev/rules.d)
# 		install(FILES PXView/px.rules DESTINATION /lib/udev/rules.d RENAME 60-px.rules)
# 	elseif(IS_DIRECTORY /etc/udev/rules.d)
# 		install(FILES PXView/px.rules DESTINATION /etc/udev/rules.d RENAME 60-px.rules)
# 	endif()

# endif()

install(FILES doc/NEWS25 DESTINATION share/PXView RENAME NEWS25)
install(FILES doc/NEWS31 DESTINATION share/PXView RENAME NEWS31)
install(FILES doc/ug25.pdf DESTINATION share/PXView RENAME ug25.pdf)
install(FILES doc/ug31.pdf DESTINATION share/PXView RENAME ug31.pdf)

install(DIRECTORY libsigrokdecode/decoders DESTINATION share/PXView/libsigrokdecode)
install(DIRECTORY lang DESTINATION share/PXView)

#===============================================================================
#= Packaging (handled by CPack)
#-------------------------------------------------------------------------------

set(CPACK_PACKAGE_VERSION_MAJOR ${PX_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PX_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PX_VERSION_MICRO})
set(CPACK_PACKAGE_DESCRIPTION_FILE ${CMAKE_CURRENT_SOURCE_DIR}/PXView/README)
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/PXView/COPYING)
set(CPACK_SOURCE_IGNORE_FILES ${CMAKE_CURRENT_BINARY_DIR} ".gitignore" ".git")
set(CPACK_SOURCE_PACKAGE_FILE_NAME
	"${CMAKE_PROJECT_NAME}-${PX_VERSION_MAJOR}.${PX_VERSION_MINOR}.${PX_VERSION_MICRO}")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_PACKAGE_CONTACT "913461865@qq.com")
include(CPack)


