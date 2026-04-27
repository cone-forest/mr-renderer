add_library(example-simple-compute-renderer STATIC simple_compute_renderer.cpp)
target_link_libraries(example-simple-compute-renderer PUBLIC mr-renderer-lib)
target_include_directories(
  example-simple-compute-renderer
  PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(
  example-simple-compute-renderer
  PRIVATE
    "MR_RENDERER_LIB_SHADER_DIR=\"${PROJECT_SOURCE_DIR}/shaders\""
)
mr_apply_exception_flags(example-simple-compute-renderer)

add_executable(simple_compute_renderer main.cpp)
target_link_libraries(
  simple_compute_renderer
  PRIVATE
    mr-renderer-lib
    example-simple-compute-renderer
)
target_include_directories(
  simple_compute_renderer
  PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
mr_apply_exception_flags(simple_compute_renderer)
