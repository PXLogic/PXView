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
        COMMAND ${NPM_EXECUTABLE} install --registry=https://registry.npmjs.org/
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
#= Tauri desktop app build (optional, run: cmake --build . --target tauri-desktop)
#
#= Builds the PXView Agent desktop wrapper (Tauri v2 + React/Vite).
#= The Tauri binary wraps the web UI and spawns PXView --headless on startup.
#= When enabled, the binary is installed alongside PXView and provides a
#= native desktop window for the AI agent interface.
#
#= Prerequisites (build-time):
#=   - Rust toolchain (rustc, cargo) in PATH  [Cargo.toml: rust-version = "1.77"]
#=   - Node.js 18+ + npm (for frontend Vite build)
#=   - Tauri CLI v2 (installed via npm devDependencies)
#
#= Platform Compatibility (runtime minimums):
#
#=   Windows:
#=     Minimum:  Windows 10 1809 (build 17763, Oct 2018 Update)
#=     WebView2 Runtime requirement (preinstalled on Win11 and Win10 since
#=     April 2023; for older Win10 the downloadBootstrapper mode auto-installs).
#=     PXView also requires UCRT (MinGW-w64 UCRT64 target) + Python 3.14,
#=     both of which require Windows 10+. Windows 7/8/8.1 are NOT supported.
#
#=   Linux:
#=     Minimum:  Ubuntu 22.04 (jammy) / Debian 12 (bookworm) / Fedora 36
#=     Tauri v2 requires webkit2gtk-4.1 (NOT 4.0). Ubuntu 20.04 only has
#=     webkit2gtk-4.0 and is NOT supported. AppImage bundles libstdc++/libgcc_s
#=     but webkit2gtk must be installed on the target system.
#=     Also requires: libgtk-3, librsvg, libayatana-appindicator3.
#
#=   macOS:
#=     Minimum:  macOS 14.0 (Sonoma) — matches PXView CMAKE_OSX_DEPLOYMENT_TARGET.
#=     Tauri uses WKWebView (system WebKit framework, no extra deps).
#=     Tauri v2 itself can target macOS 10.15+, but since PXView requires 14.0,
#=     that is the effective binding constraint.
#
#= Summary table:
#=   +---------+-------------------+-------------------------------------------+
#=   | Platform| Minimum OS        | WebView runtime                           |
#=   +---------+-------------------+-------------------------------------------+
#=   | Windows | Win10 1809 (17763)| WebView2 (auto-download bootstrapper)     |
#=   | Linux   | Ubuntu 22.04      | webkit2gtk-4.1 (system-installed)         |
#=   | macOS   | macOS 14.0 Sonoma | WKWebView (system framework)              |
#=   +---------+-------------------+-------------------------------------------+
#-------------------------------------------------------------------------------

