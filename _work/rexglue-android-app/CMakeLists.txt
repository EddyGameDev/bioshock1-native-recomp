cmake_minimum_required(VERSION 3.25)
project(bioshock1_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK source tree (patched for Android) provides rex::app, rex::kernel, etc.
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)

# Reuse the already-staged generated sources (152 TUs) from the host build.
set(GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-bioshock1/generated/default")
file(GLOB GENERATED_SOURCES
    "${GEN_DIR}/bioshock1_recomp.*.cpp"
    "${GEN_DIR}/bioshock1_init.cpp"
    "${GEN_DIR}/bioshock1_register.cpp")

add_library(bioshock1-android SHARED
    src/main.cpp
    ${GENERATED_SOURCES}
)
target_include_directories(bioshock1-android PRIVATE
    "${GEN_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-bioshock1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../rexglue-bioshock1/src"
    "${REXSDK_DIR}/src"
)
target_compile_definitions(bioshock1-android PRIVATE
    REX_PHYSICAL_OFFSET_IN_CODE=0
    BS_USE_REX_CONTEXT=1
)
# NDK r30 (Clang 21) libc++ has native std::atomic_ref and jthread, so no shim.
target_include_directories(bioshock1-android PRIVATE "${REXSDK_DIR}/thirdparty/sdl3/include")
target_link_libraries(bioshock1-android PRIVATE rex::app log android dl SDL3::SDL3-shared)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../renderer/CMakeLists.txt")
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../renderer" custom-renderer)
  if (TARGET rexgpu-xenos-custom)
    add_dependencies(bioshock1-android rexgpu-xenos-custom)
  endif ()
endif ()
