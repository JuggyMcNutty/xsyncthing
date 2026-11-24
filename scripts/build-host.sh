#!/bin/bash
# Builds the portable core and the host binary in build-host/ and runs the unit tests.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-host}"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