if(ENABLE_TAURI)
    find_program(CARGO_EXECUTABLE cargo)
    find_program(RUSTC_EXECUTABLE rustc)

    if(NPM_EXECUTABLE AND CARGO_EXECUTABLE AND RUSTC_EXECUTABLE)
        # The Tauri binary name from Cargo.toml package name
        if(WIN32)
            set(TAURI_BIN_NAME "pxview-agent.exe")
        else()
            set(TAURI_BIN_NAME "pxview-agent")
        endif()

        # Output location of the release binary (cargo build --release)
        set(TAURI_BIN_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/web/src-tauri/target/release/${TAURI_BIN_NAME}")

        # The Tauri build uses `npx tauri build --no-bundle` which:
        #   1. Runs the beforeBuildCommand (npm run build → Vite → web/dist/)
        #   2. Compiles Rust code in release mode (cargo build --release)
        #   3. Skips platform bundling (NSIS/deb/dmg) — we do our own packaging
        #
        # The --no-bundle flag requires Tauri CLI v2.1+; for older versions,
        # the binary is still produced at target/release/ even if bundling fails.
        add_custom_command(
            OUTPUT ${TAURI_BIN_PATH}
            COMMAND ${NPM_EXECUTABLE} install
            COMMAND ${NPM_EXECUTABLE} run tauri -- build --no-bundle
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web
            COMMENT "Building Tauri desktop app (PXView Agent)..."
            DEPENDS
                ${CMAKE_CURRENT_SOURCE_DIR}/web/package.json
                ${CMAKE_CURRENT_SOURCE_DIR}/web/src-tauri/Cargo.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/web/src-tauri/tauri.conf.json
        )

        add_custom_target(tauri-desktop
            DEPENDS ${TAURI_BIN_PATH}
            COMMENT "Build the Tauri desktop wrapper (PXView Agent)"
        )

        # Make `ninja` (default target = PXView) also build the Tauri desktop app.
        # This way users don't need to run `ninja tauri-desktop` separately —
        # a plain `ninja` will compile both PXView and the Tauri wrapper.
        add_dependencies(${PROJECT_NAME} tauri-desktop)

        # Standalone install target for CI convenience
        add_custom_target(install-tauri-desktop
            COMMAND ${CMAKE_COMMAND} -E copy
                ${TAURI_BIN_PATH}
                ${CMAKE_INSTALL_PREFIX}/bin/PXView-Agent${CMAKE_EXECUTABLE_SUFFIX}
            COMMENT "Copy Tauri desktop app to install directory"
            DEPENDS tauri-desktop
        )

        message(STATUS "Tauri desktop build target available: cmake --build . --target tauri-desktop")
        message(STATUS "Tauri binary will be installed as: PXView-Agent${CMAKE_EXECUTABLE_SUFFIX}")
    else()
        if(NOT NPM_EXECUTABLE)
            message(WARNING "ENABLE_TAURI is ON but npm not found. Install Node.js.")
        endif()
        if(NOT CARGO_EXECUTABLE OR NOT RUSTC_EXECUTABLE)
            message(WARNING "ENABLE_TAURI is ON but cargo/rustc not found. "
                "Install Rust toolchain from https://rustup.rs/ or disable ENABLE_TAURI.")
        endif()
    endif()
else()
    message(STATUS "Tauri desktop build disabled (enable with -DENABLE_TAURI=ON)")
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

# Install libsigrok shared library (upstream libsigrok 0.6.0 as DLL/SO/dylib).
# Windows: libsigrok.dll -> bin/ (alongside PXView.exe so the loader finds it)
# Linux:   libsigrok.so  -> lib/
# macOS:   libsigrok.dylib -> lib/ (Framework bundling handled separately if needed)
# Windows .dll.a import library -> lib/ (development only, not required at runtime)
install(TARGETS libsigrok
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Windows: copy libffi-8.dll (needed by Python's _ctypes module).
# Without this, decoders that import ctypes (ir_irmp, iso7816) fail at runtime
# and the application may crash due to corrupted Python state.
if(WIN32)
    find_file(LIBFFI_DLL NAMES libffi-8.dll libffi-7.dll libffi-6.dll
        PATHS /ucrt64/bin "$ENV{MINGW_PREFIX}/bin"
        NO_DEFAULT_PATH)
    if(NOT LIBFFI_DLL)
        find_file(LIBFFI_DLL NAMES libffi-8.dll libffi-7.dll libffi-6.dll)
    endif()
    if(LIBFFI_DLL)
        install(FILES ${LIBFFI_DLL} DESTINATION bin)
        message(STATUS "libffi DLL found: ${LIBFFI_DLL} -> bin/")
    else()
        message(WARNING "libffi DLL not found. Python _ctypes module will fail "
            "at runtime. Install libffi (e.g. 'pacman -S mingw-w64-ucrt-x86_64-libffi').")
    endif()
endif()

# Install sigrok firmware files (redistributable, from git submodules).
# Sources (see sigrok-firmware/README and sigrok-firmware-fx2lafw/README):
#   - asix-sigma + sysclk-lwla: sigrok-firmware submodule (vendor permits redistribution)
#   - fx2lafw: sigrok-firmware-fx2lafw submodule, built with sdcc (GPLv2+, see build_fx2lafw.sh)
# libsigrok's sr_resourcepaths_get() searches share/sigrok-firmware/ for firmware at runtime.
#
# asix-sigma + sysclk-lwla: install prebuilt .fw/.rbf directly from submodule
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sigrok-firmware/asix-sigma/
    DESTINATION share/sigrok-firmware
    FILES_MATCHING PATTERN "*.fw" PATTERN "LICENSE.*"
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/sigrok-firmware/sysclk-lwla/
    DESTINATION share/sigrok-firmware
    FILES_MATCHING PATTERN "*.rbf" PATTERN "LICENSE.*"
)

# fx2lafw: if prebuilt .fw files exist in submodule (built by build_fx2lafw.sh), install them.
# Otherwise warn the user — they need to run build_fx2lafw.sh (requires sdcc).
file(GLOB FX2LAFW_PREBUILT_FW
    "${CMAKE_SOURCE_DIR}/sigrok-firmware-fx2lafw/hw/*/fx2lafw-*.fw"
)
if(FX2LAFW_PREBUILT_FW)
    # Flatten all fx2lafw-*.fw files to the root of share/sigrok-firmware/.
    # libsigrok's resource.c (try_open_file) only looks in the root directory
    # (subdir=NULL), not in subdirectories. The sigrok-firmware-fx2lafw/hw/
    # layout uses vendor subdirs (saleae-logic/, cypress-fx2/, ...), which
    # would break firmware lookup if installed with DIRECTORY (preserving
    # subdirs). Use FILES to install all .fw files flat into the root.
    install(FILES ${FX2LAFW_PREBUILT_FW}
        DESTINATION share/sigrok-firmware
    )
    install(FILES
        ${CMAKE_SOURCE_DIR}/sigrok-firmware-fx2lafw/COPYING
        ${CMAKE_SOURCE_DIR}/sigrok-firmware-fx2lafw/COPYING.LESSER
        DESTINATION share/sigrok-firmware/fx2lafw-license
    )
else()
    message(WARNING
        "fx2lafw firmware not built. Run 'bash build_fx2lafw.sh' (requires sdcc) "
        "to build 15 fx2lafw-*.fw files for Cypress FX2 devices. "
        "Install will skip fx2lafw firmware; only asix-sigma + sysclk-lwla will be installed."
    )
endif()
install(DIRECTORY PXView/res DESTINATION ${MAC_RES_PREFIX}share/PXView)
install(DIRECTORY PXView/demo DESTINATION ${MAC_RES_PREFIX}share/PXView)
install(FILES PXView/icons/logo.svg DESTINATION ${MAC_RES_PREFIX}share/PXView RENAME logo.svg)
install(FILES PXView/icons/logo.svg DESTINATION ${MAC_RES_PREFIX}share/icons/hicolor/scalable/apps RENAME pxview.svg)
install(FILES PXView/icons/logo.png DESTINATION ${MAC_RES_PREFIX}share/icons/hicolor/256x256/apps RENAME pxview.png)
install(FILES PXView/icons/logo.svg DESTINATION ${MAC_RES_PREFIX}share/pixmaps RENAME pxview.svg)
install(FILES PXView/icons/logo.png DESTINATION ${MAC_RES_PREFIX}share/pixmaps RENAME pxview.png)

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

# Install Tauri desktop app if it has been built
# The binary is placed alongside PXView so it can find and spawn PXView --headless
if(ENABLE_TAURI AND NPM_EXECUTABLE AND CARGO_EXECUTABLE AND RUSTC_EXECUTABLE)
    install(CODE "
        if(EXISTS \"${TAURI_BIN_PATH}\")
            file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${MAC_RES_PREFIX}bin\"
                 TYPE FILE FILES \"${TAURI_BIN_PATH}\"
                 RENAME \"PXView-Agent${CMAKE_EXECUTABLE_SUFFIX}\")
            message(STATUS \"Installing Tauri desktop app to: \${CMAKE_INSTALL_PREFIX}/${MAC_RES_PREFIX}bin/PXView-Agent${CMAKE_EXECUTABLE_SUFFIX}\")
        else()
            message(STATUS \"Tauri desktop app not built, skipping. Run: ninja tauri-desktop\")
        endif()
    ")
endif()

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
