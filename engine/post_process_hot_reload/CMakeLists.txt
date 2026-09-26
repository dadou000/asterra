add_library(OrbitEyeAdaptationHotReload MODULE
    src/EyeAdaptationHotReloadModule.cpp
    ../post_process/src/HumanEyeAdaptation.cpp
)

target_include_directories(OrbitEyeAdaptationHotReload
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../post_process/include
)

target_link_libraries(OrbitEyeAdaptationHotReload
    PRIVATE
        Orbit::Core
        Orbit::HotReloadApi
)

target_compile_features(OrbitEyeAdaptationHotReload PRIVATE cxx_std_23)
orbit_enable_warnings(OrbitEyeAdaptationHotReload)

set_target_properties(OrbitEyeAdaptationHotReload PROPERTIES
    OUTPUT_NAME "OrbitEyeAdaptationHotReload"
)
