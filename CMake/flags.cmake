#===============================================================================
#= compile config (compiler flags / definitions / include dirs)
#-------------------------------------------------------------------------------

add_definitions(${QT_DEFINITIONS})

# Compiler warnings - only for C/C++, not for RC
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-return-type -Wno-ignored-qualifiers")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-return-type -Wno-ignored-qualifiers")

if(NOT DISABLE_WERROR)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
endif()

# C++23: required for std::expected / std::print / concepts (C++20) /
# std::span (C++20). Use CMAKE_CXX_STANDARD instead of -std=c++23 in
# CMAKE_CXX_FLAGS so that CMake appends the standard flag AFTER Qt's
# imported-target flags (which may add -std=gnu++17 on macOS, overriding
# our flag if it appears earlier on the command line).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)


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
    string(APPEND CMAKE_CXX_FLAGS " -Wall -Wextra")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
    add_compile_options(-O3)
endif()
set(CMAKE_CXX_FLAGS_DEBUG "-g")

# Enable libstdc++ runtime assertions (bounds checking on std::vector::operator[],
# std::string::operator[], iterator validation) in Debug builds only.
# Zero overhead in Release — the macro expands to nothing unless _GLIBCXX_DEBUG
# is also defined (which we deliberately do NOT enable to avoid ABI breakage).
add_compile_definitions($<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)

#===============================================================================
#= Sanitizers + Clang-Tidy (Debug mode static analysis & runtime checks)
#-------------------------------------------------------------------------------
# See options.cmake for usage. Key constraints:
#   - ASan and TSan are mutually exclusive (CMake will FATAL_ERROR if both ON)
#   - TSan is not supported on Windows/MinGW (CMake will FATAL_ERROR)
#   - Clang-Tidy runs independently of sanitizers (static analysis only)
#   - When any sanitizer is enabled, -O1 is used (not -O0/-O3) for better
#     stack traces while avoiding false positives from optimization.
#-------------------------------------------------------------------------------

# --- Mutual exclusivity check ---
if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR
        "AddressSanitizer and ThreadSanitizer are mutually exclusive.\n"
        "Use ASan for memory errors (UAF, overflow) OR TSan for data races.\n"
        "Example: cmake -DENABLE_ASAN=ON .. (NOT both)")
endif()

# --- AddressSanitizer ---
if(ENABLE_ASAN)
    message(STATUS "─── AddressSanitizer enabled ───")
    add_compile_options(
        -fsanitize=address
        -fno-omit-frame-pointer
        -fno-common
    )
    add_link_options(-fsanitize=address)
    # ASan works best with -O1 (inline enough for good stack traces,
    # but not so much that it obscures the root cause).
    add_compile_options(-O1)
    # LeakSanitizer is part of ASan on Linux/macOS (auto-enabled).
    # On Windows/MinGW it is not available, but ASan still detects UAF/overflow.
    if(WIN32)
        message(STATUS "  Note: ASan on Windows/MinGW detects UAF and buffer overflow.")
        message(STATUS "        LeakSanitizer is not available on Windows.")
        message(STATUS "        Ensure all DLLs (Qt, Python) are built with ASan for full coverage,")
        message(STATUS "        or use ASAN_OPTIONS=detect_leaks=0 to suppress leak reports.")
    endif()
endif()

# --- ThreadSanitizer ---
if(ENABLE_TSAN)
    if(WIN32)
        message(FATAL_ERROR
            "ThreadSanitizer is not supported on Windows/MinGW.\n"
            "Use AddressSanitizer instead: cmake -DENABLE_ASAN=ON ..\n"
            "For race detection, build and run on Linux/macOS with TSan.")
    endif()
    message(STATUS "─── ThreadSanitizer enabled ───")
    add_compile_options(
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=thread)
    add_compile_options(-O1)
endif()

# --- UndefinedBehaviorSanitizer ---
if(ENABLE_UBSAN)
    message(STATUS "─── UndefinedBehaviorSanitizer enabled ───")
    add_compile_options(
        -fsanitize=undefined
        -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=undefined)
    # UBSan can be used with ASan or TSan (compatible).
    # Print the full backtrace for UB violations.
    add_compile_options(-fno-sanitize-recover=all)
endif()

# --- Clang-Tidy (static analysis, independent of sanitizers) ---
if(ENABLE_CLANG_TIDY)
    # Search for clang-tidy in PATH (MSYS2, Homebrew, apt, etc.)
    find_program(CLANG_TIDY_EXE
        NAMES clang-tidy clang-tidy-22 clang-tidy-21 clang-tidy-20
              clang-tidy-19 clang-tidy-18 clang-tidy-17 clang-tidy-16
        DOC "Path to clang-tidy executable"
    )
    if(CLANG_TIDY_EXE)
        message(STATUS "─── Clang-Tidy enabled: ${CLANG_TIDY_EXE} ───")
        # Apply to all C++ targets. Uses .clang-tidy config in project root.
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE};--extra-arg=-std=c++23")
        # Also apply to C sources (libsigrokdecode, common, libsigrok)
        set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING
            "ENABLE_CLANG_TIDY=ON but clang-tidy not found in PATH.\n"
            "Install it (MSYS2: pacman -S mingw-w64-x86_64-clang-tools-extra)\n"
            "or set ENABLE_CLANG_TIDY=OFF.")
    endif()
endif()

# --- Summary ---
if(ENABLE_ASAN OR ENABLE_TSAN OR ENABLE_UBSAN OR ENABLE_CLANG_TIDY)
    message(STATUS "┌─────────────────────────────────────────────┐")
    message(STATUS "│  Debug analysis tools active:               │")
    if(ENABLE_ASAN)
        message(STATUS "│  * AddressSanitizer (runtime)               │")
    endif()
    if(ENABLE_TSAN)
        message(STATUS "│  * ThreadSanitizer (runtime)                │")
    endif()
    if(ENABLE_UBSAN)
        message(STATUS "│  * UndefinedBehaviorSanitizer (runtime)     │")
    endif()
    if(ENABLE_CLANG_TIDY)
        message(STATUS "│  * Clang-Tidy (static analysis)             │")
    endif()
    message(STATUS "│  Build type: ${CMAKE_BUILD_TYPE}                    │")
    message(STATUS "└─────────────────────────────────────────────┘")
endif()
