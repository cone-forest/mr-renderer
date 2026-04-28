# mr-renderer

`mr-renderer-lib` is a Vulkan-first rendering library skeleton with a gradient example and image-based regression tests.

Key goals:

- Clang-first workflow
- C++26 as the default standard
- CMake presets for fast onboarding
- C++26 modules for the public API
- Built-in dependency workflows with both `CPM.cmake` and `vcpkg`
- Python + `ctest` image regression tests via NVIDIA FLIP CLI

## Requirements

- CMake 3.30+
- Ninja
- Clang (recommended latest available in your package manager)

On Arch Linux (example):

```bash
sudo pacman -Syu clang cmake ninja
```

## Quick Start

```bash
cmake -Dpreset=clang-debug -P cmake/configure.cmake
cmake --build --preset build-debug
ctest --preset test-debug
```

`CPM.cmake` is always used by project CMake files to fetch GoogleTest automatically.

## Quick Start (vcpkg manifest mode)

1. Bootstrap vcpkg and set `VCPKG_ROOT`.
2. Configure with vcpkg toolchain (vcpkg is still available for dependency workflows and overlays):

```bash
cmake \
  -Dpreset=clang-debug \
  -Dextra_args:STRING="-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -P cmake/configure.cmake
cmake --build --preset build-debug
ctest --preset test-debug
```

This project includes a `vcpkg.json` manifest with `gtest`.

## Project Layout

- `include/mr-renderer/`: public API headers
- `src/`: library implementation
- `tests/`: Python + CTest integration tests
- `examples/`: usage examples
- `cmake/CPM.cmake`: CPM bootstrap helper

## Gradient Example

Build and run the example:

```bash
cmake --build --preset build-debug --target gradient
./build/clang-debug/gradient
```

The executable writes output frames into `./frames_out`.

The example consumes the public C++26 module:

```cpp
import mr.renderer.lib;
```

## Image-Comparison Tests (FLIP)

The test suite includes:

- `gradient_frames_match_golden`
  - launches `simple_compute_renderer`
  - compares output against `tests/golden/gradient/`
- `kompute_graphics_interop_frames_match_golden`
  - launches `kompute_graphics_interop_renderer`
  - compares output against `tests/golden/kompute_graphics_interop_renderer/`

Each test renders frames into a temporary test output directory and compares `frame_*.png` using NVIDIA FLIP (`flip-cuda-cli`, with `flip` fallback).

Make sure `flip` is installed and available in your `PATH` before running:

```bash
ctest --preset test-debug
```

## Public API Naming

- CMake library target: `mr-renderer-lib`
- C++26 module name: `mr.renderer.lib`
- Header include prefix: `mr-renderer/...`

## Third-Party Dependencies

Core dependencies currently linked by the library:

- Vulkan SDK (`Vulkan::Vulkan`)
- Vulkan Memory Allocator (`GPUOpen::VulkanMemoryAllocator`)
- vk-bootstrap
- vkfw / GLFW
- Kompute
- TBB
- Boost headers
- stb
- Tracy
- mr-importer
- mr-utils
- mr-math

Additional packages are resolved for asset/shader and content pipelines through vcpkg and CMake dependency discovery (for example `slang`, `fastgltf`, `draco`, `ktx`, `pxr`).

## Why vcpkg / CPM.cmake over Conan (for many teams)

### vcpkg advantages

- Tight CMake integration through official toolchain file.
- Manifest mode (`vcpkg.json`) is easy for newcomers and CI.
- Fewer moving pieces for typical CMake projects.
- Strong cross-platform prebuilt binary caching support.
- Microsoft + community maintenance gives long-term confidence.

### CPM.cmake advantages

- Ultra-lightweight: one CMake include, no external daemon/profile model.
- Keeps dependency logic inside CMake, reducing mental context switching.
- Great fit for header-only and modern CMake-first ecosystems.
- Very transparent: dependencies are declared near `target_link_libraries`.
- Usually the fastest way to prototype new libraries/tools.

## Notes about "latest Clang"

`CMakePresets.json` uses `CC=clang` and `CXX=clang++`, so whichever Clang version is first in your `PATH` is selected. This is usually the newest installed version on developer machines/CI images.

## Tool Auto-Detection

- Ninja: `cmake -P cmake/configure.cmake` selects Ninja if available; otherwise it falls back to Unix Makefiles.
- mold: `CMakeLists.txt` enables `-fuse-ld=mold` for Clang/GCC only when `mold` is found; otherwise it uses the system default linker.

## C++26 Modules

The public API is exported through module `mr.renderer.lib` and consumed by the `gradient` example.
