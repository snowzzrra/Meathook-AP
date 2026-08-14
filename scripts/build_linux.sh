#!/usr/bin/env bash
set -euo pipefail

repo=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
toolchain="$repo/cmake/windows-msvc-clang.cmake"
build_dir="$repo/build/linux-clang-msvc"
stage_dir="$repo/build/linux/Release-x64"

fail() {
  printf 'build_linux.sh: %s\n' "$*" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null 2>&1 || fail "required tool missing: $1"
}

select_llvm_tool() {
  local tool_name=$1
  local selected
  selected=$(command -v "$tool_name" 2>/dev/null || true)
  if [ -z "$selected" ]; then
    selected=$(command -v "$tool_name-14" 2>/dev/null || true)
  fi
  [ -n "$selected" ] || fail "required LLVM tool missing: $tool_name or $tool_name-14"
  printf '%s\n' "$selected"
}

require_directory() {
  [ -d "$1" ] || fail "required directory missing: $1"
}

for command_name in cmake ninja clang clang++ lld-link; do
  require_command "$command_name"
done
llvm_ar=$(select_llvm_tool llvm-ar)
llvm_readobj=$(select_llvm_tool llvm-readobj)
llvm_ml=$(select_llvm_tool llvm-ml)
require_command sha256sum || require_command shasum
[ -f "$repo/scripts/generate_rpc.sh" ] || fail 'scripts/generate_rpc.sh missing'
[ -x "$repo/scripts/generate_rpc.sh" ] || fail 'scripts/generate_rpc.sh must be executable'

: "${MH_MSVC_ROOT:?set MH_MSVC_ROOT to the exact MSVC tool root}"
: "${MH_WINSDK_ROOT:?set MH_WINSDK_ROOT to the exact Windows SDK root}"
: "${MH_WINSDK_VERSION:?set MH_WINSDK_VERSION to the exact Windows SDK version}"
require_directory "$MH_MSVC_ROOT/include"
require_directory "$MH_MSVC_ROOT/lib/x64"
for sdk_part in Include/"$MH_WINSDK_VERSION"/{ucrt,shared,um} Lib/"$MH_WINSDK_VERSION"/{ucrt/x64,um/x64}; do
  require_directory "$MH_WINSDK_ROOT/$sdk_part"
done

"$repo/scripts/generate_rpc.sh" --verify
cmake -S "$repo" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
  -DCMAKE_BUILD_TYPE=Release \
  -DMH_LLVM_AR="$llvm_ar" \
  -DMH_LLVM_ML="$llvm_ml" \
  -DMH_MSVC_ROOT="$MH_MSVC_ROOT" \
  -DMH_WINSDK_ROOT="$MH_WINSDK_ROOT" \
  -DMH_WINSDK_VERSION="$MH_WINSDK_VERSION"
cmake --build "$build_dir" --config Release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"

dll="$build_dir/bin/XINPUT1_3.dll"
[ -f "$dll" ] || fail "expected DLL missing: $dll"
mkdir -p "$stage_dir"
cp "$dll" "$stage_dir/XINPUT1_3.dll.tmp"
mv -f "$stage_dir/XINPUT1_3.dll.tmp" "$stage_dir/XINPUT1_3.dll"

pe_report=$("$llvm_readobj" --file-headers --coff-exports "$stage_dir/XINPUT1_3.dll") || fail 'llvm-readobj could not inspect staged PE'
printf '%s\n' "$pe_report"
printf '%s\n' "$pe_report" | grep -Eiq 'Magic:.*0x20b' || fail 'staged output is not PE32+ (optional-header magic 0x20B missing)'
printf '%s\n' "$pe_report" | grep -Eiq 'Machine:.*(IMAGE_FILE_MACHINE_AMD64|0x8664)' || fail 'staged output is not AMD64'
printf '%s\n' "$pe_report" | grep -Eq 'IMAGE_FILE_DLL' || fail 'staged output is not marked as DLL'

expected_exports=$'XInputGetCapabilities\nXInputGetState\nXInputSetState'
expected_exports+=$'\nmh_api_get'
actual_exports=$(printf '%s\n' "$pe_report" | sed -n 's/^[[:space:]]*Name: //p' | sort)
[ "$actual_exports" = "$(printf '%s\n' "$expected_exports" | sort)" ] || fail "staged exports do not exactly match required set: ${actual_exports//$'\n'/, }"

if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$stage_dir/XINPUT1_3.dll"
else
  shasum -a 256 "$stage_dir/XINPUT1_3.dll"
fi
printf 'staged: %s\n' "$stage_dir/XINPUT1_3.dll"
