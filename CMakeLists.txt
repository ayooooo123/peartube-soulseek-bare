cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)

project(peartube_soulseek_bare C)

option(BARE_ADDON "Build the Soulseek Bare native addon" ON)

bare_target(target)
message(STATUS "Found cmake-bare (target=${target})")

if(NOT BARE_ADDON)
  return()
endif()

# Ensure ${CMAKE_BINARY_DIR}/_bare/package.json exists before cmake-bare calls
# install_node_module(bare-headers), otherwise npm walks up to the repo root
# package.json and --force prunes devDependencies from root node_modules/.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/_bare/package.json")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_bare")
  file(WRITE "${CMAKE_BINARY_DIR}/_bare/package.json" "{}\n")
endif()

# Map Bare target (<platform>-<arch>) to Rust target triple
if(NOT DEFINED RUST_TARGET)
  if(target STREQUAL "darwin-arm64")
    set(RUST_TARGET "aarch64-apple-darwin")
  elseif(target STREQUAL "darwin-x64")
    set(RUST_TARGET "x86_64-apple-darwin")
  elseif(target STREQUAL "linux-arm64")
    set(RUST_TARGET "aarch64-unknown-linux-gnu")
  elseif(target STREQUAL "linux-x64")
    set(RUST_TARGET "x86_64-unknown-linux-gnu")
  elseif(target STREQUAL "android-arm64")
    set(RUST_TARGET "aarch64-linux-android")
  elseif(target STREQUAL "android-arm")
    set(RUST_TARGET "armv7-linux-androideabi")
  elseif(target STREQUAL "android-x64")
    set(RUST_TARGET "x86_64-linux-android")
  else()
    message(FATAL_ERROR "Unsupported Bare target '${target}'; pass -DRUST_TARGET=<triple>")
  endif()
endif()

set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo" CACHE PATH "Cargo target directory")
set(RUST_INCLUDE_DIR "${RUST_TARGET_DIR}/include" CACHE PATH "Directory containing soulseek.h")
set(RUST_STATIC_LIB "${RUST_TARGET_DIR}/${RUST_TARGET}/release/libpeartube_soulseek_bare_ffi.a" CACHE FILEPATH "Rust staticlib")
option(SKIP_CARGO_BUILD "Use prebuilt RUST_STATIC_LIB and RUST_INCLUDE_DIR instead of invoking cargo" OFF)

if(SKIP_CARGO_BUILD)
  if(NOT EXISTS "${RUST_STATIC_LIB}")
    message(FATAL_ERROR "SKIP_CARGO_BUILD=ON but RUST_STATIC_LIB not found: ${RUST_STATIC_LIB}")
  endif()
  if(NOT EXISTS "${RUST_INCLUDE_DIR}/soulseek.h")
    message(FATAL_ERROR "SKIP_CARGO_BUILD=ON but header not found: ${RUST_INCLUDE_DIR}/soulseek.h")
  endif()
  add_custom_target(peartube_soulseek_rust)
else()
  find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)

  add_custom_command(
    OUTPUT "${RUST_STATIC_LIB}" "${RUST_INCLUDE_DIR}/soulseek.h"
    COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
            "${CARGO_EXECUTABLE}" build --release
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            --target "${RUST_TARGET}"
    DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
      "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
      "${CMAKE_CURRENT_SOURCE_DIR}/build.rs"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building Rust staticlib from pinned peartube-soulseek git dependency (${RUST_TARGET})"
    VERBATIM
  )

  add_custom_target(
    peartube_soulseek_rust
    DEPENDS "${RUST_STATIC_LIB}" "${RUST_INCLUDE_DIR}/soulseek.h"
  )
endif()

add_bare_module(soulseek)

target_sources(
  ${soulseek}
  PRIVATE
    soulseek/addon.c
)

target_include_directories(
  ${soulseek}
  PRIVATE
    "${RUST_INCLUDE_DIR}"
)

add_dependencies(${soulseek} peartube_soulseek_rust)
add_dependencies(${soulseek}_module peartube_soulseek_rust)

target_link_libraries(
  ${soulseek}_module
  PRIVATE
    "${RUST_STATIC_LIB}"
)

if(target MATCHES "^linux")
  target_link_libraries(${soulseek}_module PRIVATE pthread dl m)
  target_link_options(${soulseek}_module PRIVATE -Wl,--strip-debug)
elseif(target MATCHES "^android")
  target_link_libraries(${soulseek}_module PRIVATE dl m log)
  # NDK android-legacy.toolchain.cmake defaults to -Wl,--no-undefined and -Wl,--no-undefined-version;
  # Bare addons resolve js_* and uv_* from the host libbare-kit.so / bare runtime at dlopen time.
  target_link_options(
    ${soulseek}_module
    PRIVATE
      -Wl,--unresolved-symbols=ignore-all
      -Wl,--undefined-version
      -Wl,--strip-debug
      -Wl,-z,max-page-size=16384
  )
endif()

# Stage prebuilds/<target>/peartube-soulseek-bare.bare for `require.addon()` resolution
set(PREBUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilds/${target}")
add_custom_command(
  TARGET ${soulseek}_module
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${PREBUILD_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "$<TARGET_FILE:${soulseek}_module>"
          "${PREBUILD_DIR}/peartube-soulseek-bare.bare"
  COMMENT "Staging ${PREBUILD_DIR}/peartube-soulseek-bare.bare"
  VERBATIM
)
