#!/usr/bin/env bash
set -eu

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../../.." && pwd)"
binary="${TMPDIR:-/tmp}/jieli-audio-tkl-tests"
san_binary="${TMPDIR:-/tmp}/jieli-audio-tkl-tests-sanitized"
sources=(
    platform/JIELI/tests/audio/test_tkl_audio.c
    platform/JIELI/tuyaos/tuyaos_adapter/src/driver/tkl_audio.c
    platform/JIELI/tuyaos/tuyaos_adapter/src/driver/tkl_vad.c
)
includes=(
    -Iplatform/JIELI/tests/audio/include
    -Iplatform/JIELI/tuyaos/tuyaos_adapter/include
    -Itools/porting/adapter/media
    -Itools/porting/adapter/system
    -Itools/porting/adapter/vad
    -Itools/porting/adapter/utilities/include
    -Isrc/common/include
)

cd "${repo_root}"
gcc -std=c99 -Wall -Wextra -Werror \
    "${includes[@]}" "${sources[@]}" \
    -o "${binary}"
"${binary}"

gcc -std=c99 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${includes[@]}" "${sources[@]}" \
    -o "${san_binary}"
"${san_binary}"
