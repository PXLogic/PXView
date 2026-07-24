#===============================================================================
#= PXView GUI/View layer sources, headers and Qt wrapping
#-------------------------------------------------------------------------------

# GUI/View layer: Qt Widgets/Svg dependent sources (application entry, main window,
# toolbars, docks, dialogs, view objects, widgets, ui utilities, prop bindings,
# AppControl which references QWidget for top-window tracking).
set(PXVIEW_GUI_SOURCES
    PXView/main.cpp
    PXView/application.cpp
    PXView/pv/appcontrol.cpp
    PXView/pv/mainwindow.cpp
    PXView/pv/mainframe.cpp
    PXView/pv/submainframe.cpp
    # View layer rendering objects
    PXView/pv/view/viewport.cpp
    PXView/pv/view/viewport_painter.cpp
    PXView/pv/view/viewport_interaction.cpp
    PXView/pv/view/viewport_drag.cpp
    PXView/pv/view/edge_nav_button.cpp
    PXView/pv/view/view.cpp
    PXView/pv/view/view_layout.cpp
    PXView/pv/view/view_cursors.cpp
    PXView/pv/view/view_derived_traces.cpp
    PXView/pv/view/view_signal_sync.cpp
    PXView/pv/view/view_glitch_filter.cpp
    PXView/pv/view/view_data_sync.cpp
    PXView/pv/view/timemarker.cpp
    PXView/pv/view/signal.cpp
    PXView/pv/view/signalfactory.cpp
    PXView/pv/view/ruler.cpp
    PXView/pv/view/header.cpp
    PXView/pv/view/cursor.cpp
    PXView/pv/view/logicsignal.cpp
    PXView/pv/view/analogsignal.cpp
    PXView/pv/view/dsosignal.cpp
    PXView/pv/view/dso_trigger_config.cpp
    PXView/pv/view/dso_measure.cpp
    PXView/pv/view/dsldial.cpp
    PXView/pv/view/trace.cpp
    PXView/pv/view/selectableitem.cpp
    PXView/pv/view/decodetrace.cpp
    PXView/pv/view/decodermodel.cpp
    PXView/pv/view/mathtrace.cpp
    PXView/pv/view/spectrumtrace.cpp
    PXView/pv/view/lissajoustrace.cpp
    PXView/pv/view/devmode.cpp
    PXView/pv/view/viewstatus.cpp
    PXView/pv/view/xcursor.cpp
    PXView/pv/view/pulsehistogramwidget.cpp
    PXView/pv/view/glitchfilterpopup.cpp
    PXView/pv/view/waveform_copy_helper.cpp
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
    PXView/pv/view/viewport_painter.h
    PXView/pv/view/viewport_interaction.h
    PXView/pv/view/viewport_drag.h
    PXView/pv/view/edge_nav_button.h
    PXView/pv/view/view.h
    PXView/pv/view/dock_ui_state.h
    PXView/pv/view/timemarker.h
    PXView/pv/view/ruler.h
    PXView/pv/view/header.h
    PXView/pv/view/cursor.h
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
    PXView/pv/toolbars/logobar.h
    PXView/pv/dialogs/about.h
    PXView/pv/dialogs/search.h
    PXView/pv/view/trace.h
    PXView/pv/view/selectableitem.h
    PXView/pv/data/decoderstack.h
    PXView/pv/view/decodetrace.h
    PXView/pv/view/decodermodel.h
    PXView/pv/widgets/fakelineedit.h
    PXView/pv/widgets/searchpatterninput.h
    PXView/pv/widgets/decodermenu.h
    PXView/pv/widgets/decodergroupbox.h
    PXView/pv/prop/string.h
    PXView/pv/dialogs/storeprogress.h
    PXView/pv/storesession.h
    PXView/pv/view/devmode.h
    PXView/pv/dialogs/dsomeasure.h
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
    PXView/pv/dialogs/pxdialog.h
    PXView/pv/dialogs/interval.h
    PXView/pv/dialogs/lissajousoptions.h
    PXView/pv/view/lissajoustrace.h
    PXView/pv/view/spectrumtrace.h
    PXView/pv/data/spectrumstack.h
    PXView/pv/data/datasource.h
    PXView/pv/data/signalmodel.h
    PXView/pv/data/sessionsnapshot.h
    PXView/pv/data/sessiondocument.h
    PXView/pv/dialogs/mathoptions.h
    PXView/pv/dialogs/regionoptions.h
    PXView/pv/view/xcursor.h
    PXView/pv/view/pulsehistogramwidget.h
    PXView/pv/view/glitchfilterpopup.h
    PXView/pv/view/waveform_copy_helper.h
    PXView/pv/view/signal.h
    PXView/pv/view/logicsignal.h
    PXView/pv/view/analogsignal.h
    PXView/pv/view/dsosignal.h
    PXView/pv/view/dso_trigger_config.h
    PXView/pv/view/dso_measure.h
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
