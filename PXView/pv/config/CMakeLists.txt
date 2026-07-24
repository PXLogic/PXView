# pxview-config: app config + shortcut definitions
add_library(pxview-config STATIC
    appconfig.cpp
    shortcutdefs.cpp
)

target_link_libraries(pxview-config PUBLIC
    Qt6::Core
    nlohmann_json::nlohmann_json
)

target_include_directories(pxview-config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
