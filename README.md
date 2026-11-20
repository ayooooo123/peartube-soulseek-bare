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
binding.js        `module.exports = require.addon()`
js/
  index.js        `Soulseek` class (`login`, `search`, `browse`, `download`, `state`, `setLogger`, `close`)
test/
  addon.test.js   End-to-end Bare runtime test against mock Soulseek server & peer sockets
```

## Build Order & Per-Platform Commands

`cmake-bare` is not hoisted in PearTube's root tree, so install the toolchain devDependencies first from a clean checkout:

```sh
npm i -D cmake-bare cmake-fetch cmake-npm
cargo build --release
cmake -S . -B build -DCMAKE_PREFIX_PATH="$PWD/node_modules"
cmake --build build
```

Run the Bare native addon E2E test:

```sh
./node_modules/.bin/bare test/addon.test.js
```

### Cross-Compilation Commands

1. **`darwin-arm64` (macOS Apple Silicon):**
   ```sh
   rustup target add aarch64-apple-darwin
   cmake -S . -B build-darwin-arm64 -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_PREFIX_PATH="$PWD/node_modules"
   cmake --build build-darwin-arm64
   ```
2. **`linux-x64` (Linux x86_64):**
   ```sh
   rustup target add x86_64-unknown-linux-gnu
   cmake -S . -B build-linux-x64 -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 -DRUST_TARGET=x86_64-unknown-linux-gnu -DCMAKE_PREFIX_PATH="$PWD/node_modules"
   cmake --build build-linux-x64
   ```
3. **`linux-arm64` (Linux aarch64):**
   ```sh
   rustup target add aarch64-unknown-linux-gnu
   cmake -S . -B build-linux-arm64 -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DRUST_TARGET=aarch64-unknown-linux-gnu -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_PREFIX_PATH="$PWD/node_modules"
   cmake --build build-linux-arm64
   ```
4. **`android-arm64` (Android NDK arm64-v8a, API 29+):**
   ```sh
   rustup target add aarch64-linux-android
   export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android29-clang"
   cmake -S . -B build-android-arm64 \
     -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
     -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
     -DRUST_TARGET=aarch64-linux-android \
     -DCMAKE_PREFIX_PATH="$PWD/node_modules"
   cmake --build build-android-arm64
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
