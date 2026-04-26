cmake_minimum_required(VERSION 3.30)

if(NOT DEFINED preset OR preset STREQUAL "")
  set(preset "clang-debug")
endif()

set(generator "Unix Makefiles")
find_program(ninja_executable ninja)
if(ninja_executable)
  set(generator "Ninja")
endif()

message(STATUS "Auto configure preset: ${preset}")
message(STATUS "Selected generator: ${generator}")

set(configure_command cmake --preset "${preset}" -G "${generator}")
if(DEFINED extra_args AND NOT extra_args STREQUAL "")
  list(APPEND configure_command ${extra_args})
endif()

execute_process(COMMAND ${configure_command} RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "Configuration failed with exit code ${configure_result}")
endif()
