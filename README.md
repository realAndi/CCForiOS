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
| Sileo source | `https://reallyitsandi.com/repo/` |
| Package | `com.andi.claude-code-native` ("Claude Code (native)") |
| Source | https://github.com/realAndi/CCForiOS |
| Verified on | iPhone 15 Pro (A17 Pro), iOS 17.3 (21D50), Dopamine rootless (`/var/jb`, Procursus) |

## Installing

Sileo → Sources → **+** → `https://reallyitsandi.com/repo/`, then install
**Claude Code (native)**. Then sign in — once:

```sh
claude-login
claude
```

Use `claude-login`, **not** `claude auth login`; the latter cannot save
credentials on iOS. See [Signing in](#signing-in) for why.

Requirements:

* a rootless jailbreak (`/var/jb`); tested on Dopamine: iOS 17.3 on an iPhone 15
  Pro, and iPadOS 15.6 on an iPad Pro (A12X)
* an A12 chip or newer. **A12-class devices** (iPhone XS/XR, the 2018 iPad Pro,
  iPad Air 3, iPad mini 5, iPad 8th gen) **are experimental**: the official
  build is compiled for M1-class CPUs and uses a few instructions an A12 lacks
  (LDAPUR/STLUR, SHA3, SHA512, dot product), which the shim emulates — see
  section 3 of `shim.c`. Older chips lack far more, and the install stops and
  says so.
* a network connection during install — the package does not contain Claude
  Code, it fetches it (see [What the package contains](#what-the-package-contains))
* about 700 MB free during install, about 200 MB afterwards
* `zsh`, `python3`, `ldid`, `curl`, `tar`, `coreutils` — declared as
  dependencies, so Sileo offers to install anything missing before it installs
  this. **Node is not needed**: the Claude Code binary is Bun-compiled and
  self-contained, and `claude-login` uses only the Python standard library.
  The `postinst` also checks each tool by name before doing any work, so a
  hand-installed `.deb` fails immediately with what is missing rather than
  part-way through a 200 MB install.

Install takes 5–7 seconds: download, checksum, patch, sign, smoke-test, move
into place.

Sileo and Zebra do not check repository signatures, so that is all they need.
The `apt` CLI does check. The repo is signed, so install the key once and name
it in the source line:

```sh
sudo mkdir -p /var/jb/usr/share/keyrings
curl -fsSL https://reallyitsandi.com/repo/andi.gpg \
  | sudo tee /var/jb/usr/share/keyrings/andi.gpg >/dev/null

echo 'deb [signed-by=/var/jb/usr/share/keyrings/andi.gpg] https://reallyitsandi.com/repo/ ./' \
  | sudo tee /var/jb/etc/apt/sources.list.d/andi.list
sudo apt update && sudo apt install com.andi.claude-code-native
```

The key goes in `usr/share/keyrings`, not `/var/jb/etc/apt/trusted.gpg.d/`: a
key in the global store can validate *any* repository apt sees, whereas
`signed-by=` limits it to this one.

Then run `claude`. `/var/jb/usr/local/bin` is on the PATH of a *login* shell,
which is what NewTerm gives you, but not of a non-interactive `ssh host 'cmd'`.
Over SSH use:

```sh
ssh phone 'zsh -l -c "claude -p \"reply with the single word OK\""'
```

### What works

`claude --version`, the full `--help`, `claude doctor` (reports
`Running: native (<version>)`, `Platform: darwin-arm64`,
`Search: OK (bundled)`),
interactive and `-p` sessions against the API with TLS, retry/backoff, MCP
registry, LSP manager and telemetry all starting and shutting down cleanly.
Tools exercised end to end: Bash (subprocess spawn), Grep, Read, Glob. Search is
served by Claude Code's own implementation; no ripgrep is needed. Three
consecutive multi-tool sessions ran with no panic and no bus error.

Startup is about 500 ms. A simple prompt round-trips in under 3 s (2.75 s
measured).

## Signing in

```sh
claude-login
```

A URL appears — open it in Safari, approve, paste the code back. That is the
whole flow, and it gets the **full scope set**, so Remote Control, MCP servers
and file upload all work.

```sh
claude-login --status     # what is stored, and when it expires
claude-login --refresh    # force a refresh now
claude-login --logout     # delete the stored credential
```

**Do not use `claude auth login` on iOS.** It completes the browser flow and
then silently discards the result, so the next run is signed out again. Claude
Code stores credentials through `Bun.secrets` — Bun's own store, compiled into
the binary — which cannot write on iOS.

Things that look like the cause and are not, each ruled out by measurement:

| suspected | test | result |
|---|---|---|
| missing `/usr/bin/security` CLI | PATH shim, on `logout` and on a session | never invoked |
| missing keychain entitlements | probe with the binary's exact signature | `SecItemAdd -> 0`, reads back |
| broken `Security.framework` path | `dlopen` the patched path | OK, all `SecItem*` resolve |
| the OAuth exchange failing | account lands in `~/.claude.json` | sign-in itself succeeds |

But Claude Code *reads* `~/.claude/.credentials.json` perfectly well — that is
why a credential copied from a desktop works, right up until its access token
expires ~8 hours later and cannot be refreshed in place. So `claude-login` runs
the OAuth flow itself, with the same scopes `claude auth login` requests, and
writes that file; the wrapper refreshes the access token before each run.
Everything Claude Code needs, nothing it has to write.

`claude setup-token` also works and needs no browser gymnastics, but its token
is **capped at `user:inference` by design** — Anthropic limits long-lived tokens
for security — which costs Remote Control. The wrapper still honours one at
`~/.claude/oauth-token` for headless use, and an exported
`CLAUDE_CODE_OAUTH_TOKEN` always wins, but a stored credential takes precedence.

### Where the credential is stored

Claude Code treats `~/.claude/.credentials.json` as a **legacy** path: once it
reads a valid credential from there it migrates it into `Bun.secrets` and
deletes the original. On iOS the write half of that silently fails and the
delete half succeeds, so a credential written straight to that file survives
exactly one run — which is what made a copied desktop credential look like it
"expired". An invalid credential is left alone, because the migration never gets
that far, which is why the effect looks intermittent.

So `claude-login` keeps its own master copy, and the wrapper re-creates
`.credentials.json` from it before every launch; Claude Code can delete its copy
as often as it likes. The master lives in the **iOS keychain**, through the
small `ccauth-keychain` helper installed next to the binary: a generic-password
item, `ThisDeviceOnly`, authorized by the same entitlements the binary carries.
Two properties fall out of that, both of which the old plain-text file lacked:

* **The long-lived refresh token never touches the file system.** The mirror
  Claude Code reads carries the ~8 hour access token only, and Claude Code
  deletes even that shortly after launch. A session that outlives its token is
  fixed by starting `claude` again — mid-session refresh never worked on iOS
  anyway, which is the reason this module exists.
* **The credential's home is no longer the jailbreak root.** Keychain items
  live on the data volume (`/var/Keychains`), outside `/var/jb` and the preboot
  volume. An iOS update or a jailbreak reinstall rebuilds `/var/jb` from
  scratch — the package has to be reinstalled either way, but the sign-in
  survives, because the keychain does. "Erase All Content and Settings"
  destroys the keybag, so a wiped device takes the credential with it; see
  [the security notes](#what-the-keychain-does-and-does-not-protect-against).

A master stored by an older version of this package (the plain-text
`ccauth.json`) is imported into the keychain on first use and the plain-text
original is deleted on the spot. If the keychain misbehaves on a given device,
`CCAUTH_PLAINTEXT=1` restores the old behaviour: master in
`~/.claude/ccauth.json`, mode `0600`, no keychain involved.

### What the keychain does and does not protect against

`claude-login` requests a full-scope OAuth credential — including
`org:create_api_key` — so anyone who can read it can act as the signed-in
account within those scopes. The keychain narrows who that can be, and it is
worth being precise about where the edges are:

| scenario | plain-text file (the old way) | keychain (now) |
|---|---|---|
| "Erase All Content and Settings", then the phone is sold | **survives** — erase crypto-shreds the data volume, but the preboot copy is only rewritten by a full restore | gone — the item is wrapped in the device keybag, which erase destroys |
| stolen device, offline image of the flash | readable bytes | requires the device's passcode-bound class keys |
| backups / restored onto another device | not backed up (preboot is outside the backup roots) | `ThisDeviceOnly` — excluded by construction |
| another process on a stock iOS device | anything running as the same user | blocked without the package's keychain access group |
| a process with root on the jailbroken device | readable | **readable** — see below |

That last row is the honest limit, and no storage scheme changes it: a
jailbroken device has no security boundary against root, and the entitlement
that authorizes keychain access is ldid-fake-signable by anything that can
already run code on the device. The keychain is not protecting the token from
the phone's owner-with-root; it is protecting it from everyone the phone might
belong to after it leaves your hands — the buyer, the thief, the offline image.
Against the live-root threat the mitigations are behavioural, not technical:

* Treat the credential like a pasted API key. On a device you do not fully
  control, prefer `claude setup-token` (capped at `user:inference`) or an
  `ANTHROPIC_API_KEY` over a full `claude-login`.
* `claude-login --logout` deletes the keychain item and every file copy; for
  certainty, also revoke the session in your Claude account settings — that
  works no matter what copies exist.

### Refresh-token rotation

The token endpoint issues a new refresh token on every refresh and invalidates
the old one, so losing the replacement means being signed out. The write path is
built for that: an exclusive lock so two concurrent starts cannot both refresh,
a keychain write that updates the item in place rather than delete-then-add so a
crash cannot lose the new token, and no destructive step on failure — a refresh
that fails leaves the credential exactly as it was. Unknown fields in the
credential are preserved rather than dropped, since the format is Anthropic's
and not ours.

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

The real `pthread_jit_write_protect_np` is per thread: the thread writing code
sees the pool read-write while every other thread keeps executing it. iOS can
only change protection for the whole process. So the shim tracks each 16 KB
page of JSC's 64 MB pool as RW or RX and lets faults move them:

| event | what the shim does |
|---|---|
| `mmap` with `MAP_JIT` | strips the flag, maps the pool RW instead of RWX, records every page as RW |
| `pthread_jit_write_protect_np(0)` | marks this thread as writing; changes no protection |
| write fault by a writing thread | makes that one page RW and counts the thread as its writer |
| execute fault | waits until no thread is still writing the page, makes it RX |
| `pthread_jit_write_protect_np(1)` | drops this thread's writer counts; pages stay as they are |

After a repair the faulting instruction runs again. The faults arrive as
`SIGBUS` or `SIGSEGV`, which Bun also claims for its crash handler, so the shim
installs its handler before `main` and interposes `sigaction` and `signal` for
those two signals: Bun's handler is recorded and chained to for every other
fault, and a real crash still prints Bun's `panic`. `mprotect` is interposed so
a protection change made by anything else to a pool page updates the record.

The first version of the shim `mprotect`ed the whole pool on every call, which
is process-wide: any second thread that writes code pulled the pool out from
under the main thread. Claude Code's bundle contains worker code. Measured on an
iPhone 15 Pro, iOS 17.3, running test code inside each build's own runtime:

| test | 2.1.273, old flip | 2.1.274, page faults |
|---|---|---|
| one busy worker, busy main thread | 0 of 3 survived | 15 of 15 |
| four workers with polymorphic code | 0 of 3 | 15 of 15 |

Five runs in each page-fault row had JSC's concurrent JIT turned back on.
Speed is unchanged within noise: JSON round trips 148–164 ms against 150–151 ms,
and property access on mixed shapes 64–78 ms against 77–85 ms.

The real `mmap`, `mprotect` and `sigaction` are resolved with `dlsym` on an
explicit libSystem handle, not `RTLD_DEFAULT`: the default search walks the
global list and finds the shim's own definitions first. A guard aborts with
`[ccios] fatal: cannot resolve libSystem's ...` rather than recursing, should
that ever happen.

**The two other missing symbols.**

* `__clear_cache` → `sys_icache_invalidate`, iOS's spelling of the same operation
  (the JIT calls it after emitting code).
* `posix_spawn_file_actions_addfchdir` → `posix_spawn_file_actions_addfchdir_np`.
  The un-suffixed spelling postdates iOS 17; the `_np` one is present on 17.3.

With the shim linked, `BUN_JSC_dumpOptions=2` reports `useJIT=true` and
`useSharedArrayBuffer=true`.

### The concurrent JIT setting

The wrapper sets:

```
BUN_JSC_useConcurrentJIT=0
```

With the old whole-pool flip this was load-bearing: JSC's background compiler
thread flipped the pool to RW while the main thread executed from it. The
page-fault emulation handles any number of writing threads, and the worker tests
pass with concurrent compilation on as well. The setting stays until a long
interactive session has run with it off, because a compiler thread writes far
more often than a worker does. Baseline, DFG and FTL all stay enabled either
way; only the concurrency is off.

### Runtime environment

`/var/jb/usr/local/bin/claude-native` (which `claude` symlinks to) sets:

| variable | why |
|---|---|
| `BUN_JSC_useConcurrentJIT=0` | see above |
| `DISABLE_AUTOUPDATER=1` | Claude Code's own updater would fetch the unpatched macOS binary and replace a working install with one that cannot load. No iOS build exists upstream. |
| `HOME` | defaulted to `/var/jb/var/mobile` if unset |

and `exec`s `/var/jb/usr/local/lib/claude-native/claude`. `libshim.dylib` must
sit beside the binary — it is loaded as `@executable_path/libshim.dylib`.

Set `CCIOS_SHIM_DEBUG=1` to have the shim print the JIT pool mapping at
startup (`[ccios] JIT pool 0x... len=67141632, 4098 pages of 16384 bytes`) and
the fault counts at exit.

## What the package contains

**Not Claude Code.** The `.deb` is about 70 KB and ships only:

| file | installed to | |
|---|---|---|
| `libshim.dylib` | `/var/jb/usr/local/lib/claude-native/` | the 68 KB shim, prebuilt against the iPhoneOS SDK |
| `ccauth-keychain` | `/var/jb/usr/local/lib/claude-native/` | the keychain helper: keeps the master credential as a this-device-only `SecItem`, signed with the same entitlements |
| `ccauth-keychain.c` | `/var/jb/usr/local/lib/claude-native/` | helper source, so it can be rebuilt on device |
| `ccauth.py` | `/var/jb/usr/local/lib/claude-native/` | the OAuth flow, keychain storage, credential refresh |
| `ccios_patch.py` | `/var/jb/usr/local/lib/claude-native/` | the Mach-O patcher |
| `shim.c` | `/var/jb/usr/local/lib/claude-native/` | shim source, so it can be rebuilt on device |
| `entitlements.plist` | `/var/jb/usr/local/lib/claude-native/` | JIT entitlements (`dynamic-codesigning`, `com.apple.security.cs.allow-jit`, `get-task-allow`, ...) and the keychain access group, for `ldid` |
| `version.env` | `/var/jb/usr/local/lib/claude-native/` | upstream version, pinned SHA-256s, download URLs |
| `claude-native` | `/var/jb/usr/local/bin/` | the wrapper |
| `claude-login` | `/var/jb/usr/local/bin/` | the sign-in command |

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
8. Probe the keychain via `ccauth-keychain probe` — store, read back and delete
   a throwaway item — and report whether credentials will be stored there.
   Advisory only: a broken keychain does not stop the install, because
   `CCAUTH_PLAINTEXT=1` keeps the credential in a `0600` file instead. An
   unsigned helper reports `-34018` here, which is the signature step having
   been skipped, not a broken device.

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
`ef5d2909c8af49f31ab6d5487e90316777bc2fac170adfe8160716caa8aaf4f9` for 2.1.263,
which was current when this was written.

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

### Where the packages come from

Two repositories, and it is worth knowing which is which:

| | |
|---|---|
| `https://reallyitsandi.com/repo/` | the one to add — signed, and carries other packages too |
| `https://realandi.github.io/CCForiOS/` | where this repository's CI publishes its build |

The domain syncs from the Pages repo on its own six-hour schedule, verifying
each package against the checksum the upstream index declares, and keeps the
newest few for rollback. It only ever adds or replaces, so if Pages is
unreachable the domain keeps serving what it already has.

Adding the Pages URL directly also works and gets the same packages a little
sooner; it just needs its own key (`ccforios.gpg`).

`.github/workflows/publish.yml` runs every 6 hours (and on `workflow_dispatch`,
and on pushes to `main` that touch `packaging/`, `tools/` or the workflow):

The orchestration is the reusable workflow in
[`realAndi/ios-port-ci`](https://github.com/realAndi/ios-port-ci), shared with
the other iOS ports and pinned at `@v1`; this repository supplies only the three
scripts in `tools/` and everything in `packaging/`. Its jobs:

1. **resolve** — `tools/resolve-version.sh` asks the CDN what `/latest` is,
   falling back to the npm `latest` dist-tag if that answer is unusable. Given a
   `workflow_dispatch` version it validates it instead, and fails immediately if
   Anthropic publishes no manifest for it. Reads `packaging/revision`.
2. **payload** — `tools/build-payload.sh` builds and signs `libshim.dylib` on a
   macOS runner (the only step that needs the iPhoneOS SDK; the 200 MB Claude
   Code binary is never touched here) and asserts the result is signed, because
   iOS will not load a dylib without a cdhash.
3. **publish** — on Ubuntu: `tools/build-deb.sh` fetches the real upstream
   binary, cross-checks the two channels, runs `ccios_patch.py --check`, and
   builds the 12 KB package with version `<upstream>-<revision>`. The shared
   `fetch-published.sh` pulls the packages already on the live repo forward and
   keeps the newest 10, `make-repo.py` regenerates `Packages{,.gz,.bz2,.xz}` and
   `Release` and signs them, and the result is deployed to GitHub Pages.

The schedule polls every 30 minutes, and the shared `resolve` job skips a
scheduled run whose version-revision is already published — so a poll that finds
nothing new costs a few seconds on one Ubuntu job and deploys nothing, while a
real release is picked up within half an hour instead of up to six. A push or a
manual run always builds, since the first changed the packaging and the second
is someone asking for a rebuild.

The package states which Claude Code it wraps in three places: its `Description`,
which is what Sileo shows, the depiction's "At a glance" table — `@VERSION@`
there is substituted at publish time by the shared workflow — and a Debian
`changelog` at
`/var/jb/usr/share/doc/com.andi.claude-code-native/changelog`. Both are
generated from the version being built. The changelog is dated from Anthropic's
own `buildDate` in the manifest rather than from the clock, so rebuilding a
version produces the same bytes — a timestamp there would change the package on
every run and trip the already-published guard forever.

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
sudo apt install --allow-downgrades com.andi.claude-code-native=<version>-<revision>
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

### Signing

The published `Release` is signed into `InRelease` and `Release.gpg`, and the
public key is served at
[`ccforios.gpg`](https://realandi.github.io/CCForiOS/ccforios.gpg). The shared
workflow would name it `key.gpg` by default; the caller passes
`key-file: ccforios.gpg` so the URL this README links keeps working and nobody's
`signed-by=` breaks.

The signing happens in the Action, so the private key lives in the
`CCIOS_GPG_KEY` repository secret (`CCIOS_GPG_KEY_ID` names it). That is
unavoidable for a repository that rebuilds unattended, and it is why this key is
**not** the one that signs reallyitsandi.com. That key is a different key held
in a different repository's Actions secrets (`Portfolio-Site`), so a compromise
of this repository's CI cannot forge packages there. Each port signs with its
own key for the same reason. Rotating this
one means generating a new pair, replacing both secrets, and republishing;
anyone who installed the old key has to fetch the new one.

If the secret is missing the workflow does not fail — it publishes unsigned and
emits a warning, since an unsigned repository still works for Sileo and Zebra.

### `claude doctor` warnings

`doctor` reports five warnings on a healthy install. Four come from it expecting
the official native installer's layout — `~/.local/bin/claude` on `PATH`, and an
`installMethod` recorded in config — which a dpkg install does not use. The
fifth is the keychain probe described above. All are cosmetic.

**Do not run `claude install`**, which those warnings suggest as the fix. It
would replace the patched binary with an unpatched macOS one that cannot load on
iOS. If it ever happens, reinstall the package: the postinst re-fetches and
re-patches from scratch.

They are left alone deliberately. Recording an `installMethod` would invite
Claude Code to manage — and overwrite — the binary it thinks it installed, and
five lines of noise are a better trade than that.

## Building it yourself

### Prerequisites

| step | needs |
|---|---|
| `tools/resolve-version.sh` | `curl`, `python3` |
| `tools/build-payload.sh` | macOS with Xcode command line tools and the iPhoneOS SDK (`xcrun --sdk iphoneos`), and `ldid` — both checked, neither installed |
| `tools/build-shim.sh` | the same, but `ldid` is optional (it warns rather than failing, so it stays runnable on the device); `build-payload.sh` is what turns that into a hard error |
| `tools/build-deb.sh` | `dpkg-deb` (`brew install dpkg` / `apt install dpkg-dev`), `python3`, `curl` |
| `.ci/tools/make-repo.py` | `python3`, `dpkg-deb`, `gpg` to sign |

Building the shim is the only step that truly needs macOS; everything else runs
anywhere with `dpkg-deb`.

### Build

```sh
# 1. The payload: packaging/payload/{libshim.dylib,PAYLOAD.version}
#    Takes the version; tools/resolve-version.sh picks one if you have none.
./tools/build-payload.sh "$(./tools/resolve-version.sh)"

# 2. The package: repo/debs/com.andi.claude-code-native_<version>-<revision>_iphoneos-arm64.deb
#    The version comes from PAYLOAD.version; the argument is the revision,
#    which defaults to packaging/revision.
./tools/build-deb.sh                 # or: ./tools/build-deb.sh 1

# 3. APT metadata: repo/Packages{,.gz,.bz2,.xz} and repo/Release.
#    make-repo.py is shared; tools/ci.sh clones ios-port-ci@v1 into .ci/.
python3 "$(tools/ci.sh)/make-repo.py" repo
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

`[trusted=yes]` here because a local test build is unsigned — the signing key
lives in CI, not on your machine. The published repo is signed and does not need
it; see [Installing](#installing).

### Publishing your own fork

1. Push the repo to GitHub.
2. Settings → Pages → Source: **GitHub Actions**.
3. Optional, to sign it — generate a key and add it as two secrets:

   ```sh
   gpg --quick-generate-key "Your Repo <you@example.com>" rsa4096 sign never
   FPR=$(gpg --fingerprint --with-colons "Your Repo" | awk -F: '/^fpr:/{print $10; exit}')
   gpg --export-secret-keys --armor "$FPR" | gh secret set CCIOS_GPG_KEY --repo <owner>/CCForiOS
   gh secret set CCIOS_GPG_KEY_ID --repo <owner>/CCForiOS --body "$FPR"
   ```

   Skip it and the repo publishes unsigned with a warning, which Sileo and Zebra
   accept and the `apt` CLI takes with `[trusted=yes]`.
4. Run the **build and publish repo** workflow, or wait for the 6-hour schedule.

The workflow discovers its own Pages URL, so nothing needs editing for a fork.
When rebuilding, run

```sh
"$(tools/ci.sh)/fetch-published.sh" https://<owner>.github.io/CCForiOS/ repo/debs [keep]
```

before `make-repo.py` to pull the already-published packages forward — otherwise
the repo only ever offers the newest build and there is nothing to roll back to.
`keep` defaults to 10. The workflow does this automatically.

To force a rebuild without waiting for a new upstream release — after a change
to `shim.c`, for instance — set `packaging/revision` to `<upstream version> <n>`,
for example `2.1.267 1`. The package version is then `2.1.267-1`; the plain
`2.1.267` it would otherwise be means "this is exactly what Anthropic shipped,
with nothing changed here". The suffix disappears by itself when upstream moves
past the recorded version, so it never has to be reset by hand.

One caveat when picking `<n>`: it has to beat any revision already published for
that same upstream version, because apt compares them numerically. The old
scheme's counter was global, so versions carrying a high revision may already
exist — `2.1.267-16` did — and starting again at `1` would publish something apt
considers older than what is already there.

### Layout

```
packaging/DEBIAN/control.in          control template (@VERSION@, @REPO@, @PAGES@)
packaging/DEBIAN/postinst            fetch, verify, patch, sign, smoke-test, install
packaging/DEBIAN/prerm               remove the fetched binary, hand `claude` back
packaging/payload/shim.c             the shim
packaging/payload/ccauth-keychain.c  the keychain helper (built by build-payload.sh)
packaging/payload/ccios_patch.py     the Mach-O patcher
packaging/payload/ccauth.py          the OAuth flow, keychain storage, refresh
packaging/payload/claude-native      the wrapper
packaging/payload/claude-login       sign-in, run by the user
packaging/payload/entitlements.plist JIT entitlements for ldid
packaging/payload/version.env.in     filled in by build-deb.sh
packaging/revision                   package revision; bump to force a rebuild
packaging/depiction.html, index.html copied into the published repo
tools/resolve-version.sh             which upstream version to build
tools/build-payload.sh               the payload (macOS); calls build-shim.sh
tools/build-shim.sh                  libshim.dylib (macOS + iPhoneOS SDK)
tools/build-deb.sh                   the .deb
tools/ci.sh                          clones ios-port-ci@v1 into .ci/ for local use
repo/                                generated APT repo (deployed to Pages; gitignored)
.github/workflows/publish.yml        calls realAndi/ios-port-ci@v1, 6-hourly
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

* Only 8 bytes of Mach-O header slack remain after the added `LC_LOAD_DYLIB`
  (the command needs 56). If a future upstream build adds load commands,
  `ccios_patch.py` fails loudly (`no header room for LC_LOAD_DYLIB`) rather than
  corrupting anything, but fixing it would need real work.
* macOS-only frameworks (AppKit, Carbon, ScreenCaptureKit, …) do not exist on
  iOS, path rewrite or not. Anything depending on them — screenshots, webview —
  will not work.
* A JIT write or first execution of a page costs a fault where macOS has a
  hardware toggle. Pages that are only executed stop faulting after their first
  run, and in practice startup is ~500 ms and a simple prompt round-trips in
  under 3 s.
* Install needs ~700 MB free transiently and ~200 MB afterwards, plus a network
  connection.
* Depends on `python3`, `ldid`, `curl`, `tar`. `/var/jb/usr/local/bin` is on the
  PATH of a login shell only; over SSH use `zsh -l -c` or the full path.
* Requires a rootless jailbreak (`/var/jb`). The package declares
  `firmware (>= 15.0)` to match the patched `minos`, but it has only been tested
  on Dopamine / iOS 17.3 / arm64.
