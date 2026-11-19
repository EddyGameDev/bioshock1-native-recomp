cmake_minimum_required(VERSION 3.25)

if (WIN32)
  add_library(
    rexgpu-xenos-custom SHARED
    src/xenos_hlsl.cpp
    src/render_state.cpp
    src/render_pipeline.cpp
    src/render_shaders.cpp
    src/render_resources.cpp
    src/render_commands.cpp
    src/render_d3d11.cpp
    src/pm4_processor.cpp
    src/custom_driver.cpp
    src/custom_provider.cpp
  )

  set_target_properties(
    rexgpu-xenos-custom
    PROPERTIES OUTPUT_NAME "rexgpu-xenos-custom"
               DEBUG_POSTFIX "d"
               RELWITHDEBINFO_POSTFIX "rd"
               CXX_STANDARD 23
               CXX_STANDARD_REQUIRED ON
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
               LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )

  target_include_directories(rexgpu-xenos-custom PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(rexgpu-xenos-custom PRIVATE rexgpu rexxenos rexbase d3d11 dxgi)
endif ()
