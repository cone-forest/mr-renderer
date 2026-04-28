function(mr_apply_exception_flags target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /EHs-c-)
  elseif(
    CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
    CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Kompute exposes throw statements in public inline/template code, which
    # causes parse-time errors under -fno-exceptions.
    target_compile_options(${target_name} PRIVATE -fexceptions)
  else()
    target_compile_options(${target_name} PRIVATE -fno-exceptions)
  endif()
endfunction()

function(mr_apply_tracy_flags target_name)
  if(MR_RENDERER_ENABLE_TRACY)
    target_compile_definitions(${target_name} PRIVATE TRACY_ENABLE)
  endif()
endfunction()

add_subdirectory(simple_compute_renderer)
add_subdirectory(kompute_graphics_interop_renderer)
