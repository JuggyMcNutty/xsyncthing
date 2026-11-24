#!/bin/bash
# Packages a console release: xsyncthing-<version>.zip with the .xex, the Web GUI assets (names that FATX can't hold
# are left out and listed), the CA bundle, and the docs a user needs. Copy the zip's xsyncthing/ folder to the
# console (for example Hdd:\Apps\xsyncthing) and launch xsyncthing.xex.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-xsdk}"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always)}"
OUT="${OUT:-$ROOT/dist}"
STAGE="$OUT/stage/xsyncthing"
rm -rf "$OUT/stage"
mkdir -p "$STAGE"
cp "$BUILD/xsyncthing.xex" "$STAGE/"
cp "$ROOT/assets/cacert.pem" "$STAGE/"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/NOTICES.md"
cp "$ROOT/docs/INSTALL.md" "$STAGE/README.md"
# The Web GUI, skipping names FATX can't store (42 bytes) and the Go/Markdown files that aren't served.
python3 - "$ROOT/assets/gui" "$STAGE/gui" <<'PY'
import os, shutil, sys
src, dst = sys.argv[1:]
skipped = []
renamed = []
for d, dirs, files in os.walk(src):
    rel = os.path.relpath(d, src)
    if any(len(p.encode()) > 42 for p in ([] if rel == "." else rel.split(os.sep))):
        continue
    os.makedirs(os.path.join(dst, rel), exist_ok=True)
    for f in files:
        if f.endswith((".go", ".md")):
            continue
        out = f
        if len(f.encode()) > 42:
            # A licence text whose name FATX can't hold keeps its content under a shorter name (it isn't served).
            if f.endswith(".LICENSE"):
                out = f[: 42 - len(".LICENSE")].rstrip(".-") + ".LICENSE"
                renamed.append((os.path.join(rel, f), os.path.join(rel, out)))
            else:
                skipped.append(os.path.join(rel, f))
                continue
        shutil.copy2(os.path.join(d, f), os.path.join(dst, rel, out))
for a, b in renamed:
    print(f"renamed for FATX: {a} -> {b}")
for s in skipped:
    print("left out (name over 42 bytes):", s)
if skipped:
    sys.exit("files that the Web GUI serves have names too long for FATX")
PY
mkdir -p "$OUT"
rm -f "$OUT/xsyncthing-$VERSION.zip"
(cd "$OUT/stage" && python3 -c "
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, fs in os.walk('xsyncthing'):
        for f in sorted(fs):
            z.write(os.path.join(d, f))
" "$OUT/xsyncthing-$VERSION.zip")
rm -rf "$OUT/stage"
ls -l "$OUT/xsyncthing-$VERSION.zip"
