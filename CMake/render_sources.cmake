#===============================================================================
#= pxview-render sources (QML migration Phase 3, Task 3.0)
#-------------------------------------------------------------------------------
# Widget-free rendering assets compiled ONCE into the pxview-render STATIC
# library and linked by both front-end shells (PXView.exe Widgets UI and
# PXViewQml Quick UI) — "one library, many shells".
#
# HARD CONSTRAINT: zero Qt Widgets. Files in this list must not include
# QWidget/QDialog/QMainWindow/QScrollArea (directly or via view.h/viewport.h)
# and the target links Qt6::Gui only (painting types: QPainter/QColor/QPen...).
#
# DEBT LEDGER (Task 3.1 RenderContext decoupling) — rendering files that still
# hard-depend on View/Viewport (QScrollArea/QWidget) and therefore remain in
# gui_sources.cmake until their View* usage is routed through RenderContext /
# IViewLayout delegates:
#   * (cleared in Task 3.2) decodetrace.cpp create_popup() — the only
#     remaining widget-bound DecodeTrace code — now lives in
#     pv/view/trace/decodetrace_popup.cpp (gui_sources.cmake). decodetrace.cpp
#     itself (paint pipeline) compiles into pxview-render.
# Task 3.1 (done): render_pass.cpp, viewport_painter.cpp, trace.cpp,
# selectableitem.cpp, signal.cpp, logicsignal.cpp, analogsignal.cpp,
# dsosignal.cpp, dsosignal_paint.cpp, mathtrace.cpp, spectrumtrace.cpp,
# lissajoustrace.cpp migrated here — View/Viewport access now goes through
# the widget-free IRenderView / IRenderViewport interfaces (iview_delegates.h).
# Task 3.2 (done): ruler_format.cpp, timemarker.cpp, xcursor.cpp,
# dso_trigger_config.cpp, dso_measure.cpp, decodetrace.cpp migrated here —
# View access routed through IRenderView (incl. default-no-op autoset
# linkage hooks get_hori_res/zoom/auto_trig added to IRenderView).
set(PXVIEW_RENDER_SOURCES
    PXView/pv/view/renderer/rasterize.cpp
    PXView/pv/view/signal/signalfactory.cpp
    PXView/pv/view/component/dsldial.cpp
    # Task 3.2: widget-free pure time/freq formatting functions (ex-Ruler
    # statics) — shared by View/Viewport (GUI) and QmlRenderView (QML shell).
    PXView/pv/view/component/ruler_format.cpp
    PXView/pv/view/renderer/render_pass.cpp
    PXView/pv/view/renderer/viewport_painter.cpp
    PXView/pv/view/trace/selectableitem.cpp
    PXView/pv/view/trace/trace.cpp
    PXView/pv/view/trace/mathtrace.cpp
    PXView/pv/view/trace/spectrumtrace.cpp
    PXView/pv/view/trace/lissajoustrace.cpp
    PXView/pv/view/signal/signal.cpp
    PXView/pv/view/signal/logicsignal.cpp
    PXView/pv/view/signal/analogsignal.cpp
    PXView/pv/view/signal/dsosignal.cpp
    PXView/pv/view/signal/dsosignal_paint.cpp
    # Task 3.2: cursor/DSO-measure/decode paint TUs migrated from gui_sources
    # — their View access now goes through IRenderView (get_hori_res / zoom /
    # auto_trig are default-no-op autoset linkage hooks on the interface).
    # create_popup() remains GUI (decodetrace_popup.cpp).
    PXView/pv/view/cursor/timemarker.cpp
    PXView/pv/view/cursor/xcursor.cpp
    # Task 3.3: cursor.cpp migrated from gui_sources — ctor now takes the
    # widget-free IRenderView&, View/Ruler statics routed through
    # IRenderView/ruler_format. Together with ruler_paint.cpp this lets the
    # QML shell (RulerItem/QmlRenderView) paint ruler cursor labels from the
    # shared pxview-render implementation.
    PXView/pv/view/cursor/cursor.cpp
    PXView/pv/view/component/dso_trigger_config.cpp
    PXView/pv/view/component/dso_measure.cpp
    PXView/pv/view/trace/decodetrace.cpp
    # Task 3.3: widget-free ruler tick-mark painting (ex-Ruler::draw_logic/
    # draw_osc tick-mark bodies) — shared by GUI Ruler (delegating) and the
    # QML shell's RulerItem.
    PXView/pv/view/component/ruler_paint.cpp
)

# Q_OBJECT headers whose meta-objects are generated in the root CMakeLists.txt
# and compiled INTO pxview-render ("moc follows impl" rule — same pattern as
# PXVIEW_CORE_HEADERS_MOC / PXVIEW_QMLBRIDGE_HEADERS_MOC; the project does not
# use CMAKE_AUTOMOC). Task 3.1: the migrated Trace/Signal class hierarchy
# (SelectableItem → Trace → Signal → Logic/Analog/Dso + Math/Spectrum/Lissajous
# traces) is moc'd here; removed from the GUI moc list in gui_sources.cmake.
set(PXVIEW_RENDER_HEADERS_MOC
    PXView/pv/view/trace/selectableitem.h
    PXView/pv/view/trace/trace.h
    PXView/pv/view/trace/mathtrace.h
    PXView/pv/view/trace/spectrumtrace.h
    PXView/pv/view/trace/lissajoustrace.h
    PXView/pv/view/trace/decodetrace.h
    PXView/pv/view/signal/signal.h
    PXView/pv/view/signal/logicsignal.h
    PXView/pv/view/signal/analogsignal.h
    PXView/pv/view/signal/dsosignal.h
    # Task 3.2: impls moved into pxview-render — mocs follow impl.
    # Task 3.3: cursor.h joins them (cursor.cpp migrated from gui_sources).
    PXView/pv/view/cursor/timemarker.h
    PXView/pv/view/cursor/xcursor.h
    PXView/pv/view/cursor/cursor.h
)

# Widget-free headers owned by the render library (IDE/documentation listing;
# no cpp outside this library may grow a Widgets dependency through them).
set(PXVIEW_RENDER_HEADERS
    PXView/pv/view/iview_delegates.h
    PXView/pv/view/renderer/rasterize.h
    PXView/pv/view/renderer/render_pass.h
    PXView/pv/view/renderer/viewport_painter.h
    PXView/pv/view/trace/paint_context.h
    PXView/pv/view/component/ruler_format.h
    # Task 3.3: widget-free ruler tick-mark painting (ex-Ruler bodies).
    PXView/pv/view/component/ruler_paint.h
    PXView/pv/view/trace/selectableitem.h
    PXView/pv/view/trace/trace.h
    PXView/pv/view/trace/trace_visitor.h
    PXView/pv/view/trace/mathtrace.h
    PXView/pv/view/trace/spectrumtrace.h
    PXView/pv/view/trace/lissajoustrace.h
    PXView/pv/view/trace/decodetrace.h
    PXView/pv/view/cursor/timemarker.h
    PXView/pv/view/cursor/xcursor.h
    PXView/pv/view/signal/signal.h
    PXView/pv/view/signal/logicsignal.h
    PXView/pv/view/signal/analogsignal.h
    PXView/pv/view/signal/dsosignal.h
    PXView/pv/view/signal/signalfactory.h
    PXView/pv/view/signal/change_event.h
    PXView/pv/view/component/dsldial.h
)
