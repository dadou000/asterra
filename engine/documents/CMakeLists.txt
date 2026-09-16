add_library(OrbitDocuments STATIC
    src/ProjectDocument.cpp
    src/ProjectManifest.cpp
    src/WorldDatabase.cpp
)

add_library(Orbit::Documents ALIAS OrbitDocuments)

target_include_directories(OrbitDocuments
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitDocuments
    PUBLIC
        Orbit::Core
    PRIVATE
        SQLiteCpp
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitDocuments PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitDocuments)
