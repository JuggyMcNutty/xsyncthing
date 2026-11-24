#!/bin/bash
# Builds what the console build needs:
#   1. the xsdk cross toolchain (GCC 15, binutils, newlib for powerpc-xsdk-elf) via xsdk's tools/toolchain/build.sh,
#      into $XSDK_TOOLCHAIN (default ~/xsdk-gcc), skipped if it's already there;
#   2. libxsdk.a, by `make` in third_party/xsdk.
# Prerequisites (Debian): build-essential bison flex texinfo libgmp-dev libmpfr-dev libmpc-dev curl xz-utils patch
# python3 cmake ninja-build git.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XSDK="$ROOT/third_party/xsdk"
export XSDK_TOOLCHAIN="${XSDK_TOOLCHAIN:-$HOME/xsdk-gcc}"
JOBS="${JOBS:-$(nproc)}"

if [ ! -f "$XSDK/Makefile" ]; then
	echo "== fetching submodules"
	git -C "$ROOT" submodule update --init --recursive
fi

missing=()
for tool in gcc g++ make bison flex makeinfo curl xz patch python3 cmake ninja git; do
	command -v "$tool" >/dev/null || missing+=("$tool")
done
if [ ${#missing[@]} -ne 0 ]; then
	echo "missing tools: ${missing[*]} (see docs/BUILDING.md)" >&2
	exit 1
fi

if [ -x "$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-gcc" ]; then
	echo "== toolchain already in $XSDK_TOOLCHAIN: $("$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-gcc" -dumpversion)"
else
	echo "== building the xsdk toolchain into $XSDK_TOOLCHAIN (this takes a while)"
	PREFIX="$XSDK_TOOLCHAIN" JOBS="$JOBS" bash "$XSDK/tools/toolchain/build.sh"
fi

echo "== building libxsdk.a"
make -C "$XSDK" -j"$JOBS" TOOLCHAIN="$XSDK_TOOLCHAIN/bin/powerpc-xsdk-elf-" build/libxsdk.a build/host/libxsc.so
echo "== done: scripts/build-xex.sh builds the title"
