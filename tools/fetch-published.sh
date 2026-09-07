#!/usr/bin/env bash
# Pull the packages already on the live repo into repo/debs/ so a rebuild adds
# to the version list instead of replacing it. Without this the repo only ever
# offers the newest build and there is no way to roll back a bad one.
#
#   tools/fetch-published.sh https://owner.github.io/CCForiOS/ [keep]
set -euo pipefail

BASE="${1:?usage: $0 <repo-base-url> [keep-count]}"
KEEP="${2:-10}"
BASE="${BASE%/}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEBS="$ROOT/repo/debs"
mkdir -p "$DEBS"

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

if ! curl -fsSL "$BASE/Packages" -o "$TMP/Packages" 2>/dev/null; then
    echo "no published Packages at $BASE (first run?) -- nothing to carry forward"
    exit 0
fi

# Filename: lines are relative to the repo root.
grep '^Filename:' "$TMP/Packages" | awk '{print $2}' | while read -r rel; do
    name="$(basename "$rel")"
    if [ -f "$DEBS/$name" ]; then
        echo "  = $name (rebuilt this run)"
        continue
    fi
    if curl -fsSL "$BASE/$rel" -o "$DEBS/$name"; then
        echo "  + $name (carried forward)"
    else
        echo "  ! could not fetch $rel -- skipping"
        rm -f "$DEBS/$name"
    fi
done

# Prune by VERSION, not by mtime. Sorting by mtime looks equivalent and is not:
# the carried-forward packages are downloaded *after* the fresh build, so they
# get newer timestamps than it, and the newest build is the first thing pruned.
# That silently republished stale packages while the workflow reported success.
count=$(ls -1 "$DEBS"/*.deb 2>/dev/null | wc -l | tr -d ' ')
if [ "$count" -gt "$KEEP" ]; then
    ls -1 "$DEBS"/*.deb | sort -Vr | tail -n +$((KEEP + 1)) | while read -r old; do
        echo "  - $(basename "$old") (pruned, keeping $KEEP)"
        rm -f "$old"
    done
fi
echo "repo/debs now holds $(ls -1 "$DEBS"/*.deb 2>/dev/null | wc -l | tr -d ' ') package(s)"
