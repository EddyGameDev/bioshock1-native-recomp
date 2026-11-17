cmake_minimum_required(VERSION 3.22)
project(bioshock1_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(ENGINE_SOURCES
    src/engine.cpp
    src/hle_kernel.cpp
    src/hle_imports.cpp
    src/gpu_layer.cpp
    src/xenos_pm4.cpp
    src/abi_helpers.cpp
    src/import_stubs.cpp
)

if(ANDROID)
    # Built by Gradle/CMake through the NDK toolchain. BS_GLES3 enables the
    # real GLES3 renderer; the generated PPC code from ReXGlue/XenonRecomp is
    # added to ENGINE_SOURCES by the codegen step (see docs/03-BUILDING.md).
    add_library(bioshock1-native SHARED
        ${ENGINE_SOURCES}
        src/jni_bridge.cpp
    )
    target_compile_definitions(bioshock1-native PRIVATE BS_GLES3)
    target_include_directories(bioshock1-native PRIVATE include)
    target_link_libraries(bioshock1-native
        PRIVATE
        GLESv3
        EGL
        android
        log
        z
    )

    # Opt-in integration of the ReXGlue-generated PPC code (308 files, 164 MB).
    # OFF by default until the ReXGlue runtime dispatcher + kernel HLE are
    # ported to Android: the generated code compiles for arm64 with NDK r28c,
    # but still needs rex::kernel/FunctionDispatcher symbols to link.
    option(BS_USE_GENERATED_CODE "Compile staged ReXGlue generated code" OFF)
    if(BS_USE_GENERATED_CODE)
        if(NOT DEFINED REXGLUE_INCLUDE_DIR OR NOT EXISTS "${REXGLUE_INCLUDE_DIR}")
            message(FATAL_ERROR "Set -DREXGLUE_INCLUDE_DIR=<rexglue-sdk>/win-amd64/include")
        endif()
        file(TO_CMAKE_PATH "${REXGLUE_INCLUDE_DIR}" REXGLUE_INCLUDE_DIR)
        file(GLOB GENERATED_SOURCES
            "${PROJECT_SOURCE_DIR}/../recomp/generated/bioshock1_recomp.*.cpp"
            "${PROJECT_SOURCE_DIR}/../recomp/generated/bioshock1_init.cpp"
            "${PROJECT_SOURCE_DIR}/../recomp/generated/bioshock1_register.cpp"
        )
        if(NOT GENERATED_SOURCES)
            message(FATAL_ERROR "No staged generated sources; run tools/stage_generated.py")
        endif()
        target_sources(bioshock1-native PRIVATE
            ${GENERATED_SOURCES}
            ${PROJECT_SOURCE_DIR}/src/runtime_symbols.cpp
        )
        target_include_directories(bioshock1-native PRIVATE
            "${REXGLUE_INCLUDE_DIR}"
            "${PROJECT_SOURCE_DIR}/../recomp/generated"
        )
        target_compile_definitions(bioshock1-native PRIVATE
            REX_PHYSICAL_OFFSET_IN_CODE=0
            BS_NEED_ATOMIC_REF_SHIM=1
            BS_USE_REX_CONTEXT=1
        )
        # Android libc++ (even NDK r28c) lacks std::atomic_ref and public
        # jthread/stop_token; force-include the minimal compatibility shim.
        target_compile_options(bioshock1-native PRIVATE
            "-include" "${PROJECT_SOURCE_DIR}/include/compat/atomic_ref.h"
        )
        message(STATUS "Including ${GENERATED_SOURCES} generated source files")
    endif()
else()
    # Host-side smoke target: verifies the engine/HLE skeleton compiles even
    # without an Android SDK. The GPU layer falls back to a no-op counter.
    add_executable(bioshock1-selftest ${ENGINE_SOURCES} src/selftest.cpp)
    target_include_directories(bioshock1-selftest PRIVATE include)
    enable_testing()
    add_test(NAME smoke COMMAND bioshock1-selftest)
endif()
