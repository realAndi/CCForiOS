#!/usr/bin/env bash
# Which upstream Claude Code version should we build?
#
#   tools/resolve-version.sh           the version Anthropic currently ships
#   tools/resolve-version.sh 2.1.263   that version, validated
#
# Echoes the version on stdout and NOTHING else -- CI captures it. Progress and
# errors go to stderr.
#
# Runs on Linux, before the macOS job exists. See CONTRACT.md in ios-port-ci.
set -euo pipefail

CDN="https://downloads.claude.ai/claude-code-releases"

VERSION="${1:-}"
# A leading v is tolerated because it is the habit from every other port; the
# CDN paths have no v in them.
VERSION="${VERSION#v}"

if [ -z "$VERSION" ]; then
    # Anthropic's release CDN is the canonical answer; npm's dist-tag is the
    # same thing published a second way, used only if the CDN is unreachable.
    VERSION=$(curl -fsSL "$CDN/latest" || true)
    if ! printf '%s' "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo "CDN /latest unusable ('$VERSION'), falling back to npm dist-tag" >&2
        VERSION=$(curl -fsSL https://registry.npmjs.org/@anthropic-ai/claude-code \
                  | python3 -c 'import json,sys; print(json.load(sys.stdin)["dist-tags"]["latest"])')
    fi
    echo "==> latest upstream release is $VERSION" >&2
fi

printf '%s' "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || { echo "not a version: '$VERSION'" >&2; exit 1; }

# A version that does not exist would otherwise fail minutes later inside
# build-deb.sh, after paying for two checkouts and a macOS build.
curl -fsSL -o /dev/null "$CDN/$VERSION/manifest.json" \
    || { echo "Anthropic publishes no manifest for $VERSION" >&2; exit 1; }

echo "$VERSION"
