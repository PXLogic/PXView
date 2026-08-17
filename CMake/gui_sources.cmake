#===============================================================================
#= PXView GUI/View layer sources, headers and Qt wrapping
#-------------------------------------------------------------------------------

# GUI/View layer: Qt Widgets/Svg dependent sources (application entry, main window,
# toolbars, docks, dialogs, view objects, widgets, ui utilities, prop bindings,
# AppControl which references QWidget for top-window tracking).
set(PXVIEW_GUI_SOURCES
    PXView/main.cpp
    PXView/application.cpp
    PXView/pv/mainwindow/appcontrol.cpp
    PXView/pv/mainwindow/mainwindow.cpp
    PXView/pv/mainwindow/config_io.cpp
    PXView/pv/mainwindow/event_dispatcher.cpp
    PXView/pv/mainwindow/dock_manager.cpp
    PXView/pv/mainwindow/tab_manager.cpp
    PXView/pv/mainwindow/theme_manager.cpp
    PXView/pv/mainwindow/status_bar.cpp
    PXView/pv/mainwindow/shortcut_manager.cpp
    PXView/pv/mainwindow/signal_connector.cpp
    PXView/pv/mainwindow/file_ops.cpp
    PXView/pv/mainwindow/mainframe.cpp
    PXView/pv/mainwindow/submainframe.cpp
    # Session orchestration (GUI-only: TabContext/SessionManager drive view::View)
    PXView/pv/session/sessionmanager.cpp
    PXView/pv/session/tabcontext.cpp
    # View layer rendering objects
    PXView/pv/view/viewport/viewport.cpp
    PXView/pv/view/renderer/viewport_painter.cpp
    PXView/pv/view/viewport/viewport_interaction.cpp
    PXView/pv/view/viewport/viewport_drag.cpp
    PXView/pv/view/component/edge_nav_button.cpp
    PXView/pv/view/view.cpp
    PXView/pv/view/view_layout.cpp
    PXView/pv/view/view_cursors.cpp
    PXView/pv/view/view_derived_traces.cpp
    PXView/pv/view/view_signal_sync.cpp
    PXView/pv/view/view_glitch_filter.cpp
    PXView/pv/view/view_data_sync.cpp
    PXView/pv/view/view_context.cpp
    PXView/pv/view/renderer/render_pass.cpp
    PXView/pv/view/cursor/timemarker.cpp
    PXView/pv/view/signal/signal.cpp
    PXView/pv/view/signal/signalfactory.cpp
    PXView/pv/view/component/ruler.cpp
    PXView/pv/view/component/ruler_format.cpp
    PXView/pv/view/component/header.cpp
    PXView/pv/view/cursor/cursor.cpp
    PXView/pv/view/signal/logicsignal.cpp
    PXView/pv/view/signal/analogsignal.cpp
    PXView/pv/view/signal/dsosignal.cpp
PXView/pv/view/signal/dsosignal_paint.cpp
    PXView/pv/view/component/dso_trigger_config.cpp
    PXView/pv/view/component/dso_measure.cpp
    PXView/pv/view/component/dsldial.cpp
    PXView/pv/view/trace/trace.cpp
    PXView/pv/view/trace/selectableitem.cpp
    PXView/pv/view/trace/decodetrace.cpp
    PXView/pv/view/trace/decodermodel.cpp
    PXView/pv/view/trace/mathtrace.cpp
    PXView/pv/view/trace/spectrumtrace.cpp
    PXView/pv/view/trace/lissajoustrace.cpp
    PXView/pv/view/component/devmode.cpp
    PXView/pv/view/component/viewstatus.cpp
    PXView/pv/view/cursor/xcursor.cpp
    PXView/pv/view/component/pulsehistogramwidget.cpp
    PXView/pv/view/component/glitchfilterpopup.cpp
    PXView/pv/view/component/waveform_copy_helper.cpp
    PXView/pv/view/component/decoderaudioplayer.cpp
    # Toolbars
    PXView/pv/toolbars/samplingbar.cpp
    PXView/pv/toolbars/trigbar.cpp
    PXView/pv/toolbars/filebar.cpp
    PXView/pv/toolbars/logobar.cpp
    PXView/pv/toolbars/titlebar.cpp
    # Docks
    PXView/pv/dock/protocoldock.cpp
    PXView/pv/dock/triggerdock.cpp
    PXView/pv/dock/dsotriggerdock.cpp
    PXView/pv/dock/measuredock.cpp
    PXView/pv/dock/searchdock.cpp
    PXView/pv/dock/logdock.cpp
    PXView/pv/dock/deviceoptionsdock.cpp
    PXView/pv/dock/mcpcontroldock.cpp
    PXView/pv/dock/functiondock.cpp
    PXView/pv/dock/protocolitemlayer.cpp
    PXView/pv/dock/keywordlineedit.cpp
    PXView/pv/dock/searchcombobox.cpp
    # Dialogs
    PXView/pv/dialogs/deviceoptions.cpp
    PXView/pv/dialogs/about.cpp
    PXView/pv/dialogs/search.cpp
    PXView/pv/dialogs/storeprogress.cpp
    PXView/pv/dialogs/dsomeasure.cpp
    PXView/pv/dialogs/protocollist.cpp
    PXView/pv/dialogs/protocolexp.cpp
    PXView/pv/dialogs/fftoptions.cpp
    PXView/pv/dialogs/dsmessagebox.cpp
    PXView/pv/dialogs/shadow.cpp
    PXView/pv/dialogs/pxdialog.cpp
    PXView/pv/dialogs/interval.cpp
    PXView/pv/dialogs/lissajousoptions.cpp
    PXView/pv/dialogs/mathoptions.cpp
    PXView/pv/dialogs/regionoptions.cpp
    PXView/pv/dialogs/applicationpardlg.cpp
    PXView/pv/dialogs/decoderoptionsdlg.cpp
    # Custom widgets
    PXView/pv/widgets/border.cpp
    PXView/pv/widgets/slidingdrawer.cpp
    PXView/pv/widgets/smoothscrollbar.cpp
    PXView/pv/widgets/smoothscrollarea.cpp
    PXView/pv/widgets/smoothtablehelper.cpp
    PXView/pv/widgets/sidebar.cpp
    PXView/pv/widgets/sidebarbutton.cpp
    PXView/pv/widgets/fakelineedit.cpp
    PXView/pv/widgets/searchpatterninput.cpp
    PXView/pv/widgets/decodermenu.cpp
    PXView/pv/widgets/decodergroupbox.cpp
    # UI utilities
    PXView/pv/ui/msgbox.cpp
    PXView/pv/ui/toast.cpp
    PXView/pv/ui/dscombobox.cpp
    PXView/pv/ui/dsspinbox.cpp
    PXView/pv/ui/langresource.cpp
    PXView/pv/ui/fn.cpp
    PXView/pv/ui/xtoolbutton.cpp
    PXView/pv/ui/draggabletabbar.cpp
    PXView/pv/ui/draggabletabwidget.cpp
    PXView/pv/ui/uimanager.cpp
    PXView/pv/ui/popupdlglist.cpp
    PXView/pv/ui/iconcache.cpp
    PXView/pv/ui/widgetinspector.cpp
    # Property bindings (Qt property system tied to widgets)
    PXView/pv/prop/property.cpp
    PXView/pv/prop/int.cpp
    PXView/pv/prop/enum.cpp
    PXView/pv/prop/double.cpp
    PXView/pv/prop/bool.cpp
    PXView/pv/prop/string.cpp
    PXView/pv/prop/binding/binding.cpp
    PXView/pv/prop/binding/deviceoptions.cpp
    PXView/pv/prop/binding/decoderoptions.cpp
    PXView/pv/prop/binding/probeoptions.cpp
)
# Backwards-compat alias so any code referencing PXView_SOURCES still works.
set(PXView_SOURCES ${PXVIEW_CORE_SOURCES} ${PXVIEW_GUI_SOURCES})

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
	list(APPEND PXVIEW_GUI_SOURCES
		PXView/pv/platform/winnativewidget.cpp
		PXView/pv/platform/winshadow.cpp
		PXView/pv/platform/wintaskbarprogress.cpp
	)
	list(APPEND PXView_HEADERS
		PXView/pv/platform/wintaskbarprogress.h
	)
	endif ()

