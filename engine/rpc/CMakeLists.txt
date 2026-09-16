add_library(OrbitRpc STATIC
    src/JsonRpc.cpp
)

add_library(Orbit::Rpc ALIAS OrbitRpc)

target_include_directories(OrbitRpc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitRpc
    PUBLIC
        Orbit::Core
    PRIVATE
        nlohmann_json::nlohmann_json
)

target_compile_features(OrbitRpc PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitRpc)

if(BUILD_TESTING)
    add_executable(OrbitRpcTests
        tests/JsonRpcTests.cpp
    )

    target_link_libraries(OrbitRpcTests
        PRIVATE
            Orbit::Rpc
    )

    target_compile_features(OrbitRpcTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitRpcTests)

    add_test(
        NAME Orbit.Rpc
        COMMAND OrbitRpcTests
    )
endif()
