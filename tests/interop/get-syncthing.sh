#!/bin/bash
# Downloads the stock Syncthing release the interop tests run against, checking its SHA-256 against the value
# pinned here (from the release's sha256sum.txt.asc). Prints the binary's path.
set -euo pipefail
VERSION=v2.1.7
SHA256=2edc416e7ed136b7c858ed5f3f02a3b8316589605e54c380228ceea7acbf0142
DIR="${1:-${TMPDIR:-/tmp}/xst-syncthing-$VERSION}"
NAME="syncthing-linux-amd64-$VERSION"
if [ ! -x "$DIR/$NAME/syncthing" ]; then
	mkdir -p "$DIR"
	curl -fsSL -o "$DIR/$NAME.tar.gz" "https://github.com/syncthing/syncthing/releases/download/$VERSION/$NAME.tar.gz"
	echo "$SHA256  $DIR/$NAME.tar.gz" | sha256sum -c --quiet
	tar xzf "$DIR/$NAME.tar.gz" -C "$DIR"
fi
echo "$DIR/$NAME/syncthing"
