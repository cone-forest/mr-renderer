#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRESET="clang-debug"
RUN_TESTS=0
FRESH=1

print_usage() {
  cat <<'EOF'
Usage: ./build.sh [options]

Options:
  --preset <name>   Configure preset to use (default: clang-debug)
  --debug           Shortcut for --preset clang-debug
  --release         Shortcut for --preset clang-release
  --gnu-debug       Shortcut for --preset gnu-debug
  --gnu-release     Shortcut for --preset gnu-release
  --test            Run tests after build (debug preset only)
  --no-fresh        Configure without --fresh
  -h, --help        Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)
      if [[ $# -lt 2 ]]; then
        echo "error: --preset requires a value"
        exit 1
      fi
      PRESET="$2"
      shift 2
      ;;
    --debug)
      PRESET="clang-debug"
      shift
      ;;
    --release)
      PRESET="clang-release"
      shift
      ;;
    --gnu-debug)
      PRESET="gnu-debug"
      shift
      ;;
    --gnu-release)
      PRESET="gnu-release"
      shift
      ;;
    --test)
      RUN_TESTS=1
      shift
      ;;
    --no-fresh)
      FRESH=0
      shift
      ;;
    -h|--help)
      print_usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1"
      print_usage
      exit 1
      ;;
  esac
done

case "$PRESET" in
  clang-debug) BUILD_PRESET="build-debug" ;;
  clang-release) BUILD_PRESET="build-release" ;;
  gnu-debug) BUILD_PRESET="build-gnu-debug" ;;
  gnu-release) BUILD_PRESET="build-gnu-release" ;;
  *)
    echo "error: unsupported preset '$PRESET'"
    echo "supported presets: clang-debug, clang-release, gnu-debug, gnu-release"
    exit 1
    ;;
esac

if [[ -z "${VCPKG_ROOT:-}" ]]; then
  VCPKG_ROOT="$HOME/.local/share/vcpkg"
fi
export VCPKG_ROOT
export VCPKG_INSTALLED_DIR="$SCRIPT_DIR/build/vcpkg_installed"

echo "==> project root: $SCRIPT_DIR"
echo "==> preset: $PRESET"
echo "==> vcpkg root: $VCPKG_ROOT"
echo "==> vcpkg installed dir: $VCPKG_INSTALLED_DIR"

if [[ -z "${CC:-}" ]]; then
  case "$PRESET" in
    gnu-debug|gnu-release) export CC=gcc ;;
    *) export CC=clang ;;
  esac
fi
if [[ -z "${CXX:-}" ]]; then
  case "$PRESET" in
    gnu-debug|gnu-release) export CXX=g++ ;;
    *) export CXX=clang++ ;;
  esac
fi
echo "==> compiler for vcpkg/cmake: CC=$CC CXX=$CXX"

if [[ ! -d "$VCPKG_ROOT/.git" ]]; then
  echo "==> vcpkg not found at VCPKG_ROOT, cloning..."
  rm -rf "$VCPKG_ROOT"
  git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
fi

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
  echo "==> bootstrapping vcpkg..."
  "$VCPKG_ROOT/bootstrap-vcpkg.sh"
fi

echo "==> installing vcpkg dependencies (manifest mode)..."
VCPKG_INSTALL_ARGS=(--overlay-ports="$SCRIPT_DIR/vcpkg-overlays/ports")
case "$PRESET" in
  clang-debug | clang-release)
    VCPKG_INSTALL_ARGS+=(
      --triplet=x64-linux-clang-libcxx
      "--overlay-triplets=$SCRIPT_DIR/vcpkg-overlays/triplets"
    )
    ;;
esac
"$VCPKG_ROOT/vcpkg" install "${VCPKG_INSTALL_ARGS[@]}"

CONFIGURE_CMD=(
  cmake
  --preset "$PRESET"
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
)

if [[ $FRESH -eq 1 ]]; then
  CONFIGURE_CMD+=(--fresh)
fi

echo "==> configuring project..."
"${CONFIGURE_CMD[@]}"

echo "==> building project..."
cmake --build --preset "$BUILD_PRESET"

if [[ $RUN_TESTS -eq 1 ]]; then
  if [[ "$PRESET" != "clang-debug" && "$PRESET" != "gnu-debug" ]]; then
    echo "error: --test is only supported with debug presets (clang-debug, gnu-debug)"
    exit 1
  fi
  echo "==> running tests..."
  if [[ "$PRESET" == "gnu-debug" ]]; then
    ctest --preset test-gnu-debug
  else
    ctest --preset test-debug
  fi
fi

echo "==> done"
