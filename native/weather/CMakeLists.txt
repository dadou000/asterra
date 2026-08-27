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
)
target_include_directories(asterra_weather_core PUBLIC src)

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

add_executable(asterra_weather_standard_tests
  tests/test_shallow_water_standard.cpp
)
target_link_libraries(asterra_weather_standard_tests PRIVATE asterra_weather_core)
add_test(NAME shallow_water_standard COMMAND asterra_weather_standard_tests)

# CI and the local build script can keep building one gate target. Any new core
# validation executable must be added as a dependency here before the runtime DLL.
add_custom_target(asterra_weather_core_tests
  DEPENDS
    asterra_weather_geometry_tests
    asterra_weather_transport_tests
    asterra_weather_shallow_water_tests
    asterra_weather_standard_tests
)

# godot-cpp master no longer guesses an engine API for extension projects.
# Asterra targets Godot 4.7.1, whose stable GDExtension ABI is the 4.7 API.
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
  # Temporary runtime implementation while the replacement dynamical core is
  # being built. This file will be deleted rather than maintained as a fallback
  # once the cubed-sphere dry core passes the mandatory gates.
  src/weather_native_oklahoma.cpp
)

target_include_directories(asterra_weather PRIVATE src)
target_link_libraries(asterra_weather PRIVATE godot-cpp asterra_weather_core)

# The parent atmosphere is intentionally CPU-heavy. Keep AVX2 inside each worker
# and distribute independent operations through OpenMP.
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
  target_compile_options(asterra_weather_standard_tests PRIVATE /O2)
else()
  target_compile_options(asterra_weather PRIVATE -O3 -mavx2 -mfma -ffast-math)
  target_compile_options(asterra_weather_core PRIVATE -O3)
  target_compile_options(asterra_weather_geometry_tests PRIVATE -O3)
  target_compile_options(asterra_weather_transport_tests PRIVATE -O3)
  target_compile_options(asterra_weather_shallow_water_tests PRIVATE -O3)
  target_compile_options(asterra_weather_standard_tests PRIVATE -O3)
endif()

# Keep every generator/configuration in the exact location referenced by the
# .gdextension manifest. Visual Studio otherwise appends Release/ or Debug/.
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

# Do not publish a live .gdextension until the DLL/SO exists. This prevents an
# unbuilt checkout from producing GDExtension load errors during project startup.
add_custom_command(TARGET asterra_weather POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${ASTERRA_BIN_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/asterra_weather.gdextension.in"
    "${ASTERRA_BIN_DIR}/asterra_weather.gdextension"
  COMMENT "Publishing Asterra AVX2 weather GDExtension manifest"
)
