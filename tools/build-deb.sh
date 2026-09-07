#!/usr/bin/env bash
# Assemble the .deb for a given upstream Claude Code version.
#
#   tools/build-deb.sh 2.1.263 [revision]      (blank version = current latest)
#
# The upstream binary is NOT packaged. We fetch it here only to confirm it is
# still patchable and to record the checksums the postinst will re-verify on the
# device.
#
# Integrity: Anthropic publishes a SHA-256 for each platform binary in
#   https://downloads.claude.ai/claude-code-releases/<v>/manifest.json
# We take that as authoritative and require the binary inside the npm tarball to
# match it. Two independently published channels having to agree is a good deal
# stronger than hashing one of them and trusting our own output.
#
# (That manifest is also PGP-signed as manifest.json.sig, but Anthropic does not
# publish the public key over an authenticated channel we can pin, so verifying
# it here would be theatre. Left alone deliberately.)
#
# Needs: dpkg-deb (brew install dpkg / apt install dpkg-dev), curl, python3.
# libshim.dylib must exist at packaging/payload/libshim.dylib (tools/build-shim.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CDN="https://downloads.claude.ai/claude-code-releases"

VERSION="${1:-}"
if [ -z "$VERSION" ]; then
    VERSION="$(curl -fsSL "$CDN/latest")"
    echo "==> latest is $VERSION"
fi
REVISION="${2:-$(cat "$ROOT/packaging/revision" 2>/dev/null || echo 1)}"
OUT="${OUT:-$ROOT/repo/debs}"
GH_REPO="${GH_REPO:-}"
GH_PAGES="${GH_PAGES:-}"

command -v dpkg-deb >/dev/null || { echo "need dpkg-deb (brew install dpkg)"; exit 1; }
[ -f "$ROOT/packaging/payload/libshim.dylib" ] || {
    echo "missing packaging/payload/libshim.dylib -- run tools/build-shim.sh on macOS"; exit 1; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
sha256() {
    if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

echo "==> Anthropic manifest for $VERSION"
curl -fsSL "$CDN/$VERSION/manifest.json" -o "$TMP/manifest.json"
BIN_SHA=$(python3 - "$TMP/manifest.json" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))["platforms"].get("darwin-arm64")
if not m:
    sys.exit("darwin-arm64 missing from manifest")
print(m["checksum"])
PY
)
echo "    published binary sha256: $BIN_SHA"

echo "==> npm tarball, cross-checked against that"
NPM_URL="https://registry.npmjs.org/@anthropic-ai/claude-code-darwin-arm64/-/claude-code-darwin-arm64-$VERSION.tgz"
curl -fsSL "$NPM_URL" -o "$TMP/pkg.tgz"
TGZ_SHA=$(sha256 "$TMP/pkg.tgz")
tar xzf "$TMP/pkg.tgz" -C "$TMP" package/claude
GOT=$(sha256 "$TMP/package/claude")
if [ "$GOT" != "$BIN_SHA" ]; then
    echo "!! npm and the release CDN disagree about $VERSION"
    echo "   manifest: $BIN_SHA"
    echo "   npm:      $GOT"
    exit 1
fi
echo "    npm binary matches the published checksum"
echo "    tarball sha256: $TGZ_SHA"

echo "==> is it still patchable?"
python3 "$ROOT/packaging/payload/ccios_patch.py" "$TMP/package/claude" --check

echo "==> staging"
STAGE="$TMP/stage"
LIB="$STAGE/var/jb/usr/local/lib/claude-native"
BIN="$STAGE/var/jb/usr/local/bin"
mkdir -p "$STAGE/DEBIAN" "$LIB" "$BIN"

install -m 755 "$ROOT/packaging/payload/libshim.dylib"      "$LIB/libshim.dylib"
install -m 644 "$ROOT/packaging/payload/ccios_patch.py"     "$LIB/ccios_patch.py"
install -m 644 "$ROOT/packaging/payload/shim.c"             "$LIB/shim.c"
install -m 644 "$ROOT/packaging/payload/entitlements.plist" "$LIB/entitlements.plist"
install -m 644 "$ROOT/packaging/payload/ccauth.py"          "$LIB/ccauth.py"
install -m 755 "$ROOT/packaging/payload/claude-native"      "$BIN/claude-native"
install -m 755 "$ROOT/packaging/payload/claude-login"       "$BIN/claude-login"

sed -e "s|@CC_VERSION@|$VERSION|g" \
    -e "s|@CC_BINARY_SHA256@|$BIN_SHA|g" \
    -e "s|@CC_TARBALL_SHA256@|$TGZ_SHA|g" \
    "$ROOT/packaging/payload/version.env.in" > "$LIB/version.env"
chmod 644 "$LIB/version.env"

sed -e "s|@VERSION@|$VERSION-$REVISION|g" \
    -e "s|@REPO@|$GH_REPO|g" -e "s|@PAGES@|$GH_PAGES|g" \
    "$ROOT/packaging/DEBIAN/control.in" > "$STAGE/DEBIAN/control"
[ -n "$GH_REPO"  ] || sed -i.bak '/^Icon:/d'      "$STAGE/DEBIAN/control"
[ -n "$GH_PAGES" ] || sed -i.bak '/^Depiction:/d' "$STAGE/DEBIAN/control"
rm -f "$STAGE/DEBIAN/control.bak"

install -m 755 "$ROOT/packaging/DEBIAN/postinst" "$STAGE/DEBIAN/postinst"
install -m 755 "$ROOT/packaging/DEBIAN/prerm"    "$STAGE/DEBIAN/prerm"

mkdir -p "$OUT"
DEB="$OUT/com.andi.claude-code-native_${VERSION}-${REVISION}_iphoneos-arm64.deb"
dpkg-deb -Zxz --root-owner-group --build "$STAGE" "$DEB" >/dev/null
echo "==> $DEB ($(du -h "$DEB" | cut -f1))"
