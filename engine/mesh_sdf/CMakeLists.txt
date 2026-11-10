add_library(OrbitMeshSdf STATIC
    src/MeshSdf.cpp
)

add_library(Orbit::MeshSdf ALIAS OrbitMeshSdf)

target_include_directories(OrbitMeshSdf
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitMeshSdf
    PUBLIC
        Orbit::Core
        Orbit::MeshImport
)

target_compile_features(OrbitMeshSdf PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitMeshSdf)

if(BUILD_TESTING)
    add_executable(OrbitMeshSdfTests
        tests/MeshSdfTests.cpp
    )

    target_link_libraries(OrbitMeshSdfTests
        PRIVATE
            Orbit::MeshSdf
    )

    target_compile_features(OrbitMeshSdfTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitMeshSdfTests)

    add_test(
        NAME Orbit.MeshSdf
        COMMAND OrbitMeshSdfTests
    )
endif()
