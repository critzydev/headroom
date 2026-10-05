#!/usr/bin/env bash
# Builds the release tarball: make-tarball.sh <headroom-binary> <version> <output-dir>
# Result: <output-dir>/headroom-<version>-x86_64.tar.gz containing the folder "headroom/".
# The DeepFilterNet models (Fast and Best noise removal) come from <binary folder>/models, where the build puts them;
# HEADROOM_MODELS_DIR points somewhere else, HEADROOM_NO_MODELS=1 packages without them (a -DHEADROOM_DFN=OFF build).
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <headroom-binary> <version> <output-dir>" >&2
    exit 2
fi

BINARY="$1"
VERSION="$2"
OUT_DIR="$3"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

[ -f "$BINARY" ] || { echo "make-tarball: binary not found: $BINARY" >&2; exit 1; }

ICON="$ROOT/resources/icons/headroom.svg"
MODELS="${HEADROOM_MODELS_DIR:-$(cd "$(dirname "$BINARY")" && pwd)/models}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

DEST="$STAGE/headroom"
mkdir -p "$DEST/share"

install -m 755 "$BINARY" "$DEST/headroom"
# Smaller download; keep the unstripped build in CI artifacts if you need symbols.
if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$DEST/headroom" 2>/dev/null || true
fi

install -m 755 "$HERE/install.sh"   "$DEST/install.sh"
install -m 755 "$HERE/uninstall.sh" "$DEST/uninstall.sh"
install -m 644 "$HERE/README.txt"   "$DEST/README.txt"

# Licences of what is inside: Headroom itself (AGPL-3.0), the third-party notices, the fonts, the icons.
mkdir -p "$DEST/licenses"
install -m 644 "$ROOT/LICENSE" "$ROOT/THIRD-PARTY-NOTICES.txt" "$DEST/licenses/"
for licence in "$ROOT"/resources/fonts/OFL-*.txt "$ROOT"/resources/icons/ui/LICENSE-*.txt; do
    [ -f "$licence" ] || continue
    install -m 644 "$licence" "$DEST/licenses/"
done

if [ "${HEADROOM_NO_MODELS:-0}" != "1" ]; then
    for model in DeepFilterNet3_ll_onnx.tar.gz DeepFilterNet3_onnx.tar.gz; do
        [ -f "$MODELS/$model" ] || { echo "make-tarball: $MODELS/$model not found (set HEADROOM_NO_MODELS=1 to package without the models)" >&2; exit 1; }
    done
    mkdir -p "$DEST/models"
    install -m 644 "$MODELS/DeepFilterNet3_ll_onnx.tar.gz" "$MODELS/DeepFilterNet3_onnx.tar.gz" "$DEST/models/"
    for licence in "$MODELS"/LICENSE-DeepFilterNet-*.txt; do
        [ -f "$licence" ] || continue
        install -m 644 "$licence" "$DEST/models/"
    done
fi

install -m 644 "$HERE/headroom.desktop"      "$DEST/share/headroom.desktop"
install -m 644 "$HERE/headroom.service"      "$DEST/share/headroom.service"
install -m 644 "$HERE/51-headroom-wave.conf" "$DEST/share/51-headroom-wave.conf"

if [ -f "$ICON" ]; then
    install -m 644 "$ICON" "$DEST/share/headroom.svg"
else
    echo "make-tarball: $ICON not found, using a plain placeholder icon" >&2
    cat > "$DEST/share/headroom.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 128 128">
  <rect width="128" height="128" rx="24" fill="#16171A"/>
  <rect x="34" y="30" width="22" height="68" rx="4" fill="#E9A23B"/>
  <rect x="72" y="52" width="22" height="46" rx="4" fill="#E9A23B"/>
</svg>
SVG
fi

mkdir -p "$OUT_DIR"
OUT_FILE="$(cd "$OUT_DIR" && pwd)/headroom-${VERSION}-x86_64.tar.gz"

# Same input, same bytes: fixed owner, order and timestamps.
tar --owner=0 --group=0 --numeric-owner --sort=name --mtime='UTC 2020-01-01' \
    -C "$STAGE" -cf - headroom | gzip -9n > "$OUT_FILE"

echo "$OUT_FILE"
