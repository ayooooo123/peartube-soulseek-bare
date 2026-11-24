#!/usr/bin/env sh
# Cross-compile and stage prebuilds/<target>/peartube-soulseek-bare.bare.
#
# Usage:
#   sh scripts/build.sh <target>
#
# Targets:
#   darwin-arm64 | darwin-x64 | linux-x64 | linux-arm64 | android-arm64
#   all (builds darwin-arm64, linux-x64, linux-arm64, android-arm64)
#
# On macOS without Linux GNU cross-compilers installed locally, linux-x64 and
# linux-arm64 automatically build inside Debian 12 (bookworm, glibc 2.36) Docker
# containers matching PearTube's gcr.io/distroless/cc-debian12 relay runtime.
# Pass BUILD_IN_DOCKER=0 to force host-toolchain execution or BUILD_IN_DOCKER=1
# to force Docker execution.

set -eu

usage='usage: sh scripts/build.sh <darwin-arm64|darwin-x64|linux-x64|linux-arm64|android-arm64|all>'
target=${1:?$usage}
repo=$(cd "$(dirname "$0")/.." && pwd)
name='peartube-soulseek-bare.bare'

if [ "$target" = "all" ]; then
  for t in darwin-arm64 linux-x64 linux-arm64 android-arm64; do
    sh "$repo/scripts/build.sh" "$t"
  done
  exit 0
fi

host_os=$(uname -s)
host_arch=$(uname -m)

env_name() { printf '%s' "$1" | tr '-' '_'; }
var_name() { printf '%s' "$1" | tr 'a-z-' 'A-Z_'; }

