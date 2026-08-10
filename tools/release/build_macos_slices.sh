#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: build_macos_slices.sh --source-directory PATH --configuration NAME
       --sdk-path PATH --deployment-target VERSION --ref-type TYPE --ref-name NAME
EOF
}

source_directory=
configuration=
sdk_path=
deployment_target=
ref_type=
ref_name=

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-directory)
      source_directory=$2
      shift 2
      ;;
    --configuration)
      configuration=$2
      shift 2
      ;;
    --sdk-path)
      sdk_path=$2
      shift 2
      ;;
    --deployment-target)
      deployment_target=$2
      shift 2
      ;;
    --ref-type)
      ref_type=$2
      shift 2
      ;;
    --ref-name)
      ref_name=$2
      shift 2
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

for required in \
  source_directory configuration sdk_path deployment_target ref_type ref_name; do
  if [[ -z "${!required}" ]]; then
    echo "Missing required value: $required" >&2
    usage >&2
    exit 2
  fi
done

source_directory="$(cd "$source_directory" && pwd)"
configuration_lower="$(printf '%s' "$configuration" | tr '[:upper:]' '[:lower:]')"
sha="$(git -C "$source_directory" rev-parse --short=7 HEAD)"

assert_clean_source() {
  local status

  status="$(git -C "$source_directory" status --porcelain --untracked-files=normal)"
  if [[ -n "$status" ]]; then
    printf 'CI source tree is modified:\n%s\n' "$status" >&2
    return 1
  fi
}

verify_thin_architecture() {
  local binary=$1
  local expected=$2
  local architectures

  architectures="$(lipo "$binary" -archs)"
  if [[ "$architectures" != "$expected" ]]; then
    echo "$binary contains '$architectures'; expected only '$expected'." >&2
    return 1
  fi
}

verify_deployment_target() {
  local binary=$1
  local build_information

  build_information="$(xcrun vtool -show-build "$binary")"
  if ! grep -Eq "minos[[:space:]]+$deployment_target([[:space:]]|$)" \
      <<< "$build_information"; then
    echo "$binary does not declare macOS $deployment_target as its minimum OS." >&2
    return 1
  fi
}

verify_runtime_closure() {
  local stage=$1
  local cli_dependencies
  local server_dependencies
  local sdk_library
  local sdk_dependencies

  cli_dependencies="$(otool -L "$stage/bin/axklib")"
  server_dependencies="$(otool -L "$stage/bin/axklib-server")"
  sdk_library="$(find "$stage/lib" -type f -name 'libaxklib*.dylib' -print -quit)"
  test -n "$sdk_library"
  sdk_dependencies="$(otool -L "$sdk_library")"
  printf '%s\n' "$cli_dependencies" "$server_dependencies" "$sdk_dependencies"
  if grep -Eqi 'lib(axk|sndfile|soxr|FLAC|ogg|vorbis|opus|mpg123|mp3lame)' \
      <<< "$(tail -n +2 <<< "$cli_dependencies")"; then
    echo "CLI has a private native runtime dependency" >&2
    return 1
  fi
  if grep -Eqi 'lib(axk|sndfile|soxr|FLAC|ogg|vorbis|opus|mpg123|mp3lame)' \
      <<< "$(tail -n +2 <<< "$server_dependencies")"; then
    echo "server has a private native runtime dependency" >&2
    return 1
  fi
  if grep -Eqi 'lib(sndfile|soxr|FLAC|ogg|vorbis|opus|mpg123|mp3lame)' \
      <<< "$(tail -n +2 <<< "$sdk_dependencies")"; then
    echo "C++ SDK has a private codec runtime dependency" >&2
    return 1
  fi
}

build_desktop_slice() {
  local architecture=$1
  local rust_target=$2
  local build_directory=$3
  local server=$4
  local cargo_root="${AXK_MACOS_CARGO_ROOT:-$source_directory/build/tmp/macos-cargo}"
  local cargo_target_directory="$cargo_root/$rust_target"
  local sidecar="$source_directory/apps/axkdeck/src-tauri/binaries/axklib-server-$rust_target"

  rm -rf "$cargo_target_directory"
  mkdir -p "$cargo_target_directory"

  if [[ "$architecture" == x86_64 ]]; then
    export CARGO_TARGET_X86_64_APPLE_DARWIN_RUNNER="/usr/bin/arch -x86_64"
  fi
  AXKLIB_SERVER_BINARY="$server" \
  AXKLIB_BUILD_DIRECTORY="$build_directory" \
  AXKLIB_VERSION_METADATA_FILE="$build_directory/version_metadata.json" \
  AXKLIB_PACKAGE_BASENAME_FILE="$build_directory/package_basename.txt" \
  CARGO_TARGET_DIR="$cargo_target_directory" \
    cargo test \
      --manifest-path "$source_directory/apps/axkdeck/src-tauri/Cargo.toml" \
      --target "$rust_target"
  (
    cd "$source_directory/apps/axkdeck"
    AXKLIB_SERVER_BINARY="$server" \
    AXKLIB_BUILD_DIRECTORY="$build_directory" \
    AXKLIB_VERSION_METADATA_FILE="$build_directory/version_metadata.json" \
    AXKLIB_PACKAGE_BASENAME_FILE="$build_directory/package_basename.txt" \
    AXKDECK_BUILD_NUMBER="${GITHUB_RUN_NUMBER:-0}" \
    CARGO_TARGET_DIR="$cargo_target_directory" \
      pnpm desktop:build -- --target "$rust_target" --no-bundle
  )
  verify_thin_architecture "$sidecar" "$architecture"
  if find "$cargo_target_directory" -type f -name CMakeCache.txt -print -quit | grep .; then
    echo "Cargo unexpectedly configured a nested axklib build." >&2
    return 1
  fi
}

