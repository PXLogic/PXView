#===============================================================================
#= PXView sources — split into Core (no Qt Widgets) and GUI (View) layers
#-------------------------------------------------------------------------------

# pxview-core: Core layer (no Qt Widgets dependency).
# Contains: SigSession, DeviceAgent, SignalModel/LissajousModel, DecoderStack/
# SpectrumStack/MathStack, DataSource, SessionDocument/SessionSnapshot,
# SessionService/RpcDispatcher/Transports, utility, config.
# AppControl stays in GUI layer because it references QWidget for top-window tracking.

#=== View subdirectories and top-level files FORBIDDEN in PXVIEW_CORE_SOURCES ===
# Adding any of the below to the Core layer breaks the headless build (pulls in
# Qt Widgets/Svg) and violates the Core/View responsibility boundary documented
# in AGENTS.md "## Core/View Responsibility Boundary".
#
# Forbidden View subdirectories (do NOT add any file from these to PXVIEW_CORE_SOURCES):
#   PXView/pv/view/                  View container, Signal/Trace subclasses, viewport, cursor, renderer, component
#   PXView/pv/mainwindow/            MainWindow + delegates (config_io/dock_manager/tab_manager/etc.) + AppControl + MainFrame
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
