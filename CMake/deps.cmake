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
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Widgets Gui GuiPrivate Svg Concurrent WebSockets)
message("----- Qt6:")
message(STATUS "	 includes:" ${Qt6Core_INCLUDE_DIRS})
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Qt6Widgets_EXECUTABLE_COMPILE_FLAGS}")
set(QT_INCLUDE_DIRS ${Qt6Gui_INCLUDE_DIRS} ${Qt6Widgets_INCLUDE_DIRS} ${Qt6Svg_INCLUDE_DIRS} ${Qt6Concurrent_INCLUDE_DIRS})
set(QT_LIBRARIES Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Svg Qt6::Concurrent Qt6::WebSockets)
# Qt libraries split by Core/View layer:
# - pxview-core (Core layer): QtCore + QtGui + QtNetwork + QtConcurrent + QtWebSockets (NO QtWidgets, NO QtSvg)
# - pxview executable (View layer): adds QtWidgets + QtSvg on top of QT_CORE_LIBS
set(QT_CORE_LIBS Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Network Qt6::Concurrent Qt6::WebSockets)
set(QT_GUI_LIBS Qt6::Widgets Qt6::Svg)
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
