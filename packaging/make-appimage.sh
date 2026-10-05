#!/usr/bin/env bash
# Builds the AppImage: make-appimage.sh <headroom-binary> <version> <output-dir>
# Result: <output-dir>/Headroom-<version>-x86_64.AppImage.
#
# What goes in (and what does not): the program, the two DeepFilterNet networks (they sit in usr/bin/models, where the
# program looks for them relative to itself), the licences, the icon and the menu entry. NOT bundled, on purpose:
# libpipewire, ALSA, X11, freetype, fontconfig, libstdc++ and the C library. They come from the computer it runs on.
# libpipewire in particular has to match the PipeWire that is running, and the build image is old enough (see the CI job)
# for every current distribution to have newer versions of the rest.
#
# Tools: APPIMAGETOOL (default: appimagetool on PATH) and, to pin the runtime, APPIMAGE_RUNTIME_FILE. When the tool is itself
# an AppImage and there is no FUSE (a container) it is run with APPIMAGE_EXTRACT_AND_RUN=1.
# The models come from <binary folder>/models, or HEADROOM_MODELS_DIR; HEADROOM_NO_MODELS=1 builds without them.
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

[ -f "$BINARY" ] || { echo "make-appimage: binary not found: $BINARY" >&2; exit 1; }

APPIMAGETOOL="${APPIMAGETOOL:-appimagetool}"
command -v "$APPIMAGETOOL" >/dev/null 2>&1 || [ -x "$APPIMAGETOOL" ] || { echo "make-appimage: appimagetool not found (set APPIMAGETOOL)" >&2; exit 1; }

ICON="$ROOT/resources/icons/headroom.svg"
[ -f "$ICON" ] || { echo "make-appimage: $ICON not found" >&2; exit 1; }
MODELS="${HEADROOM_MODELS_DIR:-$(cd "$(dirname "$BINARY")" && pwd)/models}"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
APPDIR="$STAGE/Headroom.AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/scalable/apps" \
         "$APPDIR/usr/share/licenses/headroom"

install -m 755 "$BINARY" "$APPDIR/usr/bin/headroom"
if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$APPDIR/usr/bin/headroom" 2>/dev/null || true
fi

if [ "${HEADROOM_NO_MODELS:-0}" != "1" ]; then
    for model in DeepFilterNet3_ll_onnx.tar.gz DeepFilterNet3_onnx.tar.gz; do
        [ -f "$MODELS/$model" ] || { echo "make-appimage: $MODELS/$model not found (set HEADROOM_NO_MODELS=1 to build without the models)" >&2; exit 1; }
    done
    mkdir -p "$APPDIR/usr/bin/models"
    install -m 644 "$MODELS/DeepFilterNet3_ll_onnx.tar.gz" "$MODELS/DeepFilterNet3_onnx.tar.gz" "$APPDIR/usr/bin/models/"
    for licence in "$MODELS"/LICENSE-DeepFilterNet-*.txt; do
        [ -f "$licence" ] || continue
        install -m 644 "$licence" "$APPDIR/usr/bin/models/"
        install -m 644 "$licence" "$APPDIR/usr/share/licenses/headroom/"
    done
fi

# Licences of what is inside: Headroom itself (AGPL-3.0), the third-party notices, the fonts, the icons.
install -m 644 "$ROOT/LICENSE" "$APPDIR/usr/share/licenses/headroom/LICENSE"
install -m 644 "$ROOT/THIRD-PARTY-NOTICES.txt" "$APPDIR/usr/share/licenses/headroom/THIRD-PARTY-NOTICES.txt"
for licence in "$ROOT"/resources/fonts/OFL-*.txt "$ROOT"/resources/icons/ui/LICENSE-*.txt; do
    [ -f "$licence" ] || continue
    install -m 644 "$licence" "$APPDIR/usr/share/licenses/headroom/"
done

# The AppImage's own menu entry (what a launcher sees when it looks inside the file). Headroom puts its real one in place
# when it installs itself. X-AppImage-Integrate=false: Headroom does that job, the integration helpers need not offer it.
{
    sed -e 's|^Exec=.*|Exec=headroom --show|' "$HERE/headroom.desktop"
    echo "X-AppImage-Name=Headroom"
    echo "X-AppImage-Version=$VERSION"
    echo "X-AppImage-Integrate=false"
} > "$APPDIR/usr/share/applications/headroom.desktop"
chmod 644 "$APPDIR/usr/share/applications/headroom.desktop"
cp "$APPDIR/usr/share/applications/headroom.desktop" "$APPDIR/headroom.desktop"

install -m 644 "$ICON" "$APPDIR/usr/share/icons/hicolor/scalable/apps/headroom.svg"
cp "$ICON" "$APPDIR/headroom.svg"
ln -s headroom.svg "$APPDIR/.DirIcon"
ln -s usr/bin/headroom "$APPDIR/AppRun"

mkdir -p "$OUT_DIR"
OUT_FILE="$(cd "$OUT_DIR" && pwd)/Headroom-${VERSION}-x86_64.AppImage"
rm -f "$OUT_FILE"

TOOL_ARGS=(--no-appstream --comp zstd --mksquashfs-opt -Xcompression-level --mksquashfs-opt 19)
if [ -n "${APPIMAGE_RUNTIME_FILE:-}" ]; then
    TOOL_ARGS+=(--runtime-file "$APPIMAGE_RUNTIME_FILE")
fi

# Same input, same bytes (as far as appimagetool allows): fixed timestamps.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1577836800}"
export ARCH=x86_64
export APPIMAGE_EXTRACT_AND_RUN="${APPIMAGE_EXTRACT_AND_RUN:-1}"

"$APPIMAGETOOL" "${TOOL_ARGS[@]}" "$APPDIR" "$OUT_FILE" >&2
chmod 755 "$OUT_FILE"

echo "$OUT_FILE"
