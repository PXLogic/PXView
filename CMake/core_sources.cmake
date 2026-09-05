#===============================================================================
#= PXView sources — split into Core (no Qt Widgets) and GUI (View) layers
#-------------------------------------------------------------------------------

# pxview-core: Core layer (no Qt Widgets dependency).
# Contains: SigSession, DeviceAgent, SignalModel/LissajousModel, DecoderStack/
# SpectrumStack/MathStack, DataSource, SessionDocument/SessionSnapshot,
# SessionService/RpcDispatcher/Transports, utility, config.
# AppControl lives here (pv/core/appcontrol.cpp): its QWidget top-window members
# were moved to the View-layer TopWindowTracker, so the class no longer pulls
# Qt Widgets into the headless (pxviewd) build.

#=== View subdirectories and top-level files FORBIDDEN in PXVIEW_CORE_SOURCES ===
# Adding any of the below to the Core layer breaks the headless build (pulls in
# Qt Widgets/Svg) and violates the Core/View responsibility boundary documented
# in AGENTS.md "## Core/View Responsibility Boundary".
#
# Forbidden View subdirectories (do NOT add any file from these to PXVIEW_CORE_SOURCES):
#   PXView/pv/view/                  View container, Signal/Trace subclasses, viewport, cursor, renderer, component
#   PXView/pv/mainwindow/            MainWindow + delegates (config_io/dock_manager/tab_manager/etc.) + TopWindowTracker + MainFrame
#   PXView/pv/platform/              Windows-specific: WinNativeWidget/WinShadow/WinTaskbarProgress (WIN32 only)
#   PXView/pv/dialogs/               DecoderOptionsDlg and other dialogs
#   PXView/pv/dock/                  MeasureDock/TriggerDock/SearchDock/ProtocolDock/etc.
#   PXView/pv/toolbars/              SamplingBar/TrigBar/FileBar/LogoBar/TitleBar
#   PXView/pv/widgets/               Custom widgets: border/slidingdrawer/sidebar/etc.
#   PXView/pv/ui/                    Ui utilities: msgbox/toast/dscombobox/dsspinbox/etc.
#   PXView/pv/prop/                  Property editors + binding/
#
# NOTE: There are no longer any top-level .cpp/.h files directly under PXView/pv/.
# All files have been moved into subdirectories during the directory restructuring.
set(PXVIEW_CORE_SOURCES
    # Core session/orchestration
    PXView/pv/base/log.cpp
    PXView/pv/core/eventbus.cpp
    PXView/pv/core/qt_async_dispatcher.cpp
    PXView/pv/core/sessionstatecontext.cpp
    PXView/pv/core/filterprocessor.cpp
    PXView/pv/core/decodetaskmanager.cpp
    PXView/pv/core/datafeedparser.cpp
    PXView/pv/core/documentregistry.cpp
    PXView/pv/core/capturemanager.cpp
    PXView/pv/core/scheduler_thread.cpp # P6: capture-cadence timers on a dedicated thread
    PXView/pv/core/measurecalculator.cpp  # Task C1.9: DSO measurement computation (Core layer)
    PXView/pv/core/measure_format.cpp     # Extracted pure formatting functions for testability
    PXView/pv/core/cursorregistry.cpp     # Task C2.8: cursor position state (Core layer)
    PXView/pv/session/sigsession.cpp
    PXView/pv/session/deviceagent.cpp
    PXView/pv/base/dstimer.cpp
    PXView/pv/base/eventobject.cpp
    PXView/pv/base/pxvdef.cpp
    PXView/pv/base/ZipMaker.cpp
    PXView/pv/session/storesession.cpp
    # Application lifecycle + API transports owner. Widget-free since the
    # top-window pointer moved to pv::mainwindow TopWindowTracker.
    PXView/pv/core/appcontrol.cpp
    # Localized string table (pure QtCore: QFile + QJson). Previously lived in
    # pv/ui/ which dragged a Widgets-free class into the GUI layer and made the
    # headless link fail on LangResource symbols.
    PXView/pv/core/langresource.cpp
    # Headless runtime shared by the GUI binary (`PXView --headless`) and the
    # console pxviewd binary, so both stay behaviourally identical.
    PXView/pv/core/headless_run.cpp
    # Optional UI services injected by the GUI binary (file dialogs); headless
    # builds get documented no-UI fallbacks instead of a Widgets dependency.
    PXView/pv/core/ui_hooks.cpp
    # NOTE: data/*.cpp live in pv/data/CMakeLists.txt (pxview-data STATIC lib)
    # API/remote-control layer (SessionService, transports, RPC dispatcher)
    PXView/pv/api/session_service.cpp
    PXView/pv/api/app_service.cpp
    PXView/pv/api/rpc_dispatcher.cpp
    PXView/pv/api/ws_transport.cpp
    PXView/pv/api/mcp_transport.cpp
    PXView/pv/api/binary_codec.cpp
    # MCP SDK (tool registration, schema generation, exception-driven dispatch)
    PXView/pv/mcp/mcp_server.cpp
    PXView/pv/mcp/mcp_serializers.cpp
    PXView/pv/mcp/mcp_tool_registry.cpp
    # NOTE: utility/*.cpp live in PXView/pv/utility/CMakeLists.txt (pxview-utility STATIC lib)
    # NOTE: config/*.cpp live in PXView/pv/config/CMakeLists.txt (pxview-config STATIC lib)
)

#=== Core Q_OBJECT headers — their meta-objects belong to this library =========
# The moc_*.cpp implementations for these Core-layer QObject classes are
# generated (qt6_wrap_cpp, see CMakeLists.txt) and compiled INTO pxview-core,
# so every consumer -- the GUI executable AND the console pxviewd daemon --
# resolves their vtables/signals from the core library itself.
#
# RULE: a class's meta-object is compiled into the archive that contains its
# implementation. Splitting moc from impl across two static archives creates a
# cross-archive circular dependency (impl .obj needs the vtable, the moc .obj
# needs the impl) that single-pass GNU ld cannot resolve. Hence:
#   * classes implemented here (session/api/base/core) -> this list
#   * classes implemented in pxview-data (stacks, models)
#     -> moc'd in PXView/pv/data/CMakeLists.txt
#
# Historically all of these were moc'd in the GUI executable target only,
# which worked as long as the GUI binary was the sole linker. pxviewd broke
# that assumption, so the meta-objects moved home. Keep this list in sync when
# adding a Q_OBJECT class under pv/core|session|api|mcp.
set(PXVIEW_CORE_HEADERS_MOC
    PXView/pv/api/mcp_transport.h
    PXView/pv/api/ws_transport.h
    PXView/pv/base/dstimer.h
    PXView/pv/base/eventobject.h
    PXView/pv/core/scheduler_thread.h
    PXView/pv/session/storesession.h
)
