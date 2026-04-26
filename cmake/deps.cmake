file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.42.1/CPM.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
  EXPECTED_HASH SHA256=f3a6dcc6a04ce9e7f51a127307fa4f699fb2bade357a8eb4c5b45df76e1dc6a5
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

# install libraries with no binaries available
find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator REQUIRED)
find_package(Boost REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(TBB REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(draco CONFIG REQUIRED)
find_package(pxr CONFIG REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Tracy REQUIRED)
find_package(Stb REQUIRED)
find_package(mr-importer REQUIRED)
find_package(mr-utils REQUIRED)
find_package(mr-math REQUIRED)

set(MR_IMPORTER_TARGET mr-importer::mr-importer)
if(TARGET mr-importer::mr-importer)
  set(MR_IMPORTER_TARGET mr-importer::mr-importer)
elseif(TARGET mr-importer::mr-importer-lib)
  set(MR_IMPORTER_TARGET mr-importer::mr-importer-lib)
  set_property(TARGET mr-importer::mr-importer-lib PROPERTY INTERFACE_PRECOMPILE_HEADERS "")
endif()

CPMAddPackage("gh:Cvelth/vkfw#main")
if (${vkfw_ADDED})
  add_library(libvkfw INTERFACE "")
  target_link_libraries(libvkfw INTERFACE glfw)
  target_include_directories(libvkfw INTERFACE ${vkfw_SOURCE_DIR}/include)
endif()

CPMAddPackage("gh:charles-lunarg/vk-bootstrap@1.4.329")
if (${vk-bootstrap_ADDED})
  add_library(vk-bootstrap-lib INTERFACE "")
  target_link_libraries(vk-bootstrap-lib INTERFACE vk-bootstrap::vk-bootstrap)
  target_include_directories(vk-bootstrap-lib INTERFACE ${vk-bootstrap_SOURCE_DIR}/src)
endif()

CPMFindPackage(
  NAME kompute
  GITHUB_REPOSITORY KomputeProject/kompute
  GIT_TAG master
  OPTIONS
    "KOMPUTE_OPT_USE_SPDLOG ON"
    "KOMPUTE_OPT_USE_BUILT_IN_SPDLOG OFF"
    "KOMPUTE_OPT_USE_BUILT_IN_FMT OFF"
    "KOMPUTE_OPT_USE_BUILT_IN_GOOGLE_TEST OFF"
    "KOMPUTE_OPT_USE_BUILT_IN_VULKAN_HEADER OFF"
)

if(NOT TARGET stb::stb)
  add_library(stb::stb INTERFACE IMPORTED)
  set_target_properties(stb::stb PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Stb_INCLUDE_DIR}")
endif()

# set important variables
set(DEPS_LIBRARIES
  Vulkan::Vulkan
  GPUOpen::VulkanMemoryAllocator
  vk-bootstrap-lib

  boost::boost

  TBB::tbb
  kompute

  libvkfw

  stb::stb

  mr-math::mr-math
  mr-utils::mr-utils
  ${MR_IMPORTER_TARGET}

  Tracy::TracyClient
)

