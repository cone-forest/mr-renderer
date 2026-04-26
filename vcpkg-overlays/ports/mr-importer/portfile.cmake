vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO 4J-company/mr-importer
    REF "v${VERSION}"
    SHA512 432494336a29280248fcbd1fd343c4ea838a26f297bec1d0bfdd45ae74d5e103b6274822ad88ece2ea5150ba02c116d1003972fb88442e6da31f6ffb68ca9a64
    HEAD_REF main
)

# Upstream links OpenSubdiv via a Conan-specific target name that is not
# provided by vcpkg's OpenSubdiv package.
vcpkg_replace_string(
    "${SOURCE_PATH}/cmake/deps.cmake"
    "  OpenSubdiv::osdgpu_static\n"
    ""
)
vcpkg_replace_string(
    "${SOURCE_PATH}/cmake/deps.cmake"
    "  openusd::openusd"
    "  usd"
)
vcpkg_replace_string(
    "${SOURCE_PATH}/CMakeLists.txt"
    "    src/mr-importer/loader.cpp\n"
    ""
)
vcpkg_replace_string(
    "${SOURCE_PATH}/CMakeLists.txt"
    "    src/mr-importer/usd_loader.cpp\n"
    ""
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMR_IMPORTER_BUILD_TESTS=OFF
        -DMR_IMPORTER_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME mr-importer CONFIG_PATH lib/cmake/mr-importer)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