set(PXView_HEADERS
    PXView/mystyle.h
    PXView/pv/base/log.h
    PXView/pv/session/sigsession.h
    PXView/pv/session/sessionmanager.h
    PXView/pv/interface/icontextaware.h
    PXView/pv/mainwindow/mainwindow.h
    PXView/pv/mainwindow/config_io.h
    PXView/pv/mainwindow/event_dispatcher.h
    PXView/pv/mainwindow/dock_manager.h
    PXView/pv/mainwindow/tab_manager.h
    PXView/pv/mainwindow/theme_manager.h
    PXView/pv/mainwindow/status_bar.h
    PXView/pv/mainwindow/shortcut_manager.h
    PXView/pv/mainwindow/signal_connector.h
    PXView/pv/mainwindow/file_ops.h
    PXView/pv/dialogs/deviceoptions.h
    PXView/pv/prop/property.h
    PXView/pv/prop/int.h
    PXView/pv/prop/enum.h
    PXView/pv/prop/double.h
    PXView/pv/prop/bool.h
    PXView/pv/toolbars/samplingbar.h
    PXView/pv/view/viewport/viewport.h
    PXView/pv/view/renderer/viewport_painter.h
    PXView/pv/view/viewport/viewport_interaction.h
    PXView/pv/view/viewport/viewport_drag.h
    PXView/pv/view/renderer/render_pass.h
    PXView/pv/view/iview_delegates.h
    PXView/pv/view/component/edge_nav_button.h
    PXView/pv/view/view.h
    PXView/pv/view/dock_ui_state.h
    PXView/pv/view/cursor/timemarker.h
    PXView/pv/view/component/ruler.h
    PXView/pv/view/component/header.h
    PXView/pv/view/cursor/cursor.h
    PXView/pv/toolbars/trigbar.h
    PXView/pv/toolbars/filebar.h
    PXView/pv/dock/protocoldock.h
    PXView/pv/dock/triggerdock.h
    PXView/pv/dock/dsotriggerdock.h
    PXView/pv/dock/measuredock.h
    PXView/pv/dock/searchdock.h
    PXView/pv/dock/logdock.h
    PXView/pv/dock/deviceoptionsdock.h
    PXView/pv/dock/mcpcontroldock.h
    PXView/pv/dock/functiondock.h
    PXView/pv/toolbars/logobar.h
    PXView/pv/dialogs/about.h
    PXView/pv/dialogs/search.h
    PXView/pv/view/trace/trace.h
    PXView/pv/view/trace/paint_context.h
    PXView/pv/view/trace/selectableitem.h
    PXView/pv/data/stack/decoderstack.h
    PXView/pv/view/trace/decodetrace.h
    PXView/pv/view/trace/decodermodel.h
    PXView/pv/widgets/fakelineedit.h
    PXView/pv/widgets/searchpatterninput.h
    PXView/pv/widgets/decodermenu.h
    PXView/pv/widgets/decodergroupbox.h
    PXView/pv/prop/string.h
    PXView/pv/dialogs/storeprogress.h
    PXView/pv/session/storesession.h
    PXView/pv/view/component/devmode.h
    PXView/pv/dialogs/dsomeasure.h
    PXView/pv/dialogs/protocollist.h
    PXView/pv/dialogs/protocolexp.h
    PXView/pv/dialogs/fftoptions.h
    PXView/pv/data/stack/mathstack.h
    PXView/pv/view/trace/mathtrace.h
    PXView/pv/view/component/viewstatus.h
    PXView/pv/toolbars/titlebar.h
    PXView/pv/mainwindow/mainframe.h
    PXView/pv/mainwindow/submainframe.h
    PXView/pv/widgets/border.h
    PXView/pv/widgets/slidingdrawer.h
    PXView/pv/widgets/smoothscrollbar.h
    PXView/pv/widgets/smoothscrollarea.h
    PXView/pv/widgets/smoothtablehelper.h
    PXView/pv/widgets/sidebar.h
    PXView/pv/widgets/sidebarbutton.h
    PXView/pv/dialogs/dsmessagebox.h
    PXView/pv/dialogs/shadow.h
    PXView/pv/dialogs/pxdialog.h
    PXView/pv/dialogs/interval.h
    PXView/pv/dialogs/lissajousoptions.h
    PXView/pv/view/trace/lissajoustrace.h
    PXView/pv/view/trace/spectrumtrace.h
    PXView/pv/data/stack/spectrumstack.h
    PXView/pv/data/datasource.h
    PXView/pv/data/model/signalmodel.h
    PXView/pv/data/model/signallistmodel.h
    PXView/pv/data/document/sessionsnapshot.h
    PXView/pv/data/document/sessiondocument.h
    PXView/pv/dialogs/mathoptions.h
    PXView/pv/dialogs/regionoptions.h
    PXView/pv/view/cursor/xcursor.h
    PXView/pv/view/component/pulsehistogramwidget.h
    PXView/pv/view/component/glitchfilterpopup.h
    PXView/pv/view/component/waveform_copy_helper.h
PXView/pv/view/component/decoderaudioplayer.h
    PXView/pv/view/signal/signal.h
    PXView/pv/view/signal/logicsignal.h
    PXView/pv/view/signal/analogsignal.h
    PXView/pv/view/signal/dsosignal.h
    PXView/pv/view/component/dso_trigger_config.h
    PXView/pv/view/component/dso_measure.h
    PXView/pv/dock/protocoldock.h
    PXView/pv/data/stack/decoderstack.h
    PXView/pv/view/trace/decodetrace.h
    PXView/pv/widgets/decodergroupbox.h
    PXView/pv/widgets/decodermenu.h
    PXView/pv/config/appconfig.h
    PXView/pv/mainwindow/appcontrol.h
    PXView/pv/base/dstimer.h
    PXView/pv/base/eventobject.h
    PXView/pv/base/ZipMaker.h
    PXView/pv/data/decode/annotationrestable.h
    PXView/pv/data/decode/decoderstatus.h
    PXView/pv/dock/protocolitemlayer.h
    PXView/pv/ui/msgbox.h
    PXView/pv/ui/toast.h
    PXView/pv/ui/dscombobox.h
    PXView/pv/ui/dsspinbox.h
    PXView/pv/base/pxvdef.h
    PXView/pv/dialogs/applicationpardlg.h
    PXView/pv/dock/keywordlineedit.h
    PXView/pv/dock/searchcombobox.h
    PXView/pv/dialogs/decoderoptionsdlg.h
    PXView/pv/utility/encoding.h
    PXView/pv/utility/path.h
    PXView/pv/utility/array.h
    PXView/pv/session/deviceagent.h
    PXView/pv/ui/fn.h
    PXView/pv/ui/xtoolbutton.h
    PXView/pv/ui/draggabletabbar.h
    PXView/pv/ui/draggabletabwidget.h
    PXView/pv/ui/iconcache.h
    PXView/pv/ui/widgetinspector.h
    PXView/pv/session/tabcontext.h
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
		PXView/pv/platform/winnativewidget.h
		PXView/pv/platform/winshadow.h
	)
