# - Try to find libusb-1.0
# Once done this will define
#
#  LIBUSB_1_FOUND - system has libusb
#  LIBUSB_1_INCLUDE_DIRS - the libusb include directory
#  LIBUSB_1_LIBRARIES - Link these to use libusb
#  LIBUSB_1_DEFINITIONS - Compiler switches required for using libusb
#  LIBUSB_1_INTERNAL - TRUE if using project-internal libusb submodule
#
#  Adapted from cmake-modules Google Code project
#
#  Copyright (c) 2006 Andreas Schneider <mail@cynapses.org>
#
#  (Changes for libusb) Copyright (c) 2008 Kyle Machulis <kyle@nonpolynomial.com>
#
# Redistribution and use is allowed according to the terms of the New BSD license.
#
# CMake-Modules Project New BSD License
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the
#   documentation and/or other materials provided with the distribution.
#
# * Neither the name of the CMake-Modules Project nor the names of its
#   contributors may be used to endorse or promote products derived from this
#   software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
# ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
#  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

# -----------------------------------------------------------------------------
# Priority 1: project-internal libusb submodule (libusb/CMakeLists.txt)
#
# libusb 1.0.30 (official) includes events_windows.c using
# WaitForMultipleObjects() instead of the legacy pollfd model. This eliminates
# the root cause of 24MHz streaming acquisition failures on Windows (where
# libusb_get_pollfds() returns NULL because WinUSB does not expose pollfds).
#
# The submodule is built by the root CMakeLists.txt (add_subdirectory called
# BEFORE add_subdirectory(libsigrok)). Here we only detect whether the target
# exists.
# -----------------------------------------------------------------------------
if(TARGET usb-1.0)
	set(LIBUSB_1_INTERNAL TRUE)
	set(LIBUSB_1_FOUND TRUE)
	set(LIBUSB_1_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/libusb/libusb")
	set(LIBUSB_1_LIBRARIES usb-1.0)
	if(NOT libusb_1_FIND_QUIETLY)
		message(STATUS "Found libusb-1.0: project-internal (v1.0.30 with Windows event abstraction)")
	endif()
	mark_as_advanced(LIBUSB_1_INCLUDE_DIRS LIBUSB_1_LIBRARIES LIBUSB_1_INTERNAL)
	return()
endif()

# Submodule directory exists but target wasn't built (shouldn't happen
# in normal flow, but handle gracefully).
if(EXISTS "${CMAKE_SOURCE_DIR}/libusb/CMakeLists.txt" AND NOT TARGET usb-1.0)
	message(WARNING "libusb submodule directory exists but usb-1.0 target was not built. "
		"Check that add_subdirectory(libusb) is in the root CMakeLists.txt. "
		"Falling back to system libusb.")
endif()

# -----------------------------------------------------------------------------
# Priority 2: system libusb package (fallback)
# -----------------------------------------------------------------------------
if (LIBUSB_1_LIBRARIES AND LIBUSB_1_INCLUDE_DIRS)
  # in cache already
  set(LIBUSB_FOUND TRUE)
else (LIBUSB_1_LIBRARIES AND LIBUSB_1_INCLUDE_DIRS)
  find_path(LIBUSB_1_INCLUDE_DIR
    NAMES
	    libusb.h
    PATHS
      /usr/local/include
      /opt/local/include
      /usr/include

    PATH_SUFFIXES
      libusb-1.0
  )

  find_library(LIBUSB_1_LIBRARY
    NAMES
      usb-1.0 usb
    PATHS
      /usr/local/lib64
      /opt/local/lib64
      /usr/lib64
      /usr/local/lib
      /opt/local/lib
      /usr/lib
  )

  set(LIBUSB_1_INCLUDE_DIRS
    ${LIBUSB_1_INCLUDE_DIR}
  )
  set(LIBUSB_1_LIBRARIES
    ${LIBUSB_1_LIBRARY}
)

  if (LIBUSB_1_INCLUDE_DIRS AND LIBUSB_1_LIBRARIES)
     set(LIBUSB_1_FOUND TRUE)
  endif (LIBUSB_1_INCLUDE_DIRS AND LIBUSB_1_LIBRARIES)

  if (LIBUSB_1_FOUND)
    if (NOT libusb_1_FIND_QUIETLY)
        #message(STATUS "Found libusb-1.0:")
	#  message(STATUS " - Includes: ${LIBUSB_1_INCLUDE_DIRS}")
	#  message(STATUS " - Libraries: ${LIBUSB_1_LIBRARIES}")
    endif (NOT libusb_1_FIND_QUIETLY)
  else (LIBUSB_1_FOUND)
    if (libusb_1_FIND_REQUIRED)
      message(FATAL_ERROR "Could not find libusb. Consider running 'git submodule update --init libusb' for the project-internal version.")
    endif (libusb_1_FIND_REQUIRED)
  endif (LIBUSB_1_FOUND)

  if (NOT LIBUSB_1_INTERNAL)
    message(WARNING "Using system libusb — 24MHz streaming on Windows may fail. "
      "Run 'git submodule update --init libusb' for v1.0.30 with Windows event abstraction.")
  endif()

  # show the LIBUSB_1_INCLUDE_DIRS and LIBUSB_1_LIBRARIES variables only in the advanced view
  mark_as_advanced(LIBUSB_1_INCLUDE_DIRS LIBUSB_1_LIBRARIES)

endif (LIBUSB_1_LIBRARIES AND LIBUSB_1_INCLUDE_DIRS)