package_slice() {
  local platform=$1
  local build_directory=$2
  local stage=$3
  local output="$source_directory/build/macos-slices/$platform"

  rm -rf "$output"
  mkdir -p "$output"
  tar -czf "$output/$platform.tar.gz" -C "$stage" .
  cp "$build_directory/version_metadata.json" \
    "$output/$platform-version-metadata.json"
  cp "$build_directory/package_basename.txt" \
    "$output/$platform-package-basename.txt"
}

build_slice() {
  local architecture=$1
  local triplet=$2
  local platform=$3
  local rust_target=$4
  local build_directory="$source_directory/build/native/$platform/$configuration_lower"
  local stage="$source_directory/build/tmp/macos-slice-$platform"
  local server="$stage/bin/axklib-server"
  local binary
  local library_arguments=()

  rm -rf "$build_directory" "$stage"
  mkdir -p "$build_directory" "$stage"
  cmake \
    -S "$source_directory" \
    -B "$build_directory" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$configuration" \
    -DCMAKE_TOOLCHAIN_FILE="$source_directory/external/vcpkg/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_OSX_ARCHITECTURES="$architecture" \
    -DCMAKE_OSX_SYSROOT="$sdk_path" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
    -DAXK_BUILD_SERVER=ON \
    -DBUILD_TESTING=ON \
    -DAXK_SBOM_FILE="$build_directory/axklib-sdk.spdx.json" \
    -DAXK_CLI_SBOM_FILE="$build_directory/axklib-cli.spdx.json" \
    -DAXK_SERVER_SBOM_FILE="$build_directory/axklib-server.spdx.json" \
    -DVCPKG_TARGET_TRIPLET="$triplet" \
    -DVCPKG_OVERLAY_TRIPLETS="$source_directory/library/cmake/triplets"
  cmake --build "$build_directory" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

  python "$source_directory/tools/python/release_metadata.py" resolve \
    --version-metadata-file "$build_directory/version_metadata.json" \
    --package-basename-file "$build_directory/package_basename.txt" \
    --ref-type "$ref_type" --ref-name "$ref_name" \
    --platform "$platform" --configuration "$configuration"
  python "$source_directory/tools/python/release_metadata.py" verify-cli \
    --version-metadata-file "$build_directory/version_metadata.json" \
    --package-basename-file "$build_directory/package_basename.txt" \
    --cli "$build_directory/apps/cli/axklib" \
    --git-sha-short "$sha" --ref-name "$ref_name"

  uv --project "$source_directory/tools/python" run python \
    "$source_directory/tools/python/generate_sbom.py" --axklib-root "$source_directory" \
    --version-metadata-file "$build_directory/version_metadata.json" \
    --package-basename-file "$build_directory/package_basename.txt" \
    --profile sdk --output "$build_directory/axklib-sdk.spdx.json"
  uv --project "$source_directory/tools/python" run python \
    "$source_directory/tools/python/generate_sbom.py" --axklib-root "$source_directory" \
    --version-metadata-file "$build_directory/version_metadata.json" \
    --package-basename-file "$build_directory/package_basename.txt" \
    --profile cli --output "$build_directory/axklib-cli.spdx.json"
  uv --project "$source_directory/tools/python" run python \
    "$source_directory/tools/python/generate_sbom.py" --axklib-root "$source_directory" \
    --version-metadata-file "$build_directory/version_metadata.json" \
    --package-basename-file "$build_directory/package_basename.txt" \
    --profile server --output "$build_directory/axklib-server.spdx.json"

  # Native CTest can launch Intel test binaries through macOS translation.
  # Running an ARM64 Homebrew CTest itself under Rosetta fails before discovery.
  ctest --test-dir "$build_directory" --output-on-failure

  for component in sdk cli server; do
    cmake --install "$build_directory" --prefix "$stage" --component "$component"
  done
  while IFS= read -r binary; do
    library_arguments+=(--allow-library "$(basename "$binary")")
  done < <(find "$stage/lib" \( -type f -o -type l \) \
    -name 'libaxklib*.dylib' -print | sort)
  python "$source_directory/tools/python/inspect_package.py" \
    "$stage" "${library_arguments[@]}"
  for binary in "$stage/bin/axklib" "$server"; do
    verify_thin_architecture "$binary" "$architecture"
    verify_deployment_target "$binary"
  done
  while IFS= read -r binary; do
    verify_thin_architecture "$binary" "$architecture"
  done < <(find "$stage/lib" -type f \
    \( -name 'libaxklib*.a' -o -name 'libaxklib*.dylib' \) -print | sort)
  while IFS= read -r binary; do
    verify_deployment_target "$binary"
  done < <(find "$stage/lib" -type f -name 'libaxklib*.dylib' -print | sort)
  verify_runtime_closure "$stage"

  build_desktop_slice "$architecture" "$rust_target" "$build_directory" "$server"
  package_slice "$platform" "$build_directory" "$stage"
}

assert_clean_source
build_slice x86_64 x64-osx-axk macos-x64 x86_64-apple-darwin
build_slice arm64 arm64-osx-axk macos-arm64 aarch64-apple-darwin
assert_clean_source
