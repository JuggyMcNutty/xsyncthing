#!/bin/bash
# Builds the console title, build-xsdk/xsyncthing.xex. Run scripts/bootstrap.sh once first.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XSDK_TOOLCHAIN="${XSDK_TOOLCHAIN:-$HOME/xsdk-gcc}"
BUILD="${BUILD:-$ROOT/build-xsdk}"
TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/third_party/xsdk/cmake/xsdk.cmake" \
	-DXSDK_TOOLCHAIN="$XSDK_TOOLCHAIN" -DCMAKE_BUILD_TYPE="$TYPE"
cmake --build "$BUILD"
ls -l "$BUILD/xsyncthing.xex"
