cmake_minimum_required(VERSION 3.20)
project(asterra_weather LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED GODOT_CPP_DIR)
  message(FATAL_ERROR "Set GODOT_CPP_DIR to a Godot 4.7-compatible godot-cpp checkout")
endif()

add_subdirectory(${GODOT_CPP_DIR} godot-cpp)

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
