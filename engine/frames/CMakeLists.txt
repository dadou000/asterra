add_library(OrbitFrames STATIC
    src/FrameGraph.cpp
)

add_library(Orbit::Frames ALIAS OrbitFrames)

target_include_directories(OrbitFrames
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitFrames
    PUBLIC
        Orbit::Core
        Orbit::Math
        Orbit::Time
)

target_compile_features(OrbitFrames PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitFrames)
