# The ten WMO cloud genera as one table (heights, thickness, extinction,
# phase). Pure C++ with no dependencies: the Weather Lab layer generator, the
# volume renderer and the planet cloud shader all read it.
add_library(OrbitCloudGenera STATIC
    src/CloudGenera.cpp
)

add_library(Orbit::CloudGenera ALIAS OrbitCloudGenera)

target_include_directories(OrbitCloudGenera
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(OrbitCloudGenera PUBLIC cxx_std_23)
orbit_enable_warnings(OrbitCloudGenera)

if(BUILD_TESTING)
    add_executable(OrbitCloudGeneraTests tests/CloudGeneraTests.cpp)
    target_link_libraries(OrbitCloudGeneraTests PRIVATE Orbit::CloudGenera)
    target_compile_features(OrbitCloudGeneraTests PRIVATE cxx_std_23)
    orbit_enable_warnings(OrbitCloudGeneraTests)

    add_test(NAME Orbit.CloudGenera COMMAND OrbitCloudGeneraTests)
endif()
