# pxview-interface: callback + event interfaces (headers only)
# Pure INTERFACE library — no sources.
add_library(pxview-interface INTERFACE)

target_include_directories(pxview-interface INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# event_notification.h / events_json.h include <nlohmann/json.hpp>, so every
# consumer of these headers needs the nlohmann include path. On Linux/macOS the
# system package installs into a default include dir (so it happens to work
# without this), but on MSYS2/Windows deps.cmake falls back to FetchContent and
# the headers are ONLY reachable through this imported target.
target_link_libraries(pxview-interface INTERFACE
    nlohmann_json::nlohmann_json
)
