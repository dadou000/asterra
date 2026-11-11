add_library(OrbitMeshImport STATIC
    src/GltfImporter.cpp
    src/PrimitiveMesh.cpp
)

add_library(Orbit::MeshImport ALIAS OrbitMeshImport)

target_include_directories(OrbitMeshImport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitMeshImport
    PUBLIC
        Orbit::Core
    PRIVATE
        nlohmann_json::nlohmann_json
)

target_compile_features(OrbitMeshImport PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitMeshImport)

if(BUILD_TESTING)
    add_executable(OrbitMeshImportTests
        tests/GltfImporterTests.cpp
    )

    target_link_libraries(OrbitMeshImportTests
        PRIVATE
            Orbit::MeshImport
    )

    target_compile_features(OrbitMeshImportTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitMeshImportTests)

    add_test(
        NAME Orbit.MeshImport
        COMMAND OrbitMeshImportTests
    )

    add_executable(OrbitPrimitiveMeshTests
        tests/PrimitiveMeshTests.cpp
    )

    target_link_libraries(OrbitPrimitiveMeshTests
        PRIVATE
            Orbit::MeshImport
    )

    target_compile_features(OrbitPrimitiveMeshTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitPrimitiveMeshTests)

    add_test(
        NAME Orbit.PrimitiveMesh
        COMMAND OrbitPrimitiveMeshTests
    )
endif()
