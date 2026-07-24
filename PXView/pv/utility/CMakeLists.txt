# pxview-utility: leaf utilities (encoding, path, array)
add_library(pxview-utility STATIC
    encoding.cpp
    path.cpp
    array.cpp
)

target_link_libraries(pxview-utility PUBLIC Qt6::Core)

target_include_directories(pxview-utility PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
