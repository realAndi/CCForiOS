# CCForiOS

Anthropic's Claude Code CLI, running natively on jailbroken iOS, distributed as
a Sileo/APT package.

Upstream stopped shipping a JavaScript `cli.js` after 2.1.112. From 2.1.113 the
npm package is only an installer that fetches a Bun-compiled native executable,
and there is no iOS build. This project patches the `darwin-arm64` executable so
that iOS's dyld will load it and JavaScriptCore's JIT will run inside it, and
packages the patcher so the whole thing installs, updates and rolls back through
APT like any other package.

| | |
|---|---|
| Sileo source | `https://realandi.github.io/CCForiOS/` |
| Package | `com.andi.claude-code-native` ("Claude Code (native)") |
| Source | https://github.com/realAndi/CCForiOS |
| Current upstream | Claude Code 2.1.263 |
| Verified on | iPhone 15 Pro (A17 Pro), iOS 17.3 (21D50), Dopamine rootless (`/var/jb`, Procursus) |

## Installing

Sileo → Sources → **+** → `https://realandi.github.io/CCForiOS/`, then install
**Claude Code (native)**.

Requirements:

* a rootless jailbreak (`/var/jb`); only Dopamine on iOS 17.3 / arm64 has been tested
* a network connection during install — the package does not contain Claude
  Code, it fetches it (see [What the package contains](#what-the-package-contains))
* about 700 MB free during install, about 200 MB afterwards
* `python3`, `ldid`, `curl`, `tar` (declared as dependencies; Sileo pulls them in)

Install takes 5–7 seconds: download, checksum, patch, sign, smoke-test, move
into place.

The repo is not GPG-signed, like most jailbreak repos. Sileo does not mind. The
`apt` CLI does, so if you add the source by hand it needs `[trusted=yes]`:

```sh
echo 'deb [trusted=yes] https://realandi.github.io/CCForiOS/ ./' \
  | sudo tee /var/jb/etc/apt/sources.list.d/ccforios.list
sudo apt update && sudo apt install com.andi.claude-code-native
```

Then run `claude`. `/var/jb/usr/local/bin` is on the PATH of a *login* shell,
which is what NewTerm gives you, but not of a non-interactive `ssh host 'cmd'`.
Over SSH use:

```sh
ssh phone 'zsh -l -c "claude -p \"reply with the single word OK\""'
```

### What works

`claude --version`, the full `--help`, `claude doctor` (reports
`Running: native (2.1.263)`, `Platform: darwin-arm64`, `Search: OK (bundled)`),
interactive and `-p` sessions against the API with TLS, retry/backoff, MCP
registry, LSP manager and telemetry all starting and shutting down cleanly.
Tools exercised end to end: Bash (subprocess spawn), Grep, Read, Glob. Search is
served by Claude Code's own implementation; no ripgrep is needed. Three
consecutive multi-tool sessions ran with no panic and no bus error.

Startup is about 500 ms. A simple prompt round-trips in under 3 s (2.75 s
measured).

## How it works

### What the macOS binary needs

The upstream `darwin-arm64` executable is a thin Mach-O 64-bit arm64 binary,
`MH_EXECUTE`, PIE, two-level namespace, 21 load commands, `LC_BUILD_VERSION`
platform 1 (macOS) minos 13.0 sdk 26.5, 199,257,984 bytes. It links exactly four
dylibs, every one of which exists on iOS — no frameworks, no `/Versions/`, no
`@rpath`:

```
/usr/lib/libicucore.A.dylib
/usr/lib/libresolv.9.dylib
/usr/lib/libc++.1.dylib
/usr/lib/libSystem.B.dylib
```

It has 855 chained-fixup imports covering 840 unique undefined symbols. Running
`dlsym` for every one of them against those four dylibs on the device found
**four** that iOS 17.3's libSystem does not export:

```
___clear_cache
_posix_spawn_file_actions_addfchdir
_pthread_jit_write_protect_np
_pthread_jit_write_protect_supported_np
```

So the port is small: tell dyld the binary is for iOS, supply four functions,
fix a set of macOS-only path strings, and make the JIT work.

### The patch

`packaging/payload/ccios_patch.py` makes three changes to the Mach-O, in pure
Python with no cctools dependency so it can run on the phone. The result is
then re-signed with `ldid -S entitlements.plist`.

**1. `LC_BUILD_VERSION` platform → iOS (2), minos 15.0, sdk 17.0.**
On its own this is enough for dyld to accept the binary; the very first launch
after this patch alone got all the way to symbol resolution and died with
`dyld: Symbol not found: ___clear_cache`.

**2. Link the shim and repoint five imports at it.**
The patcher appends an `LC_LOAD_DYLIB` for `@executable_path/libshim.dylib`
(ordinal 5, after the four existing dylibs) and, inside
`LC_DYLD_CHAINED_FIXUPS`, rewrites the `lib_ordinal` of five imports from 4
(libSystem) to 5 (the shim): the four missing symbols above plus `_mmap`, which
the JIT emulation needs to intercept (see below).

`DYLD_INSERT_LIBRARIES` cannot do this job, for two independent reasons, both
tested: dyld ignores every `DYLD_*` variable for a binary carrying entitlements
(and this one needs JIT entitlements), and the import table is two-level — each
import names libSystem specifically — so an inserted library would never win the
lookup even if the variables were honoured. `DYLD_FORCE_FLAT_NAMESPACE=1` made
no difference either.

**3. Rewrite the macOS framework paths.**
Bun `dlopen()`s CoreFoundation at runtime by its macOS bundle path,
`/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation`.
On iOS the binary sits directly inside the bundle, and the macOS path fails:

```
.../CoreFoundation.framework/Versions/A/CoreFoundation   dlopen fails
.../CoreFoundation.framework/CoreFoundation              OK
```

Without this fix the binary panics with `panic: Cannot Load CoreFoundation`.
Shimming `dlopen` is not an option (the shim needs `dlopen` to find the real
`dlopen`), so the patcher rewrites the string data instead: 38 C strings
containing `.framework/Versions/` are rewritten in place with the `Versions/X/`
component removed and re-terminated. The replacement is always shorter, so the
tail of the old string is simply left behind.

### Why JIT is not optional

With the four symbols supplied, `claude --version` worked and `--help` died with
`ReferenceError: SharedArrayBuffer is not defined`. `BUN_JSC_dumpOptions=2`
showed why:

```
useJIT=false
useSharedArrayBuffer=false
```

JavaScriptCore forces `useSharedArrayBuffer=false` whenever `useJIT=false`.
Forcing `BUN_JSC_useJIT=0 useWasm=1 useSharedArrayBuffer=1` still comes back
`useSharedArrayBuffer=false`. And Claude Code needs a SharedArrayBuffer at
startup — `new Int32Array(new SharedArrayBuffer(4))`, the Atomics-based
synchronous sleep in its file-lock retry loop. So without a working JIT the
binary cannot get past startup. Interpreter-only is not a fallback here.

macOS-arm64 JSC maps its executable pool with `MAP_JIT` and flips per-thread
W^X with `pthread_jit_write_protect_np()`. Neither exists on iOS. Probing what
iOS actually allows, each strategy in a forked child so a fault does not end the
run:

| strategy | result |
|---|---|
| `mmap` RW | OK |
| `mmap` RWX | mapping succeeds |
| `mmap` with `MAP_JIT` (any prot) | **fails, `EINVAL`** |
| write to an RWX page, then execute it | **`SIGBUS`** |
| `mmap` RW → write → `mprotect` RX → execute | **works** |
| `mmap` RWX → write → `mprotect` RX → execute | **works** |

iOS enforces W^X: it will not execute a page that is *currently* writable, but
it will let `mprotect` flip a page between RW and RX. That is enough. Dopamine's
`jbctl proc_set_debugged` does set `CS_DEBUGGED`, but it turned out not to be
required — the `mprotect` flip works without it.

### The shim

`packaging/payload/shim.c` is built against the iPhoneOS SDK into a 68 KB
`libshim.dylib` and does two jobs:

**JIT memory.**

* `mmap` — when called with `MAP_JIT` (0x0800), strips the flag, maps the region
  RW instead of RWX, and records it. This is JSC's executable pool, 64 MB.
* `pthread_jit_write_protect_np(enabled)` — `mprotect`s every recorded region to
  RX when `enabled` is 1 ("done writing, must be executable") and RW when 0
  ("about to write").
* `pthread_jit_write_protect_supported_np()` — returns 1.

The real `mmap` is resolved with `dlsym` on an explicit libSystem handle, not
`RTLD_DEFAULT`: the default search walks the global list and finds the shim's
own `mmap` first. A guard aborts with `[ccios] fatal: mmap resolved to the shim
itself` rather than recursing, should that ever happen.

**The two other missing symbols.**

* `__clear_cache` → `sys_icache_invalidate`, iOS's spelling of the same operation
  (the JIT calls it after emitting code).
* `posix_spawn_file_actions_addfchdir` → `posix_spawn_file_actions_addfchdir_np`.
  The un-suffixed spelling postdates iOS 17; the `_np` one is present on 17.3.

With the shim linked, `BUN_JSC_dumpOptions=2` reports `useJIT=true` and
`useSharedArrayBuffer=true`.

### The one load-bearing setting

The real `pthread_jit_write_protect_np` is **per-thread**; `mprotect` is
**process-wide**. With JSC's concurrent JIT enabled, a background compiler
thread flips the pool to RW while the main thread is executing from it, and Bun
dies with `panic: Bus error`.

The wrapper therefore sets:

```
BUN_JSC_useConcurrentJIT=0
```

Compilation happens on the mutator thread instead, so nothing flips the pool
underneath running code. Baseline, DFG and FTL all stay enabled; only the
concurrency does not. **Never drop this setting.**

### Runtime environment

`/var/jb/usr/local/bin/claude-native` (which `claude` symlinks to) sets:

| variable | why |
|---|---|
| `BUN_JSC_useConcurrentJIT=0` | see above |
| `DISABLE_AUTOUPDATER=1` | Claude Code's own updater would fetch the unpatched macOS binary and replace a working install with one that cannot load. No iOS build exists upstream. |
| `HOME` | defaulted to `/var/jb/var/mobile` if unset |

and `exec`s `/var/jb/usr/local/lib/claude-native/claude`. `libshim.dylib` must
sit beside the binary — it is loaded as `@executable_path/libshim.dylib`.

Set `CCIOS_SHIM_DEBUG=1` to have the shim print the JIT pool mapping
(`[ccios] JIT pool 0x... len=67108864 prot=...`) and any `mprotect` failure to
stderr.

## What the package contains

**Not Claude Code.** The `.deb` is about 12 KB and ships only:

| file | installed to | |
|---|---|---|
| `libshim.dylib` | `/var/jb/usr/local/lib/claude-native/` | the 68 KB shim, prebuilt against the iPhoneOS SDK |
| `ccios_patch.py` | `/var/jb/usr/local/lib/claude-native/` | the Mach-O patcher |
| `shim.c` | `/var/jb/usr/local/lib/claude-native/` | shim source, so it can be rebuilt on device |
| `entitlements.plist` | `/var/jb/usr/local/lib/claude-native/` | JIT entitlements (`dynamic-codesigning`, `com.apple.security.cs.allow-jit`, `get-task-allow`, ...) for `ldid` |
| `version.env` | `/var/jb/usr/local/lib/claude-native/` | upstream version, pinned SHA-256s, download URLs |
| `claude-native` | `/var/jb/usr/local/bin/` | the wrapper |

The binary itself is produced at install time by `packaging/DEBIAN/postinst`:

1. Check for ~700 MB free under `/var/jb/usr/local/lib/claude-native` (the
   download, the patched copy and the installed copy briefly coexist).
2. Stage in a temp directory *inside* that lib directory, not `/tmp`. The
   sandbox refuses to `mmap()` a dylib out of `/var/tmp`, so the smoke test in
   step 6 could not load `libshim.dylib` from there; staying on the same
   filesystem also makes the final move atomic.
3. Download the official binary (see [Where the binary comes from](#where-the-binary-comes-from))
   and refuse to continue unless it hashes to the SHA-256 pinned in
   `version.env`.
4. Run `ccios_patch.py` on it. The patcher exits non-zero if the binary links a
   dylib outside the four known-good ones or no longer imports a symbol the shim
   provides, and the install aborts.
5. Sign with `ldid -S entitlements.plist`.
6. **Run it.** With `libshim.dylib` copied alongside and the wrapper's
   environment set, execute `./claude.ios --version` and require the output to
   contain the expected version. If it does not run, the install aborts here,
   and the previously installed binary is untouched.
7. `mv -f` the verified binary over `/var/jb/usr/local/lib/claude-native/claude`
   and point `/var/jb/usr/local/bin/claude` at the wrapper. If a non-symlink
   `claude` was already there (for example the old 2.1.112 JavaScript install's
   wrapper), it is kept as `claude-legacy`; `prerm` hands `claude` back to it on
   removal.

No rollback copy of the previous binary is kept — the new one is verified before
the old one is touched, and a reinstall can always re-fetch. That saves ~200 MB.

Two consequences of this design: installing needs a network connection, and
this project never redistributes Anthropic's proprietary binary. You download it
from Anthropic, and your use of it is governed by Anthropic's terms. The
patcher, the shim and the packaging are the only things distributed here.

### Where the binary comes from

There are two independent official sources for the same artifact:

* **Anthropic's release CDN**:
  `https://downloads.claude.ai/claude-code-releases/<version>/darwin-arm64/claude`.
  `/latest` returns the current version string and
  `<version>/manifest.json` publishes an official SHA-256 per platform. This is
  the same channel the official `curl -fsSL https://claude.ai/install.sh | bash`
  installer uses.
* **The npm registry**: the `@anthropic-ai/claude-code-darwin-arm64` tarball,
  which contains the binary as `package/claude`.

Both were confirmed to serve byte-identical binaries — SHA-256
`ef5d2909c8af49f31ab6d5487e90316777bc2fac170adfe8160716caa8aaf4f9` for 2.1.263.

**At build time** (`tools/build-deb.sh`) the CDN manifest's checksum is
authoritative. The build downloads the npm tarball, extracts the binary, and
fails if it does not hash to what the manifest says: two independently published
channels having to agree is a stronger check than hashing one of them and
trusting the result. The manifest checksum and the tarball's own SHA-256 are
written into `version.env`.

**At install time** the postinst tries npm first — its tarball is ~87 MB against
~199 MB for the raw binary — checking the tarball against its pinned hash, and
falls back to the CDN if npm is unreachable or the tarball does not match.
Whichever source it ends up using, the extracted binary must hash to the pinned
checksum or the install aborts with
`Refusing to patch a binary that is not the one this package was built against.`
Both paths have been tested.

Two things deliberately not done:

* The manifest is PGP-signed (`manifest.json.sig`), but Anthropic does not
  publish the signing key over an authenticated channel that could be pinned, so
  verifying that signature would be theatre. The project does not pretend to.
* A `.zst` variant of the binary exists on the CDN (66 MB), but `zstd` is not
  on the device and is not in Procursus, so it is unused.

## Updating

There is no custom updater. **APT is the updater.**

`.github/workflows/publish.yml` runs every 6 hours (and on `workflow_dispatch`,
and on pushes to `main` that touch `packaging/`, `tools/` or the workflow):

1. **resolve** — ask the CDN what `/latest` is; fall back to the npm `latest`
   dist-tag if the CDN answer is unusable. Read `packaging/revision`.
2. **shim** — build `libshim.dylib` on a `macos-14` runner (the only step that
   needs the iPhoneOS SDK; the 200 MB Claude Code binary is never touched here).
3. **publish** — on Ubuntu: `build-deb.sh` fetches the real upstream binary,
   cross-checks the two channels, runs `ccios_patch.py --check`, and builds the
   12 KB package with version `<upstream>-<revision>`. `fetch-published.sh`
   pulls the packages already on the live repo forward and keeps the newest 10.
   `make-repo.py` regenerates `Packages{,.gz,.bz2,.xz}` and `Release`. The
   result is deployed to GitHub Pages.

Because the package version mirrors upstream, there is no state to track: Sileo
sees a new version exactly when Anthropic ships one, and offers the upgrade.
On upgrade the postinst does the same fetch-verify-patch-sign-run sequence as a
fresh install.

Claude Code's own auto-updater stays disabled (`DISABLE_AUTOUPDATER=1`) for the
reason given above: it would replace a working install with a macOS binary that
cannot load.

### Two gates

A bad upstream build cannot break a device, because two independent checks have
to pass and they catch different things:

* **Build time** — `ccios_patch.py --check` fails the build if upstream links a
  new dylib or dropped a shimmed symbol. Nothing publishes; the phone stays on
  the version it has. This catches changes to the binary's shape.
* **Install time** — the on-device smoke test runs the patched binary and aborts
  before touching the working copy if it fails. CI cannot execute iOS binaries,
  so this gate is doing real work: it catches anything that only shows up at
  runtime.

Both gates have already fired in practice.

### Manual upgrade, rollback, and when the build fails

To upgrade without waiting for Sileo:

```sh
sudo apt update && sudo apt install --only-upgrade com.andi.claude-code-native
```

To force a rebuild of the repo without a new upstream release, run the
**build and publish repo** workflow manually (optionally with a specific
upstream version), or bump `packaging/revision` and push.

The last 10 versions stay in the repo, so Sileo shows a version list. From a
shell:

```sh
sudo apt install --allow-downgrades com.andi.claude-code-native=2.1.263-1
```

(The `-1` is the package revision.) Verified working.

If the workflow starts failing, upstream changed something the port depends on.
Reproduce it locally:

```sh
python3 packaging/payload/ccios_patch.py <the-new-claude-binary> --check
```

The likely causes, in order: a newly linked dylib that may not exist on iOS; one
of the five shimmed symbols no longer imported (or a new one that needs
shimming); or the Mach-O header running out of slack for the added
`LC_LOAD_DYLIB` — see [Known limitations](#known-limitations). Fix `shim.c` /
`ccios_patch.py`, bump `packaging/revision`, push.

## Building it yourself

### Prerequisites

| step | needs |
|---|---|
| `tools/build-shim.sh` | macOS with Xcode command line tools and the iPhoneOS SDK (`xcrun --sdk iphoneos`); `ldid` to sign the shim (optional, warns if absent) |
| `tools/build-deb.sh` | `dpkg-deb` (`brew install dpkg` / `apt install dpkg-dev`), `python3`, `curl` |
| `tools/make-repo.py` | `python3`, `dpkg-deb` |

Building the shim is the only step that truly needs macOS; everything else runs
anywhere with `dpkg-deb`.

### Build

```sh
# 1. The shim: packaging/payload/libshim.dylib
./tools/build-shim.sh

# 2. The package: repo/debs/com.andi.claude-code-native_<version>-<revision>_iphoneos-arm64.deb
#    version defaults to the CDN's /latest, revision to packaging/revision
./tools/build-deb.sh                 # or: ./tools/build-deb.sh 2.1.263 1

# 3. APT metadata: repo/Packages{,.gz,.bz2,.xz} and repo/Release
python3 tools/make-repo.py repo
```

`build-deb.sh` downloads the upstream binary only to cross-check the two
channels, run `ccios_patch.py --check`, and record the checksums; it is not put
in the package. Set `GH_REPO` (`owner/name`) and `GH_PAGES`
(`owner.github.io/name`) to fill in the `Icon:` and `Depiction:` fields of the
control file; without them those fields are dropped. `OUT` overrides the output
directory.

To check whether a given upstream build is still patchable, or to patch one by
hand:

```sh
python3 packaging/payload/ccios_patch.py <binary> --check
python3 packaging/payload/ccios_patch.py <binary> <out> [--shim @executable_path/libshim.dylib]
```

The shim can also be built on the device with Procursus `clang` against
`/var/jb/usr/share/SDKs/iPhoneOS.sdk`. It is shipped prebuilt only to keep
`clang` off the package's dependency list.

### Testing a repo locally before publishing

This is exactly how the whole flow was verified. On the Mac, serve the repo:

```sh
cd repo && python3 -m http.server 8000
```

On the device:

```sh
echo 'deb [trusted=yes] http://<mac-ip>:8000/ ./' | sudo tee /var/jb/etc/apt/sources.list.d/ccforios-local.list
sudo apt update
sudo apt install com.andi.claude-code-native
claude --version
```

`[trusted=yes]` is needed because the repo is not GPG-signed.

### Publishing your own fork

1. Push the repo to GitHub.
2. Settings → Pages → Source: **GitHub Actions**.
3. Run the **build and publish repo** workflow, or wait for the 6-hour schedule.

The workflow discovers its own Pages URL, so nothing needs editing for a fork.
When rebuilding, run

```sh
tools/fetch-published.sh https://<owner>.github.io/CCForiOS/ [keep]
```

before `make-repo.py` to pull the already-published packages forward — otherwise
the repo only ever offers the newest build and there is nothing to roll back to.
`keep` defaults to 10. The workflow does this automatically.

To force a rebuild without waiting for a new upstream release — after a change
to `shim.c`, for instance — bump `packaging/revision`.

### Layout

```
packaging/DEBIAN/control.in          control template (@VERSION@, @REPO@, @PAGES@)
packaging/DEBIAN/postinst            fetch, verify, patch, sign, smoke-test, install
packaging/DEBIAN/prerm               remove the fetched binary, hand `claude` back
packaging/payload/shim.c             the shim
packaging/payload/ccios_patch.py     the Mach-O patcher
packaging/payload/claude-native      the wrapper
packaging/payload/entitlements.plist JIT entitlements for ldid
packaging/payload/version.env.in     filled in by build-deb.sh
packaging/revision                   package revision; bump to force a rebuild
packaging/depiction.html, index.html copied into the published repo
tools/build-shim.sh                  libshim.dylib (macOS + iPhoneOS SDK)
tools/build-deb.sh                   the .deb
tools/fetch-published.sh             carry published versions forward
tools/make-repo.py                   Packages + Release
repo/                                generated APT repo (deployed to Pages; gitignored)
.github/workflows/publish.yml        the 6-hourly build
```

## License

The code in this repository — the shim, the Mach-O patcher, the packaging and
the tooling — is MIT licensed. See [LICENSE](LICENSE).

That covers this project only. **Claude Code itself is Anthropic's software and
is not licensed by this repository.** Nothing here redistributes it: the package
is ~12 KB of patcher and downloads the official binary from Anthropic's own
release CDN or npm at install time, so your use of Claude Code is governed by
Anthropic's terms, exactly as it would be on any other platform.

## Known limitations

* **`BUN_JSC_useConcurrentJIT=0` is load-bearing.** The wrapper sets it. Without
  it you get `panic: Bus error` as soon as a background compiler thread flips the
  JIT pool while the main thread executes from it.
* The same W^X race is theoretically reachable from Bun `worker_threads`: any
  second JS thread compiling while another executes. Nothing in testing hit it,
  but it is the known weak point of emulating a per-thread API with a
  process-wide `mprotect`.
* Only 8 bytes of Mach-O header slack remain after the added `LC_LOAD_DYLIB`
  (the command needs 56). If a future upstream build adds load commands,
  `ccios_patch.py` fails loudly (`no header room for LC_LOAD_DYLIB`) rather than
  corrupting anything, but fixing it would need real work.
* macOS-only frameworks (AppKit, Carbon, ScreenCaptureKit, …) do not exist on
  iOS, path rewrite or not. Anything depending on them — screenshots, webview —
  will not work.
* `mprotect` on a 64 MB region at every W^X flip is slower than the hardware
  toggle it replaces. In practice startup is ~500 ms and a simple prompt
  round-trips in under 3 s, so it is not a practical problem.
* Install needs ~700 MB free transiently and ~200 MB afterwards, plus a network
  connection.
* Depends on `python3`, `ldid`, `curl`, `tar`. `/var/jb/usr/local/bin` is on the
  PATH of a login shell only; over SSH use `zsh -l -c` or the full path.
* Requires a rootless jailbreak (`/var/jb`). The package declares
  `firmware (>= 15.0)` to match the patched `minos`, but it has only been tested
  on Dopamine / iOS 17.3 / arm64.
