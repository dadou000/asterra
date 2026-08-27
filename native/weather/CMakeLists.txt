cmake_minimum_required(VERSION 3.20)
project(asterra_weather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_testing()

add_library(asterra_weather_core STATIC
  src/cubed_sphere_grid.cpp
  src/conservative_transport.cpp
  src/shallow_water_cgrid.cpp
  src/geodesic_voronoi_grid.cpp
  src/geodesic_voronoi_mpas.cpp
  src/spherical_latlon_sampler.cpp
  src/voronoi_shallow_water.cpp
  src/voronoi_dry_hydrostatic.cpp
  src/voronoi_dry_transport.cpp
  src/voronoi_dry_dynamics.cpp
  src/voronoi_dry_vertical_transport.cpp
  src/voronoi_dry_core.cpp
  src/voronoi_moist_thermodynamics.cpp
  src/voronoi_moist_diagnostics.cpp
  src/voronoi_surface_exchange.cpp
)
target_include_directories(asterra_weather_core PUBLIC src)

add_executable(asterra_weather_geometry_tests tests/test_cubed_sphere.cpp)
target_link_libraries(asterra_weather_geometry_tests PRIVATE asterra_weather_core)
add_test(NAME cubed_sphere_geometry COMMAND asterra_weather_geometry_tests)

add_executable(asterra_weather_transport_tests tests/test_conservative_transport.cpp)
target_link_libraries(asterra_weather_transport_tests PRIVATE asterra_weather_core)
add_test(NAME conservative_transport COMMAND asterra_weather_transport_tests)

add_executable(asterra_weather_shallow_water_tests tests/test_shallow_water_cgrid.cpp)
target_link_libraries(asterra_weather_shallow_water_tests PRIVATE asterra_weather_core)
add_test(NAME shallow_water_cgrid COMMAND asterra_weather_shallow_water_tests)

add_executable(asterra_weather_geodesic_tests tests/test_geodesic_voronoi.cpp)
target_link_libraries(asterra_weather_geodesic_tests PRIVATE asterra_weather_core)
add_test(NAME geodesic_voronoi_grid COMMAND asterra_weather_geodesic_tests)

add_executable(asterra_weather_trisk_weight_tests tests/test_trisk_weights.cpp)
target_link_libraries(asterra_weather_trisk_weight_tests PRIVATE asterra_weather_core)
add_test(NAME trisk_weights COMMAND asterra_weather_trisk_weight_tests)

add_executable(asterra_weather_latlon_sampler_tests tests/test_spherical_latlon_sampler.cpp)
target_link_libraries(asterra_weather_latlon_sampler_tests PRIVATE asterra_weather_core)
add_test(NAME spherical_latlon_sampler COMMAND asterra_weather_latlon_sampler_tests)

add_executable(asterra_weather_voronoi_shallow_water_tests tests/test_voronoi_shallow_water.cpp)
target_link_libraries(asterra_weather_voronoi_shallow_water_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_shallow_water COMMAND asterra_weather_voronoi_shallow_water_tests)

add_executable(asterra_weather_rossby_haurwitz_tests tests/test_rossby_haurwitz_phase.cpp)
target_link_libraries(asterra_weather_rossby_haurwitz_tests PRIVATE asterra_weather_core)
add_test(NAME rossby_haurwitz COMMAND asterra_weather_rossby_haurwitz_tests)

add_executable(asterra_weather_hydrostatic_tests tests/test_voronoi_dry_hydrostatic.cpp)
target_link_libraries(asterra_weather_hydrostatic_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_hydrostatic COMMAND asterra_weather_hydrostatic_tests)

add_executable(asterra_weather_dry_transport_tests tests/test_voronoi_dry_transport.cpp)
target_link_libraries(asterra_weather_dry_transport_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_transport COMMAND asterra_weather_dry_transport_tests)

add_executable(asterra_weather_dry_dynamics_tests tests/test_voronoi_dry_dynamics.cpp)
target_link_libraries(asterra_weather_dry_dynamics_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_dynamics COMMAND asterra_weather_dry_dynamics_tests)

add_executable(asterra_weather_dry_vertical_tests tests/test_voronoi_dry_vertical_transport.cpp)
target_link_libraries(asterra_weather_dry_vertical_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_vertical_transport COMMAND asterra_weather_dry_vertical_tests)

add_executable(asterra_weather_dry_remap_tests tests/test_voronoi_dry_coordinate_remap.cpp)
target_link_libraries(asterra_weather_dry_remap_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_coordinate_remap COMMAND asterra_weather_dry_remap_tests)

add_executable(asterra_weather_dry_core_tests tests/test_voronoi_dry_core.cpp)
target_link_libraries(asterra_weather_dry_core_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_core COMMAND asterra_weather_dry_core_tests)

add_executable(asterra_weather_dry_budget_tests tests/test_voronoi_dry_budgets.cpp)
target_link_libraries(asterra_weather_dry_budget_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_budgets COMMAND asterra_weather_dry_budget_tests)

add_executable(asterra_weather_dry_orography_tests tests/test_voronoi_dry_orography.cpp)
target_link_libraries(asterra_weather_dry_orography_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_dry_orography COMMAND asterra_weather_dry_orography_tests)

add_executable(asterra_weather_passive_tracer_tests tests/test_voronoi_passive_tracers.cpp)
target_link_libraries(asterra_weather_passive_tracer_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_passive_tracers COMMAND asterra_weather_passive_tracer_tests)

add_executable(asterra_weather_moist_thermodynamics_tests tests/test_voronoi_moist_thermodynamics.cpp)
target_link_libraries(asterra_weather_moist_thermodynamics_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_moist_thermodynamics COMMAND asterra_weather_moist_thermodynamics_tests)

add_executable(asterra_weather_moist_cycle_tests tests/test_voronoi_moist_cycle.cpp)
target_link_libraries(asterra_weather_moist_cycle_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_moist_cycle COMMAND asterra_weather_moist_cycle_tests)

add_executable(asterra_weather_moist_density_tests tests/test_voronoi_moist_density.cpp)
target_link_libraries(asterra_weather_moist_density_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_moist_density COMMAND asterra_weather_moist_density_tests)

add_executable(asterra_weather_surface_exchange_tests tests/test_voronoi_surface_exchange.cpp)
target_link_libraries(asterra_weather_surface_exchange_tests PRIVATE asterra_weather_core)
add_test(NAME voronoi_surface_exchange COMMAND asterra_weather_surface_exchange_tests)

add_custom_target(asterra_weather_core_tests
  DEPENDS
    asterra_weather_geometry_tests
    asterra_weather_transport_tests
    asterra_weather_shallow_water_tests
    asterra_weather_geodesic_tests
    asterra_weather_trisk_weight_tests
    asterra_weather_latlon_sampler_tests
    asterra_weather_voronoi_shallow_water_tests
    asterra_weather_rossby_haurwitz_tests
    asterra_weather_hydrostatic_tests
    asterra_weather_dry_transport_tests
    asterra_weather_dry_dynamics_tests
    asterra_weather_dry_vertical_tests
    asterra_weather_dry_remap_tests
    asterra_weather_dry_core_tests
    asterra_weather_dry_budget_tests
    asterra_weather_dry_orography_tests
    asterra_weather_passive_tracer_tests
    asterra_weather_moist_thermodynamics_tests
    asterra_weather_moist_cycle_tests
    asterra_weather_moist_density_tests
    asterra_weather_surface_exchange_tests
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
  src/weather_core_native.cpp
  src/weather_dry_core_native.cpp
  src/weather_wind_sampler.cpp
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
  target_compile_options(asterra_weather_trisk_weight_tests PRIVATE /O2)
  target_compile_options(asterra_weather_latlon_sampler_tests PRIVATE /O2)
  target_compile_options(asterra_weather_voronoi_shallow_water_tests PRIVATE /O2)
  target_compile_options(asterra_weather_rossby_haurwitz_tests PRIVATE /O2)
  target_compile_options(asterra_weather_hydrostatic_tests PRIVATE /O2)
  target_compile_options(asterra_weather_dry_transport_tests PRIVATE /O2)
  target_compile_options(asterra_weather_dry_dynamics_tests PRIVATE /O2)
  target_compile_options(asterra_weather_dry_vertical_tests PRIVATE /O2)
  target_compile_options(asterra_weather_dry_remap_tests PRIVATE /O2)
  target_compile_options(asterra_weather_dry_core_tests PRIVATE /O2)
  target_compile_options(asterra_weather_dry_budget_tests PRIVATE /O2)
  target_compile_options(asterra_weather_dry_orography_tests PRIVATE /O2)
  target_compile_options(asterra_weather_passive_tracer_tests PRIVATE /O2)
  target_compile_options(asterra_weather_moist_thermodynamics_tests PRIVATE /O2)
  target_compile_options(asterra_weather_moist_cycle_tests PRIVATE /O2)
  target_compile_options(asterra_weather_moist_density_tests PRIVATE /O2)
  target_compile_options(asterra_weather_surface_exchange_tests PRIVATE /O2)
else()
  target_compile_options(asterra_weather PRIVATE -O3 -mavx2 -mfma -ffast-math)
  target_compile_options(asterra_weather_core PRIVATE -O3)
  target_compile_options(asterra_weather_geometry_tests PRIVATE -O3)
  target_compile_options(asterra_weather_transport_tests PRIVATE -O3)
  target_compile_options(asterra_weather_shallow_water_tests PRIVATE -O3)
  target_compile_options(asterra_weather_geodesic_tests PRIVATE -O3)
  target_compile_options(asterra_weather_trisk_weight_tests PRIVATE -O3)
  target_compile_options(asterra_weather_latlon_sampler_tests PRIVATE -O3)
  target_compile_options(asterra_weather_voronoi_shallow_water_tests PRIVATE -O3)
  target_compile_options(asterra_weather_rossby_haurwitz_tests PRIVATE -O3)
  target_compile_options(asterra_weather_hydrostatic_tests PRIVATE -O3)
  target_compile_options(asterra_weather_dry_transport_tests PRIVATE -O3)
  target_compile_options(asterra_weather_dry_dynamics_tests PRIVATE -O3)
  target_compile_options(asterra_weather_dry_vertical_tests PRIVATE -O3)
  target_compile_options(asterra_weather_dry_remap_tests PRIVATE -O3)
  target_compile_options(asterra_weather_dry_core_tests PRIVATE -O3)
  target_compile_options(asterra_weather_dry_budget_tests PRIVATE -O3)
  target_compile_options(asterra_weather_dry_orography_tests PRIVATE -O3)
  target_compile_options(asterra_weather_passive_tracer_tests PRIVATE -O3)
  target_compile_options(asterra_weather_moist_thermodynamics_tests PRIVATE -O3)
  target_compile_options(asterra_weather_moist_cycle_tests PRIVATE -O3)
  target_compile_options(asterra_weather_moist_density_tests PRIVATE -O3)
  target_compile_options(asterra_weather_surface_exchange_tests PRIVATE -O3)
endif()

get_filename_component(ASTERRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ASTERRA_BIN_DIR "${ASTERRA_ROOT}/bin" CACHE PATH "Directory for the Asterra weather runtime artifacts")

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
