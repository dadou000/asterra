cmake_minimum_required(VERSION 3.20)
project(asterra_weather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

enable_testing()

# Replacement dynamical-core foundation. This library deliberately has no Godot
# dependency so geometry, transport and staggered dynamics can be validated
# deterministically before the runtime backend is allowed to depend on them.
add_library(asterra_weather_core STATIC
  src/cubed_sphere_grid.cpp
  src/conservative_transport.cpp
  src/shallow_water_cgrid.cpp
  src/geodesic_voronoi_grid.cpp
  src/geodesic_voronoi_mpas.cpp
  src/voronoi_shallow_water.cpp
)
target_include_directories(asterra_weather_core PUBLIC src)

# Legacy cubed-sphere foundation tests remain temporarily as useful regression
# references for conservative transport. The nonlinear cubed-sphere dynamical
# path has been rejected and is no longer a release gate.
add_executable(asterra_weather_geometry_tests
  tests/test_cubed_sphere.cpp
)
target_link_libraries(asterra_weather_geometry_tests PRIVATE asterra_weather_core)
add_test(NAME cubed_sphere_geometry COMMAND asterra_weather_geometry_tests)

add_executable(asterra_weather_transport_tests
  tests/test_conservative_transport.cpp
)
target_link_libraries(asterra_weather_transport_tests PRIVATE asterra_weather_core)
add_test(NAME conservative_transport COMMAND asterra_weather_transport_tests)

add_executable(asterra_weather_shallow_water_tests
  tests/test_shallow_water_cgrid.cpp
)
target_link_libraries(asterra_weather_shallow_water_tests PRIVATE asterra_weather_core)
add_test(NAME shallow_water_cgrid COMMAND asterra_weather_shallow_water_tests)

add_executable(asterra_weather_geodesic_tests
  tests/test_geodesic_voronoi.cpp
)
target_link_libraries(asterra_weather_geodesic_tests PRIVATE asterra_weather_core)
add_test(NAME geodesic_voronoi_grid COMMAND asterra_weather_geodesic_tests)

add_executable(asterra_weather_voronoi_shallow_water_tests
  tests/test_voronoi_shallow_water.cpp
)
target_link_libraries(asterra_weather_voronoi_shallow_water_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_shallow_water COMMAND asterra_weather_voronoi_shallow_water_tests)

add_custom_target(asterra_weather_core_tests
  DEPENDS
    asterra_weather_geometry_tests
    asterra_weather_transport_tests
    asterra_weather_shallow_water_tests
    asterra_weather_geodesic_tests
    asterra_weather_voronoi_shallow_water_tests
)

if(NOT GODOTCPP_API_VERSION)
  set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version for godot-cpp" FORCE)
endif()
if(NOT GODOTCPP_TARGET)
  set(GODOTCPP_TARGET "template_release" CACHE STRING "godot-cpp target kind")
endif()

if(DEFINED GODOT_CPP_DIR)
  add_subdirectory(${GODOT_CPP_DIR} godot-cpp)
else()
  include(FetchContent)
  FetchContent_Declare(
    godot-cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG master
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(godot-cpp)
endif()

add_library(asterra_weather SHARED
  src/register_types.cpp
  src/weather_wind_sampler.cpp
  # Temporary runtime implementation until the Voronoi dry core passes its
  # mandatory gates. It will be deleted, not maintained as a fallback.
  src/weather_native_oklahoma.cpp
)

target_include_directories(asterra_weather PRIVATE src)
target_link_libraries(asterra_weather PRIVATE godot-cpp asterra_weather_core)

find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  target_link_libraries(asterra_weather PRIVATE OpenMP::OpenMP_CXX)
  target_compile_definitions(asterra_weather PRIVATE ASTERRA_WEATHER_OPENMP=1)
endif()

if(MSVC)
  target_compile_options(asterra_weather PRIVATE /O2 /arch:AVX2 /fp:fast)
  target_compile_options(asterra_weather_core PRIVATE /O2)
  target_compile_options(asterra_weather_geometry_tests PRIVATE /O2)
  target_compile_options(asterra_weather_transport_tests PRIVATE /O2)
  target_compile_options(asterra_weather_shallow_water_tests PRIVATE /O2)
  target_compile_options(asterra_weather_geodesic_tests PRIVATE /O2)
  target_compile_options(asterra_weather_voronoi_shallow_water_tests PRIVATE /O2)
else()
  target_compile_options(asterra_weather PRIVATE -O3 -mavx2 -mfma -ffast-math)
  target_compile_options(asterra_weather_core PRIVATE -O3)
  target_compile_options(asterra_weather_geometry_tests PRIVATE -O3)
  target_compile_options(asterra_weather_transport_tests PRIVATE -O3)
  target_compile_options(asterra_weather_shallow_water_tests PRIVATE -O3)
  target_compile_options(asterra_weather_geodesic_tests PRIVATE -O3)
  target_compile_options(asterra_weather_voronoi_shallow_water_tests PRIVATE -O3)
endif()

get_filename_component(ASTERRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ASTERRA_BIN_DIR "${ASTERRA_ROOT}/bin" CACHE PATH
  "Directory for the Asterra weather runtime artifacts")

set_target_properties(asterra_weather PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${ASTERRA_BIN_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${ASTERRA_BIN_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${ASTERRA_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${ASTERRA_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${ASTERRA_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ASTERRA_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${ASTERRA_BIN_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_DEBUG "${ASTERRA_BIN_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${ASTERRA_BIN_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ASTERRA_BIN_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL "${ASTERRA_BIN_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${ASTERRA_BIN_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${ASTERRA_BIN_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${ASTERRA_BIN_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${ASTERRA_BIN_DIR}"
  OUTPUT_NAME "asterra_weather"
)

add_custom_command(TARGET asterra_weather POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ASTERRA_BIN_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/asterra_weather.gdextension.in"
    "${ASTERRA_BIN_DIR}/asterra_weather.gdextension"
  COMMENT "Publishing Asterra AVX2 weather GDExtension manifest"
)
