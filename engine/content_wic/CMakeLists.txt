add_library(OrbitContentWic STATIC
    src/WicTextureImporter.cpp
)

add_library(Orbit::ContentWic ALIAS OrbitContentWic)

target_include_directories(OrbitContentWic
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitContentWic
    PUBLIC
        Orbit::Content
    PRIVATE
        ole32
        windowscodecs
)

target_compile_features(OrbitContentWic PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitContentWic)
