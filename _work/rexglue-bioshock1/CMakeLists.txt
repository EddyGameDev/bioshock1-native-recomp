# bioshock1 - ReXGlue Recompiled Project
#
# The SDK package provides rexglue_project and the rexglue_setup_* functions.

cmake_minimum_required(VERSION 3.25)
project(bioshock1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (REXSDK_DIR)
  add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
else ()
  find_package(rexglue 0.11.0 REQUIRED CONFIG)
endif ()
rexglue_project(bioshock1 OUTPUT_DIR generated/default)

# Host sources. src/main.cpp stays on the executable.
set(BIOSHOCK1_HOST_SOURCES src/bioshock1_hooks.cpp)

# OBJECT and never STATIC, an archive drops the REX_EXPORT symbols.
add_library(bioshock1_common OBJECT ${BIOSHOCK1_HOST_SOURCES})
rexglue_setup_host_library(bioshock1_common)

if (WIN32)
  add_executable(bioshock1 WIN32 src/main.cpp)
  if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../renderer" custom-renderer)
  endif ()
else ()
  add_executable(bioshock1 src/main.cpp)
endif ()

rexglue_setup_target(bioshock1)
target_link_libraries(bioshock1 PRIVATE bioshock1_common)
if (WIN32 AND TARGET rexgpu-xenos-custom)
  add_dependencies(bioshock1 rexgpu-xenos-custom)
endif ()
