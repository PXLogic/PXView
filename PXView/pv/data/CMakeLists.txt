# pxview-data: data layer (snapshots, models, stacks, decode/)
# Extracted from core_sources.cmake to reduce pxview-core surface and
# allow data/ modifications without triggering core recompilation.
#
# Link note: external headers (libsigrok.h, libsigrokdecode.h, fftw3.h,
# boost/*, glib.h) are exposed via global include_directories() set in
# cmake/deps.cmake + cmake/flags.cmake. The corresponding libraries
# (glib, FFTW, etc.) are linked transitively through pxview-core's PUBLIC
# ${PXVIEW_LINK_LIBS}; libsigrok/libsigrokdecode are compiled as sources
# directly into the executable. Qt6::Core is the only direct link needed
# here (matches pxview-utility/pxview-config convention).
add_library(pxview-data STATIC
    snapshot/snapshot.cpp
    model/signaldata.cpp
    model/signalmodel.cpp
    model/signallistmodel.cpp
    datasource.cpp
    triggerconfig.cpp
    stack/lissajousmodel.cpp
    snapshot/logicsnapshot.cpp
    snapshot/logicsnapshot_diskcache_writer.cpp
    snapshot/logicsnapshot_glitch_filter.cpp
    snapshot/logicsnapshot_pattern_search.cpp
    snapshot/logicsnapshot_edge_scan.cpp
    pulse_analyzer.cpp
    pulse_analyzer.h
    snapshot/dsosnapshot.cpp
    snapshot/analogsnapshot.cpp
    document/sessionsnapshot.cpp
    document/sessiondata.cpp
    document/sessiondocument.cpp
    model/signalconfigstore.cpp
    stack/decoderstack.cpp
    stack/mathstack.cpp
    stack/spectrumstack.cpp
    decoderanalogdata.cpp
    cache/disk_buffer_manager.cpp
    cache/disk_write_thread.cpp
    cache/disk_read_cache.cpp
    cache/mmap_allocator.cpp
    decode/rowdata.cpp
    decode/row.cpp
    decode/decoder.cpp
    decode/annotation.cpp
    decode/annotationrestable.cpp
    decode/decoderstatus.cpp
)

target_link_libraries(pxview-data PUBLIC Qt6::Core)
# pxview-data uses FFTW directly (spectrumstack.cpp). Declare it here so CMake
# places ${FFTW_LIBRARIES} AFTER libpxview-data.a in the final link line —
# required for static-lib symbol resolution (the linker processes left-to-right
# and would otherwise discard fftw_* symbols before pxview-data references them).
target_link_libraries(pxview-data PUBLIC ${FFTW_LIBRARIES})

# PRIVATE external libs: must follow libpxview-data.a on the link line so the
# linker resolves data-layer references (fftw_* in spectrumstack.cpp, g_* in
# signalmodel/decoderstack/signalconfigstore/decode/decoder.cpp). libsigrok /
# libsigrokdecode are compiled as sources directly into the executable, so
# their symbols resolve from the executable's own objects (no lib dep needed).
# FFTW/glib also appear in pxview-core's PUBLIC ${PXVIEW_LINK_LIBS}, but those
# precede libpxview-data.a on the link line and cannot resolve its references
# (static-library link order is left-to-right), hence the PRIVATE re-assertion.
target_link_libraries(pxview-data PRIVATE
    ${FFTW_LIBRARIES}
    -lglib-2.0
)

# Expose PXView/ as include path so data .cpp files can resolve
# includes (pv/session/sigsession.h, pv/session/deviceagent.h, pv/base/log.h, pv/ui/langresource.h,
# pv/config/appconfig.h, pv/utility/array.h, pv/base/pxvdef.h, etc.).
target_include_directories(pxview-data PUBLIC
    ${CMAKE_SOURCE_DIR}/PXView
)