endif ()

set(PXView_HEADERS_NO_MOC
    PXView/pv/base/log.h
    PXView/pv/session/sigsession.h
    PXView/pv/session/sessionmanager.h
    PXView/pv/interface/icontextaware.h
    PXView/pv/data/datasource.h
    PXView/pv/data/document/sessionsnapshot.h
    PXView/pv/data/document/sessiondocument.h
    PXView/pv/config/appconfig.h
    PXView/pv/mainwindow/appcontrol.h
    PXView/pv/base/ZipMaker.h
    PXView/pv/data/decode/annotationrestable.h
    PXView/pv/data/decode/decoderstatus.h
    PXView/pv/ui/msgbox.h
    PXView/pv/base/pxvdef.h
    PXView/pv/utility/encoding.h
    PXView/pv/utility/path.h
    PXView/pv/utility/array.h
    PXView/pv/session/deviceagent.h
    PXView/pv/ui/fn.h
    PXView/pv/ui/iconcache.h
    PXView/pv/session/tabcontext.h
)

if(WIN32)
    list(APPEND PXView_HEADERS_NO_MOC PXView/pv/platform/winnativewidget.h)
endif()

list(REMOVE_ITEM PXView_HEADERS ${PXView_HEADERS_NO_MOC})

#===============================================================================
#= compile config (forms / resources / Qt wrapping)
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
        list(APPEND PXVIEW_GUI_SOURCES applogo.rc)
	# Unicode support for Windows API
	add_definitions(-DUNICODE -D_UNICODE)
endif()

qt6_wrap_cpp(PXView_HEADERS_MOC ${PXView_HEADERS})
qt6_add_resources(PXView_RESOURCES_RCC ${PXView_RESOURCES})
