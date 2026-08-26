cmake_minimum_required(VERSION 3.20)
project(asterra_weather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

enable_testing()

# Phase-1 replacement-core tests deliberately have no Godot dependency. They
# validate spherical geometry/connectivity and conservation before the runtime
# backend is allowed to depend on the new core.
add_executable(asterra_weather_core_tests
  tests/test_cubed_sphere.cpp
  src/cubed_sphere_grid.cpp
)
target_include_directories(asterra_weather_core_tests PRIVATE src)
add_test(NAME cubed_sphere_geometry COMMAND asterra_weather_core_tests)

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
  src/cubed_sphere_grid.cpp
  # Temporary runtime implementation while the replacement dynamical core is
  # being built. This file will be deleted rather than maintained as a fallback
  # once the cubed-sphere dry core passes the mandatory gates.
  src/weather_native_oklahoma.cpp
)

target_include_directories(asterra_weather PRIVATE src)
target_link_libraries(asterra_weather PRIVATE godot-cpp)

# The parent atmosphere is intentionally CPU-heavy. Keep AVX2 inside each worker
# and distribute independent operations through OpenMP.
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
  target_link_libraries(asterra_weather PRIVATE OpenMP::OpenMP_CXX)
  target_compile_definitions(asterra_weather PRIVATE ASTERRA_WEATHER_OPENMP=1)
endif()

if(MSVC)
  target_compile_options(asterra_weather PRIVATE /O2 /arch:AVX2 /fp:fast)
  target_compile_options(asterra_weather_core_tests PRIVATE /O2)
else()
  target_compile_options(asterra_weather PRIVATE -O3 -mavx2 -mfma -ffast-math)
  target_compile_options(asterra_weather_core_tests PRIVATE -O3)
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
