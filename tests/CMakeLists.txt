find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GRADIENT_TEST_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/test_gradient.py")
set(GRADIENT_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden/gradient")
set(GRADIENT_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/gradient_frames")

add_test(
  NAME gradient_frames_match_golden
  COMMAND
    "${Python3_EXECUTABLE}" "${GRADIENT_TEST_SCRIPT}"
    --example "$<TARGET_FILE:gradient>"
    --golden-dir "${GRADIENT_GOLDEN_DIR}"
    --output-dir "${GRADIENT_OUTPUT_DIR}"
)
