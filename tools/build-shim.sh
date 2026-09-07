#!/usr/bin/env bash
# Build libshim.dylib for iOS. macOS only -- needs the iPhoneOS SDK.
# The device can also build it (Procursus clang + /var/jb/usr/share/SDKs), but
# shipping it prebuilt keeps clang off the package's dependency list.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
OUT="$ROOT/packaging/payload/libshim.dylib"

xcrun --sdk iphoneos clang -dynamiclib -arch arm64 \
    -miphoneos-version-min=15.0 -isysroot "$SDK" \
    -install_name @executable_path/libshim.dylib \
    -O2 "$ROOT/packaging/payload/shim.c" -o "$OUT"

# iOS refuses to load unsigned code. The postinst re-signs on device as a
# backstop, but ship it signed.
if command -v ldid >/dev/null; then
    ldid -S "$OUT"
else
    echo "warn: ldid not found -- shim left UNSIGNED (brew install ldid)" >&2
fi
echo "built $OUT"
nm -g "$OUT" | grep ' T ' | sed 's/^/    /'
