set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Match project Clang presets (-stdlib=libc++) so vcpkg static libs share libc++ ABI with mr-renderer.
# vcpkg requires VCPKG_C_FLAGS whenever VCPKG_CXX_FLAGS is customized (detect_compiler port).
string(APPEND VCPKG_C_FLAGS " ")
string(APPEND VCPKG_CXX_FLAGS " -stdlib=libc++")
string(APPEND VCPKG_LINKER_FLAGS " -stdlib=libc++")
