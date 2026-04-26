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
  *)
    echo "error: unsupported preset '$PRESET'"
    echo "supported presets: clang-debug, clang-release"
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
  export CC=clang
fi
if [[ -z "${CXX:-}" ]]; then
  export CXX=clang++
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
"$VCPKG_ROOT/vcpkg" install --overlay-ports="$SCRIPT_DIR/vcpkg-overlays/ports"

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
  if [[ "$PRESET" != "clang-debug" ]]; then
    echo "error: --test is only supported with clang-debug preset"
    exit 1
  fi
  echo "==> running tests..."
  ctest --preset test-debug
fi

echo "==> done"
