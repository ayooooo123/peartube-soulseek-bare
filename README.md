# peartube-soulseek-bare

Native **Bare addon** (`BARE_MODULE`) exposing [`peartube-soulseek`](https://github.com/ayooooo123/peartube-soulseek) in-process for PearTube's distroless `bare` relay and Android worklet.

PearTube's relay runs `bare` as PID 1 inside a distroless image with no `/bin/sh` and cannot exec a 295 MB `.NET` daemon (`slskd`), nor can the Android worklet spawn a bundled binary. This repository owns **only** the C ABI Bare shim (`soulseek/addon.c`), CMake build, `js/index.js` wrapper, and Bare addon tests. All Soulseek protocol, session, transfer, and C ABI implementation code lives in [`peartube-soulseek`](https://github.com/ayooooo123/peartube-soulseek).

## Crate & Dependency Decision

- **Linkage route chosen:** **Pinned Cargo git dependency (`rev = "1cee3df7f8c41ea7dbeb755ef8a0e03f4e5c9827"`)** on `https://github.com/ayooooo123/peartube-soulseek.git` (MIT license).
- **Why:**
  1. Zero code duplication between repos: `src/lib.rs` is a single line (`pub use peartube_soulseek::ffi::*;`) compiled as `crate-type = ["staticlib"]`, and `build.rs` reads `DEP_PEARTUBE_SOULSEEK_INCLUDE` from `peartube-soulseek`'s `links = "peartube_soulseek"` metadata to stage `soulseek.h` into `CARGO_TARGET_DIR/include/soulseek.h` for `soulseek/addon.c`.
  2. Pinning by exact git commit SHA (`rev`) + committed `Cargo.lock` guarantees reproducible offline builds after `cargo fetch` and lets `cargo build --release --target <triple>` cross-compile the entire pure-Rust stack (`md-5 = "=0.10.6"`, `flate2 = "=1.1.2"` `rust_backend`, `parking_lot = "=0.12.5"`) in one step without CMake needing separate archive download rules.
- **Upstream `soulseek-rs` evaluation:** `soulseek-rs` `20.0.0` (MIT) was evaluated and not used in `peartube-soulseek` because it requires Rust 1.91, hardcodes reserved client major `176`, and omits `queue_length` when parsing `FileSearchResponse`. Full rationale and protocol message coverage tables live in [`peartube-soulseek/README.md`](https://github.com/ayooooo123/peartube-soulseek#readme) and [`peartube-soulseek/NOTES.md`](https://github.com/ayooooo123/peartube-soulseek/blob/main/NOTES.md).

## Account & Credential Policy (Mandatory)

- **Accounts do NOT auto-register.** Generating random usernames violates official Soulseek server rules (`https://www.slsknet.org/news/node/681`) and gets IPs/accounts banned.
- **The operator MUST supply an existing Soulseek username and password.** `login()` validates non-empty credentials and fails loudly with the server's exact rejection reason (`INVALIDUSERNAME`, `EMPTYPASSWORD`, `INVALIDPASS`, `INVALIDVERSION`, `SVRFULL`, `SVRPRIVATE`).
- **There is no password reset on Soulseek.** Never discard or overwrite stored operator credentials.
- Passwords are never logged at any log level and are zeroed out of C shim buffers immediately after marshaling; peer paths are redacted to basenames in `info`/`warn`/`error` logs.

## Architecture

A Rust `cdylib` cannot be loaded directly by Bare because Bare resolves native addons via the `BARE_MODULE` symbol registration macros in `<bare.h>` / `<js.h>`.

```
soulseek/
  addon.c         #include <bare.h> / <js.h> / <uv.h> + "soulseek.h" (from peartube-soulseek)
                  BARE_MODULE(soulseek, soulseek_exports)
                  uv_async_t + uv_mutex_t completion queue -> zero JS loop blocking
src/
  lib.rs          1-line staticlib re-export: `pub use peartube_soulseek::ffi::*;`
build.rs          Copies `soulseek.h` from `DEP_PEARTUBE_SOULSEEK_INCLUDE` for CMake
CMakeLists.txt    cmake-bare build linking `libpeartube_soulseek_bare_ffi.a` -> `.bare`
scripts/
  build.sh        Cross-compile driver staging `prebuilds/<platform>-<arch>/peartube-soulseek-bare.bare`
binding.js        `module.exports = require.addon()`
js/
  index.js        `Soulseek` class (`login`, `search`, `browse`, `download`, `state`, `setLogger`, `close`)
test/
  addon.test.js   End-to-end Bare runtime test against mock Soulseek server & peer sockets
```

## Platform Support & Prebuild Status

`prebuilds/` is **gitignored by design** (`git ls-files | grep prebuilds` is empty) so binary `.bare` shared objects never bloat git history. Instead, GitHub Actions (`.github/workflows/ci.yml`) builds and verifies every target on push and publishes `.bare` artifacts (and GitHub Release assets on `v*` tags).

| Target | Status | `file prebuilds/<target>/peartube-soulseek-bare.bare` | How Verified / Load-Tested |
|---|---|---|---|
| `darwin-arm64` | `built+tested` | `Mach-O 64-bit dynamically linked shared library arm64` | `./node_modules/.bin/bare -e "console.log(Object.keys(require('./binding.js')).join(','))"` + `bare test/addon.test.js` on macOS arm64 |
| `linux-x64` | `built+tested` | `ELF 64-bit LSB shared object, x86-64, version 1 (SYSV), dynamically linked` | Built against Debian 12 glibc 2.36; load-tested (`bare -e` exports + `test/addon.test.js`) inside `gcr.io/distroless/cc-debian12` via `docker run --platform linux/amd64` |
| `linux-arm64` | `built+tested` | `ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV), dynamically linked` | Built against Debian 12 glibc 2.36; load-tested (`bare -e` exports + `test/addon.test.js`) inside `gcr.io/distroless/cc-debian12` via `docker run --platform linux/arm64` |
| `android-arm64` | `built+tested` | `ELF 64-bit LSB shared object, ARM aarch64, version 1 (SYSV), dynamically linked` | Built with Android NDK r29 (`aarch64-linux-android29-clang`, 16 KiB `PT_LOAD` page alignment); load-tested (`bare -e` exports + `test/addon.test.js`) on physical Pixel 5a (`arm64-v8a`) under `bare-runtime-android-arm64` |
| `darwin-x64` | `recipe only` | `Mach-O 64-bit dynamically linked shared library x86_64` | Supported by `sh scripts/build.sh darwin-x64` on macOS with `rustup target add x86_64-apple-darwin` |

## Getting Prebuilds (Release Artifact vs Local Build)

### Option A — Download CI / GitHub Release Artifacts (No Rust or NDK Needed)

- **From a tagged GitHub Release (`v*`):**
  ```sh
  gh release download --repo ayooooo123/peartube-soulseek-bare \
    --pattern 'peartube-soulseek-bare-prebuilds.tar.gz' \
    --pattern 'SHA256SUMS.txt'
  shasum -a 256 -c SHA256SUMS.txt --ignore-missing
  tar -xzf peartube-soulseek-bare-prebuilds.tar.gz
  ```
  Extracting `peartube-soulseek-bare-prebuilds.tar.gz` populates `prebuilds/{darwin-arm64,linux-x64,linux-arm64,android-arm64}/peartube-soulseek-bare.bare` directly at the repo root where `require.addon()` resolves it. Individual `peartube-soulseek-bare-<target>.bare` files are also attached to each release.

- **From the latest `main` CI run (clean clone without a tag):**
  ```sh
  run_id=$(gh run list --repo ayooooo123/peartube-soulseek-bare --branch main --workflow CI --status success --limit 1 --json databaseId --jq '.[0].databaseId')
  gh run download "$run_id" --repo ayooooo123/peartube-soulseek-bare --name prebuilds --dir .
  ```

### Option B — Build Locally (`scripts/build.sh`)

`cmake-bare` is not hoisted in PearTube's root tree; install devDependencies first in a clean clone (`scripts/build.sh` runs `npm ci` automatically if `node_modules/cmake-bare` is missing):

```sh
npm ci
sh scripts/build.sh darwin-arm64
sh scripts/build.sh linux-x64
sh scripts/build.sh linux-arm64
sh scripts/build.sh android-arm64
# Or build all four PearTube targets in one pass:
sh scripts/build.sh all
```

### Exact Per-Target Cross-Compile Recipes & Load Probes

`scripts/build.sh <target>` runs `cargo build --release --target <triple>` with matching `CARGO_TARGET_<TRIPLE>_LINKER` / `CC_<triple>` env vars and `--remap-path-prefix` flags, then drives `cmake` (`-DSKIP_CARGO_BUILD=ON` + matching `-DCMAKE_C_COMPILER`) to link `soulseek/addon.c` against `libpeartube_soulseek_bare_ffi.a` and stage `prebuilds/<platform>-<arch>/peartube-soulseek-bare.bare`.

1. **`darwin-arm64` (macOS Apple Silicon):**
   ```sh
   sh scripts/build.sh darwin-arm64
   file prebuilds/darwin-arm64/peartube-soulseek-bare.bare
   ./node_modules/.bin/bare -e "console.log(Object.keys(require('./binding.js')).join(','))"
   ./node_modules/.bin/bare test/addon.test.js
   ```

2. **`linux-x64` & `linux-arm64` (Debian 12 / `gcr.io/distroless/cc-debian12` glibc 2.36):**
   On macOS (where GNU Linux cross-compilers are not installed natively), `scripts/build.sh` automatically builds inside a cached `node:22-bookworm-slim` Docker container (`glibc 2.36`, matching PearTube's `gcr.io/distroless/cc-debian12` relay image) using `x86_64-linux-gnu-gcc` / `aarch64-linux-gnu-gcc`:
   ```sh
   sh scripts/build.sh linux-x64
   sh scripts/build.sh linux-arm64
   file prebuilds/linux-x64/peartube-soulseek-bare.bare prebuilds/linux-arm64/peartube-soulseek-bare.bare
   ```
   To probe either Linux artifact inside the exact `gcr.io/distroless/cc-debian12` runtime image PearTube's relay uses:
   ```sh
   # Set TARGET=linux-x64 PLATFORM=linux/amd64  OR  TARGET=linux-arm64 PLATFORM=linux/arm64
   TARGET=linux-x64 PLATFORM=linux/amd64
   docker build --platform "$PLATFORM" -t slsk-probe:$TARGET -f - . <<'EOF'
   FROM docker.io/library/node:22-bookworm-slim AS deps
   WORKDIR /app
   COPY package.json package-lock.json ./
   RUN npm ci --no-audit --no-fund \
     && P=$(basename node_modules/bare-runtime-linux-* | sed 's/^bare-runtime-//') \
     && mv node_modules/bare-runtime-$P/bin/bare /bare && chmod +x /bare
   FROM gcr.io/distroless/cc-debian12
   COPY --from=deps /bare /usr/local/bin/bare
   WORKDIR /app
   COPY package.json binding.js ./
   COPY --from=deps /app/node_modules ./node_modules
   COPY prebuilds ./prebuilds
   ENTRYPOINT ["/usr/local/bin/bare"]
   EOF
   docker run --rm --platform "$PLATFORM" slsk-probe:$TARGET -e "console.log(Object.keys(require('./binding.js')).join(','))"
   ```

3. **`android-arm64` (Android NDK r29 / API 29+, 16 KiB ELF page alignment):**
   Requires `ANDROID_NDK_HOME` (or an NDK installed under `$ANDROID_HOME/ndk`, auto-detected on macOS at `~/Library/Android/sdk/ndk/<ver>` and Linux at `~/Android/Sdk/ndk/<ver>`). `CMakeLists.txt` passes `-Wl,--unresolved-symbols=ignore-all -Wl,--undefined-version -Wl,-z,max-page-size=16384` so Bare `js_*`/`uv_*` symbols bind from `libbare-kit.so` at `dlopen` time and `PT_LOAD` segments satisfy Android 15+ 16 KiB page-size checks:
   ```sh
   export ANDROID_NDK_HOME="$HOME/Library/Android/sdk/ndk/29.0.14206865" # optional if under default SDK path
   sh scripts/build.sh android-arm64
   file prebuilds/android-arm64/peartube-soulseek-bare.bare
   ```
   One-line `adb` load probe on an `arm64-v8a` device/emulator using `bare-runtime-android-arm64` (Android `bare` writes `console.log` to `logcat -s bare:*`):
   ```sh
   tmp=$(mktemp -d)
   npm pack bare-runtime-android-arm64 --pack-destination "$tmp" && tar -xzf "$tmp"/bare-runtime-android-arm64-*.tgz -C "$tmp"
   adb shell 'rm -rf /data/local/tmp/slsk-probe && mkdir -p /data/local/tmp/slsk-probe/prebuilds/android-arm64'
   adb push "$tmp/package/bin/bare" /data/local/tmp/slsk-probe/bare
   adb push package.json binding.js /data/local/tmp/slsk-probe/
   adb push prebuilds/android-arm64/peartube-soulseek-bare.bare /data/local/tmp/slsk-probe/prebuilds/android-arm64/
   adb logcat -c
   adb shell 'chmod 755 /data/local/tmp/slsk-probe/bare && cd /data/local/tmp/slsk-probe && ./bare -e "console.log(Object.keys(require(\"./binding.js\")).join(\",\"))"'
   adb logcat -d -s bare:*
   adb shell 'rm -rf /data/local/tmp/slsk-probe' && rm -rf "$tmp"
   ```

## Usage Example (Bare + FLAC Verification)

```js
const { Soulseek } = require('peartube-soulseek-bare')
const subprocess = require('bare-subprocess')

const client = new Soulseek({
  logger: ({ level, event, message }) => {
    console.error(`[slsk:${level}] ${event}: ${message}`)
  }
})

await client.login('vps.slsknet.org', 2271, process.env.SOULSEEK_USER, process.env.SOULSEEK_PASS)

const hits = await client.search('boards of canada olson flac', 6000)
const best = hits
  .filter((f) => f.slots && f.filename.toLowerCase().endsWith('.flac') && (f.bit_depth ?? 16) >= 16)
  .sort((a, b) => a.queue - b.queue || b.speed - a.speed)[0]

if (!best) throw new Error('No free-slot FLAC candidate found')

const { path } = await client.download(best.username, best.filename, './downloads/', (p) => {
  if (p.phase === 'queued') console.log(`Queued at position ${p.queue}`)
  else if (p.phase === 'transferring') console.log(`${p.bytes} / ${p.total} bytes (${Math.round(p.speed / 1024)} KiB/s)`)
})

const check = subprocess.spawnSync('flac', ['-t', '--silent', path])
if (check.status !== 0) throw new Error(`Corrupt FLAC download: ${path}`)
console.log('Verified lossless FLAC:', path)

client.close()
```
