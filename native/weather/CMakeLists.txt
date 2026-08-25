cmake_minimum_required(VERSION 3.20)
project(asterra_weather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
  # This translation unit includes the proven AVX2 core and wraps only the
  # capped-convection/cloud-presentation entry points. Do not compile
  # weather_native.cpp separately or its symbols would be defined twice.
  src/weather_native_oklahoma.cpp
)

target_include_directories(asterra_weather PRIVATE src)
target_link_libraries(asterra_weather PRIVATE godot-cpp)

if(MSVC)
  target_compile_options(asterra_weather PRIVATE /O2 /arch:AVX2 /fp:fast)
else()
  target_compile_options(asterra_weather PRIVATE -O3 -mavx2 -mfma -ffast-math)
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