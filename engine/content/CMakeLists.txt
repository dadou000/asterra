add_library(OrbitContent STATIC
    src/ContentService.cpp
    src/ContentHash.cpp
    src/DerivedDataCache.cpp
    src/AssetPipeline.cpp
    src/ThumbnailService.cpp
    src/PbrSetImport.cpp
    src/MaterialInstance.cpp
)

add_library(Orbit::Content ALIAS OrbitContent)

target_include_directories(OrbitContent
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(OrbitContent
    PUBLIC
        Orbit::Core
    PRIVATE
        tomlplusplus::tomlplusplus
)

target_compile_features(OrbitContent PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitContent)

if(BUILD_TESTING)
    add_executable(OrbitContentTests tests/ContentServiceTests.cpp)
    target_link_libraries(OrbitContentTests PRIVATE Orbit::Content)
    target_compile_features(OrbitContentTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitContentTests)
    add_test(NAME Orbit.Content COMMAND OrbitContentTests)

    add_executable(OrbitAssetPipelineTests tests/AssetPipelineTests.cpp)
    target_link_libraries(OrbitAssetPipelineTests PRIVATE Orbit::Content)
    target_compile_features(OrbitAssetPipelineTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitAssetPipelineTests)
    add_test(NAME Orbit.AssetPipeline COMMAND OrbitAssetPipelineTests)
endif()
