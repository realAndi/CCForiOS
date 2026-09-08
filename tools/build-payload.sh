#!/usr/bin/env bash
# Build everything this package ships, into packaging/payload/.
#
#   tools/build-payload.sh 2.1.263     (version required; tools/resolve-version.sh picks one)
#
# Produces, in packaging/payload/:
#   libshim.dylib     the shim, compiled against the iPhoneOS SDK and ldid-signed
#   PAYLOAD.version   the upstream version, read by build-deb.sh
#
# Needs the iPhoneOS SDK, so it has to run on macOS. The result is a ~68 KB
# dylib; the ~200 MB Claude Code binary is never touched at this stage -- it is
# not in this package at all. build-deb.sh downloads it only to checksum it, and
# the postinst fetches and patches the real thing on device.
#
# The version argument does not reach the compiler: the shim supplies symbols
# iOS's libSystem lacks and emulates JIT W^X, neither of which depends on which
# Claude Code is being wrapped. It is recorded so build-deb.sh and the payload
# cannot disagree about what was built. See CONTRACT.md in ios-port-ci.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PAYLOAD="$ROOT/packaging/payload"

# check-macho.py is shared with the other ports and lives in
# realAndi/ios-port-ci. CI checks that out and points CI_TOOLS at it; locally
# tools/ci.sh clones it on first use.
CI_TOOLS="${CI_TOOLS:-$("$ROOT/tools/ci.sh")}"

# In CI the workflow installs these because the caller declares them --
# brew-packages in publish.yml. Locally they are yours to have. Checked, never
# installed: a script that quietly fixes its own environment hides the fact that
# the caller forgot to declare it.
command -v ldid >/dev/null \
    || { echo "need ldid (brew install ldid)" >&2; exit 1; }
xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1 \
    || { echo "need the iPhoneOS SDK -- install Xcode and run xcode-select --switch" >&2; exit 1; }

VERSION="${1:?usage: $0 <upstream version, e.g. 2.1.263>}"
VERSION="${VERSION#v}"

"$ROOT/tools/build-shim.sh"

# build-shim.sh only warns when ldid is missing, so that it stays runnable on
# the device where signing may be done separately. That makes this assertion the
# thing standing between an unsigned dylib and a package that cannot load on
# anybody's phone: dyld refuses a Mach-O with no cdhash, and nothing before this
# point exits non-zero. check-macho.py rather than `codesign -dv` because it is
# pure stdlib and reads LC_CODE_SIGNATURE directly, so this check survives on a
# laptop, on Linux, and anywhere codesign is absent -- and it also confirms the
# dylib claims iOS and links nothing that iOS lacks.
echo "==> verifying the Mach-O"
python3 "$CI_TOOLS/check-macho.py" "$PAYLOAD/libshim.dylib"

printf '%s\n' "$VERSION" > "$PAYLOAD/PAYLOAD.version"

echo
echo "==> packaging/payload/libshim.dylib  $(du -h "$PAYLOAD/libshim.dylib" | cut -f1)  for Claude Code $VERSION"
echo "    next: tools/build-deb.sh"
