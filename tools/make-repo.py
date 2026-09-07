#!/usr/bin/env python3
"""Generate the APT metadata Sileo needs, next to repo/debs/.

    tools/make-repo.py [repo-dir]

Produces a flat repo: Packages(+.gz/.xz/.bz2) and Release at the root, with
Filename: pointing into debs/. Uses `dpkg-deb -f` to read each package's
control, so it needs no dpkg-scanpackages.
"""
import bz2
import gzip
import hashlib
import lzma
import os
import subprocess
import sys
from datetime import datetime, timezone

ORIGIN = os.environ.get("REPO_ORIGIN", "CCForiOS")
LABEL = os.environ.get("REPO_LABEL", "Claude Code for iOS")
DESCRIPTION = os.environ.get(
    "REPO_DESCRIPTION", "Native Claude Code CLI, patched to run on jailbroken iOS")

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "repo")
DEBS = os.path.join(ROOT, "debs")


def digest(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if not os.path.isdir(DEBS):
        raise SystemExit("no %s -- build a package first" % DEBS)

    stanzas = []
    for name in sorted(os.listdir(DEBS)):
        if not name.endswith(".deb"):
            continue
        path = os.path.join(DEBS, name)
        control = subprocess.check_output(["dpkg-deb", "-f", path]).decode().rstrip("\n")
        stanzas.append("\n".join([
            control,
            "Filename: debs/%s" % name,
            "Size: %d" % os.path.getsize(path),
            "MD5sum: %s" % digest(path, "md5"),
            "SHA1: %s" % digest(path, "sha1"),
            "SHA256: %s" % digest(path, "sha256"),
        ]))
        print("  + %s" % name)

    packages = ("\n\n".join(stanzas) + "\n").encode()
    written = {}
    for suffix, data in (
        ("", packages),
        (".gz", gzip.compress(packages, 9)),
        (".bz2", bz2.compress(packages, 9)),
        (".xz", lzma.compress(packages, preset=9)),
    ):
        p = os.path.join(ROOT, "Packages" + suffix)
        with open(p, "wb") as f:
            f.write(data)
        written["Packages" + suffix] = p

    lines = [
        "Origin: %s" % ORIGIN,
        "Label: %s" % LABEL,
        "Suite: stable",
        "Version: 1.0",
        "Codename: ios",
        "Architectures: iphoneos-arm64",
        "Components: main",
        "Description: %s" % DESCRIPTION,
        "Date: %s" % datetime.now(timezone.utc).strftime("%a, %d %b %Y %H:%M:%S UTC"),
    ]
    for algo, header in (("md5", "MD5Sum"), ("sha256", "SHA256")):
        lines.append("%s:" % header)
        for rel in sorted(written):
            p = written[rel]
            lines.append(" %s %d %s" % (digest(p, algo), os.path.getsize(p), rel))

    with open(os.path.join(ROOT, "Release"), "w") as f:
        f.write("\n".join(lines) + "\n")

    print("wrote Packages{,.gz,.bz2,.xz} and Release in %s" % ROOT)
    print("%d package(s)" % len(stanzas))


main()
