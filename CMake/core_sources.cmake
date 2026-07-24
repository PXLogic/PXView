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
#   PXView/pv/view/                  View container, Signal/Trace subclasses, viewport, cursor, dso_measure
#   PXView/pv/dialogs/               DecoderOptionsDlg and other dialogs
#   PXView/pv/dock/                  MeasureDock/TriggerDock/SearchDock/ProtocolDock/etc.
#   PXView/pv/toolbars/              SamplingBar/TrigBar/FileBar/LogoBar/TitleBar
#   PXView/pv/widgets/               Custom widgets: border/slidingdrawer/sidebar/etc.
#   PXView/pv/ui/                    Ui utilities: msgbox/toast/dscombobox/dsspinbox/etc.
#   PXView/pv/prop/                  Property editors + binding/
#
# Forbidden View top-level files (do NOT add to PXVIEW_CORE_SOURCES):
#   PXView/pv/mainwindow.*           QMainWindow
#   PXView/pv/tabcontext.*           Per-tab View/Session/Document binding
#   PXView/pv/mainframe.*            Main frame container
#   PXView/pv/submainframe.*         Sub-frame container
#   PXView/pv/winnativewidget.*      Windows native widget (WIN32 only)
#   PXView/pv/winshadow.*            Windows window shadow (WIN32 only)
#   PXView/pv/wintaskbarprogress.*   Windows taskbar progress (WIN32 only)
set(PXVIEW_CORE_SOURCES
    # Core session/orchestration
    PXView/pv/log.cpp
    PXView/pv/core/eventbus.cpp
    PXView/pv/core/sessionstatecontext.cpp
    PXView/pv/core/filterprocessor.cpp
    PXView/pv/core/decodetaskmanager.cpp
    PXView/pv/core/datafeedparser.cpp
    PXView/pv/core/documentregistry.cpp
    PXView/pv/core/capturemanager.cpp
    PXView/pv/core/measurecalculator.cpp  # Task C1.9: DSO measurement computation (Core layer)
    PXView/pv/core/cursorregistry.cpp     # Task C2.8: cursor position state (Core layer)
    PXView/pv/sigsession.cpp
    PXView/pv/sessionmanager.cpp
    PXView/pv/deviceagent.cpp
    PXView/pv/dstimer.cpp
    PXView/pv/eventobject.cpp
    PXView/pv/dsvdef.cpp
    PXView/pv/ZipMaker.cpp
    PXView/pv/storesession.cpp
    PXView/pv/tabcontext.cpp
    # NOTE: data/*.cpp live in pv/data/CMakeLists.txt (pxview-data STATIC lib)
    # API/remote-control layer (SessionService, transports, RPC dispatcher)
    PXView/pv/api/session_service.cpp
    PXView/pv/api/app_service.cpp
    PXView/pv/api/rpc_dispatcher.cpp
    PXView/pv/api/ws_transport.cpp
    PXView/pv/api/mcp_transport.cpp
    # NOTE: utility/*.cpp live in PXView/pv/utility/CMakeLists.txt (pxview-utility STATIC lib)
    # NOTE: config/*.cpp live in PXView/pv/config/CMakeLists.txt (pxview-config STATIC lib)
)
