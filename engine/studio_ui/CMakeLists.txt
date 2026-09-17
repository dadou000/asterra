add_library(OrbitStudioUi STATIC
    src/ProjectAuthoringUi.cpp
)

add_library(Orbit::StudioUi ALIAS OrbitStudioUi)

target_include_directories(OrbitStudioUi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitStudioUi
    PUBLIC
        Orbit::EditorUi
        Orbit::StudioSession
)

target_compile_features(OrbitStudioUi PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitStudioUi)
