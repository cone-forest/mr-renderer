find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GRADIENT_TEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/test_gradient.py")
set(GRADIENT_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden/gradient")
set(GRADIENT_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/gradient_frames")
set(INTEROP_TEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/test_kompute_graphics_interop.py")
set(INTEROP_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden/kompute_graphics_interop_renderer")
set(INTEROP_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/kompute_graphics_interop_frames")

if(EXISTS "${GRADIENT_GOLDEN_DIR}")
  add_test(
    NAME gradient_frames_match_golden
    COMMAND
      "${Python3_EXECUTABLE}" "${GRADIENT_TEST_SCRIPT}"
      --example "$<TARGET_FILE:simple_compute_renderer>"
      --golden-dir "${GRADIENT_GOLDEN_DIR}"
      --output-dir "${GRADIENT_OUTPUT_DIR}"
  )
else()
  message(STATUS "Skipping gradient_frames_match_golden: missing golden directory ${GRADIENT_GOLDEN_DIR}")
endif()

if(EXISTS "${INTEROP_GOLDEN_DIR}")
  add_test(
    NAME kompute_graphics_interop_frames_match_golden
    COMMAND
      "${Python3_EXECUTABLE}" "${INTEROP_TEST_SCRIPT}"
      --example "$<TARGET_FILE:kompute_graphics_interop_renderer>"
      --golden-dir "${INTEROP_GOLDEN_DIR}"
      --output-dir "${INTEROP_OUTPUT_DIR}"
  )
else()
  message(
    STATUS
    "Skipping kompute_graphics_interop_frames_match_golden: missing golden directory ${INTEROP_GOLDEN_DIR}"
  )
endif()
