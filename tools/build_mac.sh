#!/usr/bin/env bash
# Build the HOT-STEP LINK Mac beta (universal arm64 + x86_64, ad-hoc signed) and stage it in dist/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JOBS="${JOBS:-4}"
BUILD_DIR="$ROOT/build-mac"
TARGET="HOTSTEP_LINK"
NAME="HOT-STEP LINK"

VER="$(sed -n 's/^project(.* VERSION \([0-9][0-9.]*\)).*/\1/p' CMakeLists.txt | head -n1)"
if [ -z "$VER" ]; then
    echo "error: could not read version from CMakeLists.txt" >&2
    exit 1
fi
echo "HOT-STEP LINK version $VER"

STAMP="$(date +%Y%m%d-%H%M%S)"
trash() {
    # Never rm -rf: move old output to ~/.Trash instead.
    local p="$1"
    if [ -e "$p" ]; then
        mkdir -p "$HOME/.Trash"
        mv "$p" "$HOME/.Trash/$(basename "$p")-$STAMP"
        echo "moved old $(basename "$p") to ~/.Trash"
    fi
}

cmake -S "$ROOT" -B "$BUILD_DIR" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0

cmake --build "$BUILD_DIR" --config Release -j "$JOBS" \
    --target "${TARGET}_AU" "${TARGET}_VST3" "${TARGET}_CLAP" "${TARGET}_Standalone"

ART="$BUILD_DIR/${TARGET}_artefacts/Release"

# Ad-hoc deep sign every bundle (no developer identity, no notarization).
for b in "$ART/AU/$NAME.component" "$ART/VST3/$NAME.vst3"          "$ART/CLAP/$NAME.clap" "$ART/Standalone/$NAME.app"; do
    codesign --force --deep --sign - "$b"
    echo "signed (ad-hoc): $b"
done
DIST="$ROOT/dist/LINK_${VER}_beta_mac"
trash "$DIST"
mkdir -p "$DIST/AU" "$DIST/VST3" "$DIST/CLAP" "$DIST/Standalone"

cp -R "$ART/AU/$NAME.component"   "$DIST/AU/"
cp -R "$ART/VST3/$NAME.vst3"      "$DIST/VST3/"
cp -R "$ART/CLAP/$NAME.clap"      "$DIST/CLAP/"
cp -R "$ART/Standalone/$NAME.app" "$DIST/Standalone/"
if [ -f "$ROOT/docs/mac/QUICKSTART_MAC.md" ]; then
    cp "$ROOT/docs/mac/QUICKSTART_MAC.md" "$DIST/QUICKSTART.md"
fi

# Verify every binary is universal.
fail=0
for bin in \
    "$DIST/AU/$NAME.component/Contents/MacOS/$NAME" \
    "$DIST/VST3/$NAME.vst3/Contents/MacOS/$NAME" \
    "$DIST/CLAP/$NAME.clap/Contents/MacOS/$NAME" \
    "$DIST/Standalone/$NAME.app/Contents/MacOS/$NAME"; do
    if [ ! -f "$bin" ]; then
        echo "MISSING: $bin" >&2
        fail=1
        continue
    fi
    archs="$(lipo -archs "$bin")"
    for a in arm64 x86_64; do
        case " $archs " in
            *" $a "*) ;;
            *) echo "FAIL: $bin lacks $a (has: $archs)" >&2; fail=1 ;;
        esac
    done
    echo "ok: $bin [$archs]"
done

# Verify the ad-hoc signature on every staged bundle.
for b in "$DIST/AU/$NAME.component" "$DIST/VST3/$NAME.vst3"          "$DIST/CLAP/$NAME.clap" "$DIST/Standalone/$NAME.app"; do
    if codesign --verify --deep --strict --verbose=2 "$b"; then
        echo "codesign ok: $b"
    else
        echo "CODESIGN FAIL: $b" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "universal/codesign check failed" >&2
    exit 1
fi
echo "staged: $DIST"