run_docker_linux() {
  linux_target=$1
  case "$linux_target" in
    linux-x64|linux-arm64)
      if [ "$host_arch" = "arm64" ] || [ "$host_arch" = "aarch64" ]; then
        docker_platform="linux/arm64"
      else
        docker_platform="linux/amd64"
      fi
      ;;
    *) echo "internal error: unexpected docker target $linux_target" >&2; exit 1 ;;
  esac

  command -v docker >/dev/null 2>&1 || {
    echo "error: building $linux_target on $host_os requires either GNU cross-compilers or docker" >&2
    exit 1
  }

  # Avoid broken macOS credential helpers (e.g. docker-credential-desktop off PATH)
  # when pulling public Debian/Node base images.
  docker_cfg=${DOCKER_CONFIG:-}
  cleanup_cfg=""
  if [ -z "$docker_cfg" ] && [ -f "$HOME/.docker/config.json" ] && grep -q '"credsStore"' "$HOME/.docker/config.json" 2>/dev/null; then
    helper=$(sed -n 's/.*"credsStore"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$HOME/.docker/config.json" | head -n 1)
    if [ -n "$helper" ] && ! command -v "docker-credential-$helper" >/dev/null 2>&1; then
      cleanup_cfg=$(mktemp -d "${TMPDIR:-/tmp}/slsk-docker-cfg.XXXXXX")
      printf '{}\n' > "$cleanup_cfg/config.json"
      docker_cfg=$cleanup_cfg
    fi
  fi

  builder_img="peartube-soulseek-builder:$(printf '%s' "$docker_platform" | tr '/' '-')"
  if ! DOCKER_CONFIG=${docker_cfg:-$HOME/.docker} docker image inspect "$builder_img" >/dev/null 2>&1; then
    echo "==> Preparing cached Docker toolchain image $builder_img ($docker_platform, Debian 12 / glibc 2.36)..."
    DOCKER_CONFIG=${docker_cfg:-$HOME/.docker} docker build --platform "$docker_platform" -t "$builder_img" - <<'EOF'
FROM docker.io/library/node:22-bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive \
    RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH
RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl git build-essential cmake pkg-config file \
      gcc-x86-64-linux-gnu libc6-dev-amd64-cross \
      gcc-aarch64-linux-gnu libc6-dev-arm64-cross \
 && rm -rf /var/lib/apt/lists/* \
 && curl -fsSL https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable \
      --target x86_64-unknown-linux-gnu --target aarch64-unknown-linux-gnu
EOF
  fi

  echo "==> Building $linux_target in Docker ($builder_img)..."
  status=0
  DOCKER_CONFIG=${docker_cfg:-$HOME/.docker} docker run --rm \
    --platform "$docker_platform" \
    -v "$repo:/work" \
    -w /work \
    "$builder_img" \
    sh -ceu '
      mkdir -p /tmp/npm-work
      cp package.json package-lock.json /tmp/npm-work/
      npm --prefix /tmp/npm-work ci --ignore-scripts --no-audit --no-fund >/dev/null
      CMAKE_PREFIX_OVERRIDE=/tmp/npm-work/node_modules \
      CARGO_TARGET_DIR_OVERRIDE=/tmp/cargo-target \
      CMAKE_BUILD_DIR_OVERRIDE=/tmp/cmake-build \
      BUILD_IN_DOCKER=0 \
      sh scripts/build.sh "'"$linux_target"'"
    ' || status=$?
  [ -z "$cleanup_cfg" ] || rm -rf "$cleanup_cfg"
  return "$status"
}

case "$target" in
  darwin-arm64)
    [ "$host_os" = "Darwin" ] || { echo "error: $target requires a macOS host" >&2; exit 1; }
    triple=aarch64-apple-darwin
    c_compiler=$(xcrun --find clang 2>/dev/null || command -v clang)
    set -- -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_C_COMPILER=$c_compiler"
    ;;

  darwin-x64)
    [ "$host_os" = "Darwin" ] || { echo "error: $target requires a macOS host" >&2; exit 1; }
    triple=x86_64-apple-darwin
    c_compiler=$(xcrun --find clang 2>/dev/null || command -v clang)
    set -- -DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_OSX_ARCHITECTURES=x86_64 "-DCMAKE_C_COMPILER=$c_compiler"
    ;;

  ios-arm64)
    [ "$host_os" = "Darwin" ] || { echo "error: $target requires a macOS host" >&2; exit 1; }
    triple=aarch64-apple-ios
    sdk_path=$(xcrun --sdk iphoneos --show-sdk-path)
    c_compiler=$(xcrun --sdk iphoneos --find clang)
    export IPHONEOS_DEPLOYMENT_TARGET=17.0
    set -- -DCMAKE_SYSTEM_NAME=iOS "-DCMAKE_OSX_SYSROOT=$sdk_path" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 "-DCMAKE_C_COMPILER=$c_compiler"
    ;;

  ios-arm64-simulator)
    [ "$host_os" = "Darwin" ] || { echo "error: $target requires a macOS host" >&2; exit 1; }
    triple=aarch64-apple-ios-sim
    sdk_path=$(xcrun --sdk iphonesimulator --show-sdk-path)
    c_compiler=$(xcrun --sdk iphonesimulator --find clang)
    export IPHONEOS_DEPLOYMENT_TARGET=17.0
    set -- -DCMAKE_SYSTEM_NAME=iOS "-DCMAKE_OSX_SYSROOT=$sdk_path" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 "-DCMAKE_C_COMPILER=$c_compiler"
    ;;

  linux-x64)
    triple=x86_64-unknown-linux-gnu
    use_docker=${BUILD_IN_DOCKER:-auto}
    if [ "$use_docker" = "1" ]; then
      run_docker_linux "$target"
      exit 0
    fi
    if [ "$host_os" != "Linux" ]; then
      if [ "$use_docker" = "auto" ] && ! command -v x86_64-linux-gnu-gcc >/dev/null 2>&1; then
        run_docker_linux "$target"
        exit 0
      fi
      c_compiler=$(command -v x86_64-linux-gnu-gcc || true)
      [ -n "$c_compiler" ] || { echo "error: x86_64-linux-gnu-gcc not found (or enable Docker)" >&2; exit 1; }
      export "CARGO_TARGET_$(var_name "$triple")_LINKER=$c_compiler"
      export "CC_$(env_name "$triple")=$c_compiler"
    elif [ "$host_arch" != "x86_64" ] && [ "$host_arch" != "amd64" ]; then
      c_compiler=$(command -v x86_64-linux-gnu-gcc || true)
      [ -n "$c_compiler" ] || { echo "error: cross-compiling $target on $host_arch requires x86_64-linux-gnu-gcc" >&2; exit 1; }
      export "CARGO_TARGET_$(var_name "$triple")_LINKER=$c_compiler"
      export "CC_$(env_name "$triple")=$c_compiler"
    else
      c_compiler=$(command -v gcc || command -v cc)
    fi
    set -- -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=x86_64 "-DCMAKE_C_COMPILER=$c_compiler"
    ;;

  linux-arm64)
    triple=aarch64-unknown-linux-gnu
    use_docker=${BUILD_IN_DOCKER:-auto}
    if [ "$use_docker" = "1" ]; then
      run_docker_linux "$target"
      exit 0
    fi
    if [ "$host_os" != "Linux" ]; then
      if [ "$use_docker" = "auto" ] && ! command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        run_docker_linux "$target"
        exit 0
      fi
      c_compiler=$(command -v aarch64-linux-gnu-gcc || true)
      [ -n "$c_compiler" ] || { echo "error: aarch64-linux-gnu-gcc not found (or enable Docker)" >&2; exit 1; }
      export "CARGO_TARGET_$(var_name "$triple")_LINKER=$c_compiler"
      export "CC_$(env_name "$triple")=$c_compiler"
    elif [ "$host_arch" != "aarch64" ] && [ "$host_arch" != "arm64" ]; then
      c_compiler=$(command -v aarch64-linux-gnu-gcc || true)
      [ -n "$c_compiler" ] || { echo "error: cross-compiling $target on $host_arch requires aarch64-linux-gnu-gcc" >&2; exit 1; }
      export "CARGO_TARGET_$(var_name "$triple")_LINKER=$c_compiler"
      export "CC_$(env_name "$triple")=$c_compiler"
    else
      c_compiler=$(command -v gcc || command -v cc)
    fi
    set -- -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 "-DCMAKE_C_COMPILER=$c_compiler"
    ;;

  android-arm64)
    triple=aarch64-linux-android
    api=${ANDROID_API:-29}
    ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
    if [ -z "$ndk" ]; then
      case "$host_os" in
        Darwin) default_sdk="$HOME/Library/Android/sdk" ;;
        *)      default_sdk="$HOME/Android/Sdk" ;;
      esac
      sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$default_sdk}}
      if [ -d "$sdk/ndk" ]; then
        ndk=$(ls -d "$sdk"/ndk/* 2>/dev/null | sort -V | tail -n 1 || true)
      fi
    fi
    if [ -z "$ndk" ] || [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
      echo "error: Android NDK not found; set ANDROID_NDK_HOME or install under \$ANDROID_HOME/ndk" >&2
      exit 1
    fi
    ndk_prebuilt=$(ls -d "$ndk"/toolchains/llvm/prebuilt/* 2>/dev/null | head -n 1 || true)
    c_compiler="$ndk_prebuilt/bin/aarch64-linux-android${api}-clang"
    [ -x "$c_compiler" ] || { echo "error: NDK clang not found at $c_compiler" >&2; exit 1; }

    t_env=$(env_name "$triple")
    export "CARGO_TARGET_$(var_name "$triple")_LINKER=$c_compiler"
    export "CC_$t_env=$c_compiler"
    export "CXX_$t_env=${c_compiler}++"
    export "AR_$t_env=$ndk_prebuilt/bin/llvm-ar"

    set -- \
      "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" \
      -DANDROID_ABI=arm64-v8a \
      "-DANDROID_PLATFORM=android-$api" \
      -DANDROID_ALLOW_UNDEFINED_SYMBOLS=ON \
      -DANDROID_ALLOW_UNDEFINED_VERSION_SCRIPT_SYMBOLS=ON \
      "-DCMAKE_C_COMPILER=$c_compiler"
    ;;

  *)
    echo "$usage" >&2
    exit 1
    ;;
esac

cmake_prefix=${CMAKE_PREFIX_OVERRIDE:-"$repo/node_modules"}
if [ ! -d "$cmake_prefix/cmake-bare" ]; then
  echo "==> Installing cmake-bare devDependencies..."
  npm --prefix "$repo" ci --ignore-scripts --no-audit --no-fund
fi

if command -v rustup >/dev/null 2>&1; then
  if ! rustup target list --installed | grep -qx "$triple"; then
    echo "==> Installing Rust target $triple..."
    rustup target add "$triple"
  fi
fi

cargo_bin=${CARGO:-$(command -v cargo || echo "$HOME/.cargo/bin/cargo")}
cargo_dir=${CARGO_TARGET_DIR_OVERRIDE:-"$repo/build-$target/cargo"}
cmake_dir=${CMAKE_BUILD_DIR_OVERRIDE:-"$repo/build-$target/cmake"}

# Strip build-machine home paths out of panic strings in staged .bare artifacts.
us=$(printf '\037')
cargo_home=${CARGO_HOME:-$HOME/.cargo}
remap="--remap-path-prefix=$repo=."
remap="$remap${us}--remap-path-prefix=$cargo_home/registry=~/.cargo/registry"
remap="$remap${us}--remap-path-prefix=$cargo_home/git=~/.cargo/git"
remap="$remap${us}--remap-path-prefix=$HOME=~"
if [ "$target" = "android-arm64" ]; then
  export CARGO_ENCODED_RUSTFLAGS="-Clink-arg=-Wl,-z,max-page-size=16384${us}$remap"
else
  export CARGO_ENCODED_RUSTFLAGS="$remap"
fi

echo "==> [1/2] cargo build --release --target $triple ($target)"
CARGO_TARGET_DIR="$cargo_dir" "$cargo_bin" build --release \
  --manifest-path "$repo/Cargo.toml" \
  --target "$triple"

static_lib="$cargo_dir/$triple/release/libpeartube_soulseek_bare_ffi.a"
inc_dir="$cargo_dir/include"

echo "==> [2/2] cmake configure & build ($target, CC=$c_compiler)"
# Pre-create $cmake_dir/_bare/package.json BEFORE cmake runs so cmake-npm's
# install_node_module(bare-headers) stays inside $cmake_dir/_bare instead of
# walking up to $repo/package.json and mutating $repo/node_modules.
mkdir -p "$cmake_dir/_bare"
[ -f "$cmake_dir/_bare/package.json" ] || printf '{}\n' > "$cmake_dir/_bare/package.json"
PATH="$cmake_prefix/.bin:$PATH" cmake -S "$repo" -B "$cmake_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_PREFIX_PATH=$cmake_prefix" \
  "-DRUST_TARGET=$triple" \
  "-DRUST_TARGET_DIR=$cargo_dir" \
  "-DRUST_INCLUDE_DIR=$inc_dir" \
  "-DRUST_STATIC_LIB=$static_lib" \
  -DSKIP_CARGO_BUILD=ON \
  "$@"

cmake --build "$cmake_dir" --config Release

staged="$repo/prebuilds/$target/$name"
[ -f "$staged" ] || { echo "error: expected staged prebuild at $staged" >&2; exit 1; }

echo "==> Staged $staged ($(wc -c < "$staged" | tr -d ' ') bytes)"
if command -v file >/dev/null 2>&1; then
  file "$staged"
fi
