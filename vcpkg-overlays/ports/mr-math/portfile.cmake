vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO 4J-company/mr-math
    REF "v${VERSION}"
    SHA512 1ebd1205de47eb9f5316b9613e326ce286aecbc91ffd778cc634bf6cf4e89cd1f5f101ae5d9085cc4fb6b734a12046a3c0bc6e8a06b7e134a58af74d3707a043
    HEAD_REF main
)

# Work around invalid alignas() in debug-only template instantiations shipped
# in mr-math/debug.hpp that break consumers built as Debug.
vcpkg_replace_string(
    "${SOURCE_PATH}/include/mr-math/math.hpp"
    "#ifndef NDEBUG\n  #include \"debug.hpp\"\n#endif\n"
    ""
)

file(GLOB MR_MATH_HEADERS "${SOURCE_PATH}/include/mr-math/*.hpp")
foreach(_mr_math_header IN LISTS MR_MATH_HEADERS)
    file(READ "${_mr_math_header}" _mr_math_content)
    string(REPLACE "alignas(T) " "" _mr_math_content "${_mr_math_content}")
    string(REPLACE "alignas(float) " "" _mr_math_content "${_mr_math_content}")
    file(WRITE "${_mr_math_header}" "${_mr_math_content}")
endforeach()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMR_MATH_ENABLE_BENCHMARK=OFF
        -DMR_MATH_ENABLE_TESTING=OFF
        -DMR_MATH_ENABLE_PROFILING=OFF
        -DMR_MATH_EXTRA_OPTIMIZED=OFF
)

vcpkg_cmake_install()

# mr-importer calls find_package(mr-math), while this project exports
# mr-math-lib. Provide a vcpkg wrapper to bridge package names.
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(
    WRITE
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/vcpkg-cmake-wrapper.cmake"
    "find_package(mr-math-lib CONFIG REQUIRED)\nif(TARGET mr::mr-math-lib AND NOT TARGET mr-math::mr-math)\n    add_library(mr-math::mr-math INTERFACE IMPORTED)\n    set_target_properties(mr-math::mr-math PROPERTIES INTERFACE_LINK_LIBRARIES mr::mr-math-lib)\nendif()\n"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
