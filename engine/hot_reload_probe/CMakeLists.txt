add_library(OrbitHotReloadProbe SHARED
    src/HotReloadProbe.cpp
)

target_link_libraries(OrbitHotReloadProbe
    PRIVATE
        Orbit::HotReloadApi
)

target_compile_features(OrbitHotReloadProbe PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitHotReloadProbe)

set_target_properties(OrbitHotReloadProbe PROPERTIES
    OUTPUT_NAME "OrbitHotReloadProbe"
)
