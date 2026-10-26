#!/usr/bin/env bash
# INITIAL DRAFT: clean-room Factorio Android build orchestrator.
# Deliberately no cleanup/deletion. Each attempt gets a unique intermediate tree;
# published APK paths are never overwritten. This script has not been build-tested.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd "$ROOT"

usage() {
    echo "Usage: $0 [/path/to/factorio_linux-arm64_VERSION.tar.xz]" >&2
    echo "   or: $0 [/path/to/factorio-space-age_linux-arm64_VERSION.tar.xz]" >&2
    exit 2
}
mkdir -p "$ROOT/input"
[[ $# -le 1 ]] || usage
if [[ $# == 1 ]]; then
    ARCHIVE=$(realpath -- "$1")
else
    matches=()
    versions=()
    for candidate in "$ROOT"/input/*.tar.xz; do
        [[ -f $candidate ]] || continue
        candidate_base=${candidate##*/}
        if [[ $candidate_base =~ ^factorio(-space-age)?_linux-arm64_([0-9]+\.[0-9]+\.[0-9]+)\.tar\.xz$ ]]; then
            matches+=("$candidate")
            versions+=("${BASH_REMATCH[2]}")
        fi
    done
    ((${#matches[@]} > 0)) || {
        echo "No supported Factorio ARM64 .tar.xz archives found directly in $ROOT/input" >&2
        exit 1
    }
    highest=$(printf '%s\n' "${versions[@]}" | sort -V | tail -n 1)
    highest_matches=()
    for i in "${!matches[@]}"; do
        [[ ${versions[$i]} == "$highest" ]] && highest_matches+=("${matches[$i]}")
    done
    ((${#highest_matches[@]} == 1)) || {
        echo "Multiple Factorio ARM64 archives match highest version $highest; pass the desired archive path explicitly:" >&2
        printf '  %s\n' "${highest_matches[@]}" >&2
        exit 1
    }
    ARCHIVE=$(realpath -- "${highest_matches[0]}")
fi
[[ -f "$ARCHIVE" ]] || { echo "Missing Factorio archive: $ARCHIVE" >&2; exit 1; }
BASE=$(basename -- "$ARCHIVE")
if [[ $BASE =~ ^factorio(-space-age)?_linux-arm64_([0-9]+\.[0-9]+\.[0-9]+)\.tar\.xz$ ]]; then
    GAME_VERSION=${BASH_REMATCH[2]}
else
    echo "Unsupported archive filename (expected Factorio Linux ARM64 versioned .tar.xz)" >&2
    exit 1
fi
APKOUT="$ROOT/build/output/apk"
APKS=("$APKOUT/factorio-$GAME_VERSION-panfrost-experimental.apk"
      "$APKOUT/factorio-$GAME_VERSION-glcompat-unverisal.apk")
for apk in "${APKS[@]}"; do
    [[ ! -e $apk && ! -L $apk ]] || { echo "Refusing to overwrite existing APK: $apk" >&2; exit 1; }
done

SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}
NDK=${ANDROID_NDK_HOME:-$SDK/ndk/android-ndk-r28c}
BT=${BUILD_TOOLS:-$SDK/build-tools/36.0.0}
ANDROID_JAR=${ANDROID_JAR:-$SDK/platforms/android-37.0/android.jar}
export PATH="$BT:$PATH"
for required in tools/port.py tools/elf.py native/fshim/CMakeLists.txt \
    native/glcompat/CMakeLists.txt patches/sdl/sdl-dynapi-android.patch \
    patches/sdl/sdl-glproc-android.patch android-wrapper/AndroidManifest.xml \
    android-wrapper/config.ini.template; do
    [[ -f "$ROOT/$required" ]] || { echo "Missing project input: $required" >&2; exit 1; }
done
[[ -d "$ROOT/android-wrapper/java" ]] || { echo "Missing Android Java sources: android-wrapper/java" >&2; exit 1; }

for tool in bash python3 tar curl git cmake ninja meson pkg-config flex bison \
    clang llvm-config glslangValidator llvm-spirv patch aapt2 d8 zipalign apksigner \
    javac jar keytool zip realpath grep sort tail find awk ln cp; do
    command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 1; }
done
python3 - <<'PY'
required_modules = ("capstone", "mako", "yaml", "packaging")
missing = []
for module in required_modules:
    try:
        __import__(module)
    except ImportError:
        missing.append(module)
if missing:
    raise SystemExit("Missing Python modules: " + ", ".join(missing))
PY
[[ -f "$NDK/build/cmake/android.toolchain.cmake" ]] || { echo "Missing Android NDK toolchain: $NDK" >&2; exit 1; }
[[ -f "$ANDROID_JAR" ]] || { echo "Missing Android platform jar: $ANDROID_JAR" >&2; exit 1; }
[[ -x "$BT/aapt2" && -x "$BT/d8" && -x "$BT/zipalign" && -x "$BT/apksigner" ]] || { echo "Incomplete Android build-tools directory: $BT" >&2; exit 1; }

mkdir -p build/intermediate build/dependencies build/output/apk
RUN=$(mktemp -d "$ROOT/build/intermediate/run.XXXXXXXX")
DEPS="$ROOT/build/dependencies"
SRC="$RUN/src"
mkdir -p "$SRC" "$RUN/build" "$RUN/stage"
echo "== initial-draft build run: $RUN =="

# Validate tar member names before extraction; extract just the executable from
# the appropriate archive's known top-level layout. Never extract untrusted paths.
case "$BASE" in
    factorio_linux-arm64_*) EXE_MEMBER="factorio/bin/arm64/factorio" ;;
    factorio-space-age_linux-arm64_*) EXE_MEMBER="factorio/bin/arm64/factorio" ;;
esac
if ! tar -tJf "$ARCHIVE" | grep -Fxq "$EXE_MEMBER"; then
    echo "Expected executable member '$EXE_MEMBER' not found in archive; inspect archive layout." >&2
    exit 1
fi
GAME_EXE="$RUN/factorio/bin/arm64/factorio"
python3 - "$ARCHIVE" "$EXE_MEMBER" "$GAME_EXE" <<'PY'
import pathlib
import shutil
import sys
import tarfile

archive, member_name, destination = sys.argv[1:]
destination = pathlib.Path(destination)
with tarfile.open(archive, mode="r:xz") as tf:
    matches = [m for m in tf.getmembers() if m.name == member_name]
    if len(matches) != 1 or not matches[0].isfile():
        raise SystemExit(f"Expected exactly one regular executable member: {member_name}")
    source = tf.extractfile(matches[0])
    if source is None:
        raise SystemExit(f"Cannot read archive member: {member_name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with source, destination.open("xb") as output:
        shutil.copyfileobj(source, output)
    destination.chmod(0o755)
PY
python3 tools/port.py --source "$GAME_EXE" \
    --out "$RUN/build/libfactorio_game.so" --report "$RUN/build/port-report.json" \
    --constants "$RUN/build/game_constants.h" --game-version "$GAME_VERSION"

# Use source releases pinned by version/tag or immutable commit. The SDL GitHub
# release tag is 3.4.14; Panfrost is pinned to the tested JimVulkan commit.
SDL_VERSION=3.4.14
SDL_URL="https://github.com/libsdl-org/SDL/archive/refs/tags/release-$SDL_VERSION.tar.gz"
SDL_ARCHIVE="$DEPS/SDL3-$SDL_VERSION.tar.gz"
if [[ ! -f $SDL_ARCHIVE ]]; then
    curl --fail --location --retry 3 "$SDL_URL" --output "$SDL_ARCHIVE"
fi
SDL_UNPACK="$RUN/dependencies/sdl-unpack"
mkdir -p "$SDL_UNPACK"
tar -xzf "$SDL_ARCHIVE" -C "$SDL_UNPACK" --no-same-owner --no-same-permissions
SDL_SOURCE=$(find "$SDL_UNPACK" -mindepth 1 -maxdepth 1 -type d -print -quit)
[[ -n $SDL_SOURCE && -f $SDL_SOURCE/CMakeLists.txt ]] || { echo "SDL archive layout unexpected" >&2; exit 1; }
cp -a "$SDL_SOURCE" "$SRC/SDL"
patch --batch --fuzz=0 -d "$SRC/SDL" -p1 --forward < patches/sdl/sdl-dynapi-android.patch
patch --batch --fuzz=0 -d "$SRC/SDL" -p1 --forward < patches/sdl/sdl-glproc-android.patch
# Intentionally do not apply the abandoned touch-hold experiment.

PANFROST_COMMIT=8af067e0a48a977334838c97f4a992bbfa100f69
PANFROST_URL=https://github.com/JimVulkan/mali-panfrost.git
PANFROST_SOURCE="$RUN/dependencies/mali-panfrost"
mkdir -p "$RUN/dependencies"
git clone --filter=blob:none --no-checkout "$PANFROST_URL" "$PANFROST_SOURCE"
git -C "$PANFROST_SOURCE" fetch --depth=1 origin "$PANFROST_COMMIT"
git -C "$PANFROST_SOURCE" checkout --detach "$PANFROST_COMMIT"

# Preserve relative source-tree expectations in native/glcompat/CMakeLists.txt
# without touching the checkout's source or vendor directories.
mkdir -p "$SRC/native" "$SRC/vendor"
cp -a native/glcompat "$SRC/native/glcompat"
cp -a "$SRC/SDL" "$SRC/vendor/SDL-release-3.4.14"

# Build each component from fresh staged inputs so existing vendor/native files
# are untouched. GLCompat's current CMake target includes etc2.c directly.
cmake -S native/fshim -B "$RUN/build/fshim" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
    -DGAME_INCLUDE_DIR="$RUN/build"
ninja -C "$RUN/build/fshim" fshim

cmake -S "$SRC/SDL" -B "$RUN/build/sdl" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_LIBC=ON
ninja -C "$RUN/build/sdl" SDL3-shared

cmake -S "$SRC/native/glcompat" -B "$RUN/build/glcompat" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release
ninja -C "$RUN/build/glcompat"

# Mesa build.sh handles host tools and cross compilation. It is invoked only in
# the disposable cloned source; BUILD_DIR redirects the Android build tree.
(
    cd "$PANFROST_SOURCE"
    PATH="$PWD/build-host/bin:$PATH" BUILD_DIR="$RUN/build/panfrost-android" \
        ./build.sh "$NDK"
)

# Package the base wrapper and backend-specific libraries from a fresh APK run.
# The established APK recipe currently expects repository-local Android Java,
# manifest and config template files. Keep intermediate artifacts run-unique.
PKG="$RUN/package"
mkdir -p "$PKG/classes" "$PKG/dex" "$PKG/assets" "$PKG/stage/lib/arm64-v8a"
cp android-wrapper/config.ini.template "$PKG/assets/config.ini.template"
find android-wrapper/java -name '*.java' -print > "$PKG/sources.txt"
javac --release 11 -Xlint:-options -cp "$ANDROID_JAR" -d "$PKG/classes" @"$PKG/sources.txt"
jar cf "$PKG/classes.jar" -C "$PKG/classes" .
d8 --release --min-api 26 --lib "$ANDROID_JAR" --output "$PKG/dex" "$PKG/classes.jar"
aapt2 link -o "$PKG/base.apk" -I "$ANDROID_JAR" -A "$PKG/assets" --manifest android-wrapper/AndroidManifest.xml \
    --min-sdk-version 26 --target-sdk-version 33 --version-code 1 --version-name "$GAME_VERSION"
KEYSTORE=${FACTORIO_KEYSTORE:-$ROOT/build/intermediate/debug.keystore}
if [[ ! -f $KEYSTORE ]]; then
    mkdir -p "$(dirname -- "$KEYSTORE")"
    keytool -genkeypair -noprompt -keystore "$KEYSTORE" -storepass android \
        -alias androiddebugkey -keypass android -keyalg RSA -keysize 2048 \
        -validity 10000 -dname 'CN=Android Debug,O=Android,C=US' >/dev/null 2>&1
fi

package_variant() {
    local variant=$1 backend=$2 suffix=$3 stage="$PKG/$1/stage" libs="$PKG/$1/libs" unsigned="$PKG/$1/unsigned.apk" aligned="$PKG/$1/aligned.apk" signed="$PKG/$1/signed.apk" target="$APKOUT/factorio-$GAME_VERSION-$3.apk"
    mkdir -p "$stage/lib/arm64-v8a" "$libs"
    cp "$RUN/build/sdl/libSDL3.so" "$libs/"
    cp "$RUN/build/fshim/lib/libfshim.so" "$stage/lib/arm64-v8a/"
    cp "$RUN/build/libfactorio_game.so" "$stage/lib/arm64-v8a/"
    cp "$libs/libSDL3.so" "$stage/lib/arm64-v8a/"
    if [[ $backend == glcompat ]]; then
        cp "$RUN/build/glcompat/lib/libGLCompat.so" "$stage/lib/arm64-v8a/"
    else
        PANBUILD="$RUN/build/panfrost-android"
        PANLIB="$stage/lib/arm64-v8a"
        cp "$PANBUILD/src/egl/libEGL_mesa.so" "$PANLIB/"
        cp "$PANBUILD/src/mesa/glapi/es2api/libGLESv2_mesa.so" "$PANLIB/"
        cp "$PANBUILD/src/gallium/targets/dri/libgallium_dri.so" "$PANLIB/"
        for name in libhardware liblog libnativewindow libsync; do
            cp "$PANBUILD/src/android_stub/$name.so" "$PANLIB/"
        done
    fi
    cp "$PKG/base.apk" "$unsigned"
    cp "$PKG/dex/classes.dex" "$stage/classes.dex"
    (cd "$stage" && zip -q -X -9 "$unsigned" classes.dex)
    mapfile -t native_entries < <(cd "$stage" && find lib/arm64-v8a -maxdepth 1 -type f -printf '%p\n' | sort)
    (cd "$stage" && zip -q -X -0 "$unsigned" "${native_entries[@]}")
    zipalign -P 16 -f 4 "$unsigned" "$aligned"
    apksigner sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
        --ks-key-alias androiddebugkey --out "$signed" "$aligned"
    # noclobber publication; never replace a pre-existing output.
    ln "$signed" "$target"
    echo "Created $target"
}
package_variant panfrost panfrost panfrost-experimental
package_variant glcompat glcompat glcompat-unverisal
