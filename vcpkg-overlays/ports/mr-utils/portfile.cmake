vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO 4J-company/mr-utils
    REF "v${VERSION}"
    SHA512 342a120a909dfccc7b79682e213fc9ff9144ab8734234f310dafffaf1da5e017971148cd08b34314d5892f08a7bc93ddb17c919290f9b2f28dd52b7d8fbe347d
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME mr-utils CONFIG_PATH lib/cmake/mr-utils)

file(
    WRITE
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/vcpkg-cmake-wrapper.cmake"
    "_find_package(libassert CONFIG REQUIRED)\n_find_package(spdlog CONFIG REQUIRED)\n_find_package(Boost CONFIG REQUIRED)\nif(TARGET Boost::boost AND NOT TARGET boost::boost)\n    add_library(boost::boost INTERFACE IMPORTED)\n    set_target_properties(boost::boost PROPERTIES INTERFACE_LINK_LIBRARIES Boost::boost)\nelseif(TARGET Boost::headers AND NOT TARGET boost::boost)\n    add_library(boost::boost INTERFACE IMPORTED)\n    set_target_properties(boost::boost PROPERTIES INTERFACE_LINK_LIBRARIES Boost::headers)\nendif()\n_find_package(mr-utils CONFIG REQUIRED)\nif(TARGET mr-utils-lib AND NOT TARGET mr-utils::mr-utils)\n    add_library(mr-utils::mr-utils INTERFACE IMPORTED)\n    set_target_properties(mr-utils::mr-utils PROPERTIES INTERFACE_LINK_LIBRARIES mr-utils-lib)\nendif()\n"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
