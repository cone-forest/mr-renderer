cmake_minimum_required(VERSION 4.0)

project(mr-renderer-lib)
option(MR_RENDERER_ENABLE_TRACY "Enable Tracy instrumentation" ON)

# libc++ for Clang applies to vcpkg (triplet) and to FetchContent/CPM deps (Kompute, vk-bootstrap).
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
  add_link_options($<$<LINK_LANGUAGE:CXX>:-stdlib=libc++>)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmake/deps.cmake)

file(GLOB_RECURSE PRIVATE_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp
)

file(GLOB_RECURSE PRIVATE_HEADERS
  ${CMAKE_CURRENT_LIST_DIR}/src/*.hpp
)

file(GLOB_RECURSE PUBLIC_HEADERS
  ${CMAKE_CURRENT_LIST_DIR}/include/*.hpp
)

add_library(mr-renderer-lib ${PRIVATE_SOURCES} ${PRIVATE_HEADERS})
target_sources(mr-renderer-lib PUBLIC ${PUBLIC_HEADERS})
target_sources(
  mr-renderer-lib
  PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES FILES
      ${CMAKE_CURRENT_LIST_DIR}/src/mr-renderer-lib.cppm
)
set_source_files_properties(
  ${CMAKE_CURRENT_LIST_DIR}/src/mr-renderer-lib.cppm
  PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
)

target_compile_definitions(
  mr-renderer-lib
  PRIVATE
  "MR_RENDERER_LIB_SHADER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/shaders\""
  $<$<CONFIG:Debug>:MR_RENDERER_ENABLE_VK_VALIDATION=1>
  $<$<BOOL:${MR_RENDERER_ENABLE_TRACY}>:TRACY_ENABLE>
)

# dependencies
target_link_libraries(mr-renderer-lib PUBLIC ${DEPS_LIBRARIES})

target_compile_features(mr-renderer-lib PUBLIC cxx_std_26)
set_target_properties(mr-renderer-lib PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
  target_compile_options(mr-renderer-lib PRIVATE /EHs-c-)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Kompute exposes throw statements in public inline/template code, which GCC
  # rejects under -fno-exceptions during header parse.
  target_compile_options(mr-renderer-lib PRIVATE -fexceptions)
else()
  target_compile_options(mr-renderer-lib PRIVATE -fno-exceptions)
endif()

# Clang presets use triplet x64-linux-clang-libcxx so vcpkg static deps match libc++ ABI.

# headers
target_include_directories(mr-renderer-lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_precompile_headers(mr-renderer-lib PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/pch.hpp)

add_subdirectory(examples)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
