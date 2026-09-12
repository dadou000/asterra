add_library(OrbitDevServer STATIC
    src/DevServer.cpp
)

add_library(Orbit::DevServer ALIAS OrbitDevServer)

target_include_directories(OrbitDevServer
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitDevServer
    PUBLIC
        Orbit::Core
    PRIVATE
        ws2_32
)

target_compile_features(OrbitDevServer PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitDevServer)
