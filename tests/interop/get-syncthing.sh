#!/bin/bash
# Downloads the stock Syncthing release the interop tests run against (or, with --relay, its strelaysrv), checking
# the SHA-256 pinned here (from each release's sha256sum.txt.asc). Prints the binary's path.
set -euo pipefail
VERSION=v2.1.7
if [ "${1:-}" = "--relay" ]; then
	shift
	REPO=syncthing/relaysrv
	NAME="strelaysrv-linux-amd64-$VERSION"
	BIN=strelaysrv
	SHA256=93c3b8244422ecbdb05fda5b5cc644bf1b35c6462b739e2abad678e191cd5049
else
	REPO=syncthing/syncthing
	NAME="syncthing-linux-amd64-$VERSION"
	BIN=syncthing
	SHA256=2edc416e7ed136b7c858ed5f3f02a3b8316589605e54c380228ceea7acbf0142
fi
DIR="${1:-${TMPDIR:-/tmp}/xst-syncthing-$VERSION}"
if [ ! -x "$DIR/$NAME/$BIN" ]; then
	mkdir -p "$DIR"
	curl -fsSL -o "$DIR/$NAME.tar.gz" "https://github.com/$REPO/releases/download/$VERSION/$NAME.tar.gz"
	echo "$SHA256  $DIR/$NAME.tar.gz" | sha256sum -c --quiet
	tar xzf "$DIR/$NAME.tar.gz" -C "$DIR"
fi
echo "$DIR/$NAME/$BIN"
