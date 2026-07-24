# pxview-interface: callback + event interfaces (headers only)
# Pure INTERFACE library — no sources, no dependencies.
add_library(pxview-interface INTERFACE)

target_include_directories(pxview-interface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
