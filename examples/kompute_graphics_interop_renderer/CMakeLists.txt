add_library(example-kompute-graphics-interop-renderer STATIC kompute_graphics_interop_renderer.cpp)
target_link_libraries(example-kompute-graphics-interop-renderer PUBLIC mr-renderer-lib)
target_include_directories(
  example-kompute-graphics-interop-renderer
  PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(
  example-kompute-graphics-interop-renderer
  PRIVATE
    "MR_RENDERER_LIB_SHADER_DIR=\"${PROJECT_SOURCE_DIR}/shaders\""
)
mr_apply_exception_flags(example-kompute-graphics-interop-renderer)
