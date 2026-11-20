cmake_minimum_required(VERSION 3.25)

find_package(cmake-bare REQUIRED PATHS node_modules/cmake-bare)

project(peartube_soulseek_bare C)

option(BARE_ADDON "Build the Soulseek Bare native addon" ON)

bare_target(target)
message(STATUS "Found cmake-bare (target=${target})")

if(NOT BARE_ADDON)
  return()
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

find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)

set(RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")
set(RUST_INCLUDE_DIR "${RUST_TARGET_DIR}/include")
set(RUST_STATIC_LIB "${RUST_TARGET_DIR}/${RUST_TARGET}/release/libpeartube_soulseek_bare_ffi.a")

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
elseif(target MATCHES "^android")
  target_link_libraries(${soulseek}_module PRIVATE dl m log)
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
