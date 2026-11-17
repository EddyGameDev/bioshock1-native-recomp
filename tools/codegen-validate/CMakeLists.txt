cmake_minimum_required(VERSION 3.25)
project(bioshock1_codegen_validate LANGUAGES CXX)

# Validates that ReXGlue's generated PPC->C++ sources compile with the SDK
# headers, independently of the Windows/D3D12 host application. This is the
# first gate before porting the generated code to the Android CMake target.
#
# Configure with:
#   cmake -S tools/codegen-validate -B build/codegen-validate -G Ninja \
#     -DGEN_DIR=<...>/generated/default \
#     -DSDK_INCLUDE_DIR=<...>/rexglue-sdk-0.11.0/win-amd64/include

if(NOT DEFINED GEN_DIR OR NOT EXISTS "${GEN_DIR}")
  message(FATAL_ERROR "Set -DGEN_DIR to the generated/default directory")
endif()
if(NOT DEFINED SDK_INCLUDE_DIR OR NOT EXISTS "${SDK_INCLUDE_DIR}")
  message(FATAL_ERROR "Set -DSDK_INCLUDE_DIR to the ReXGlue SDK include directory")
endif()

file(TO_CMAKE_PATH "${GEN_DIR}" GEN_DIR)
file(TO_CMAKE_PATH "${SDK_INCLUDE_DIR}" SDK_INCLUDE_DIR)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB RECOMP_SOURCES "${GEN_DIR}/bioshock1_recomp.*.cpp")
if(NOT RECOMP_SOURCES)
  message(FATAL_ERROR "No generated sources found in ${GEN_DIR}")
endif()

add_library(generated_code OBJECT
  ${RECOMP_SOURCES}
  "${GEN_DIR}/bioshock1_init.cpp"
  "${GEN_DIR}/bioshock1_register.cpp"
)
target_include_directories(generated_code PRIVATE "${SDK_INCLUDE_DIR}" "${GEN_DIR}")
target_compile_definitions(generated_code PRIVATE REX_PHYSICAL_OFFSET_IN_CODE=0)
set_target_properties(generated_code PROPERTIES POSITION_INDEPENDENT_CODE ON)

message(STATUS "Validating ${CMAKE_CURRENT_LIST_DIR}: ${RECOMP_SOURCES} TUs")

# Phase 2 gate: link the generated code into a real arm64 shared object.
option(BS_BUILD_LINK_TEST "Build a SHARED lib from generated code + runtime stubs" OFF)
if(BS_BUILD_LINK_TEST)
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../native" NATIVE_DIR)
  add_library(bs1_link_test SHARED
    $<TARGET_OBJECTS:generated_code>
    "${NATIVE_DIR}/src/engine.cpp"
    "${NATIVE_DIR}/src/hle_kernel.cpp"
    "${NATIVE_DIR}/src/hle_imports.cpp"
    "${NATIVE_DIR}/src/gpu_layer.cpp"
    "${NATIVE_DIR}/src/xenos_pm4.cpp"
    "${NATIVE_DIR}/src/abi_helpers.cpp"
    "${NATIVE_DIR}/src/import_stubs.cpp"
    "${NATIVE_DIR}/src/runtime_symbols.cpp"
  )
  target_include_directories(bs1_link_test PRIVATE
    "${SDK_INCLUDE_DIR}" "${GEN_DIR}" "${NATIVE_DIR}/include"
  )
  target_compile_definitions(bs1_link_test PRIVATE
    REX_PHYSICAL_OFFSET_IN_CODE=0 BS_NEED_ATOMIC_REF_SHIM=1 BS_USE_REX_CONTEXT=1
  )
  target_compile_options(bs1_link_test PRIVATE
    "-include" "${NATIVE_DIR}/include/compat/atomic_ref.h"
  )
  target_link_libraries(bs1_link_test PRIVATE log)
  # Catch every unresolved symbol instead of allowing a half-linked library.
  target_link_options(bs1_link_test PRIVATE "LINKER:-z,defs")
endif()

# Host-side first-boot harness: links generated host code with our runtime.
option(BS_BUILD_BOOT_HARNESS "Build a host first-boot harness" OFF)
if(BS_BUILD_BOOT_HARNESS)
  file(TO_CMAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../native" NATIVE_DIR2)
  add_executable(bs1_boot
    "${CMAKE_CURRENT_LIST_DIR}/../boot-harness/main.cpp"
    $<TARGET_OBJECTS:generated_code>
    "${NATIVE_DIR2}/src/engine.cpp"
    "${NATIVE_DIR2}/src/hle_kernel.cpp"
    "${NATIVE_DIR2}/src/hle_imports.cpp"
    "${NATIVE_DIR2}/src/gpu_layer.cpp"
    "${NATIVE_DIR2}/src/xenos_pm4.cpp"
    "${NATIVE_DIR2}/src/abi_helpers.cpp"
    "${NATIVE_DIR2}/src/import_stubs.cpp"
    "${NATIVE_DIR2}/src/runtime_symbols.cpp"
  )
  target_include_directories(bs1_boot PRIVATE
    "${SDK_INCLUDE_DIR}" "${GEN_DIR}" "${NATIVE_DIR2}/include"
  )
  target_compile_definitions(bs1_boot PRIVATE REX_PHYSICAL_OFFSET_IN_CODE=0 BS_USE_REX_CONTEXT=1)
endif()
