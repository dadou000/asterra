cmake_minimum_required(VERSION 3.20)
project(asterra_weather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
  src/weather_native.cpp
)

target_include_directories(asterra_weather PRIVATE src)
target_link_libraries(asterra_weather PRIVATE godot-cpp)

if(MSVC)
  target_compile_options(asterra_weather PRIVATE /O2 /arch:AVX2 /fp:fast)
else()
  target_compile_options(asterra_weather PRIVATE -O3 -mavx2 -mfma -ffast-math)
endif()

set_target_properties(asterra_weather PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../../bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../../bin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../../bin"
  OUTPUT_NAME "asterra_weather"
)
