#!/bin/sh

set -eu
if [ -z "${JIELI_TOOL_DIR:-}" ]; then
    DIR="$(cd "$(dirname "$0")" && pwd -P)"
    # Keep the version in sync with LINUX_TOOLCHAIN_VERSION in
    # tools/jieli_build/toolchain.py, which downloads it on first build.
    if [ -x "${DIR}/.tools/portable-jieli-linux/jieli-linux-toolchains-20260730.1/pi32v2/bin/lto-ar" ]; then
        JIELI_TOOL_DIR="${DIR}/.tools/portable-jieli-linux/jieli-linux-toolchains-20260730.1/pi32v2/bin"
    elif [ -x "${DIR}/chip/wl82/ipc_ac7916a/toolchain/jieli-linux-toolchains/pi32v2/bin/lto-ar" ]; then
        JIELI_TOOL_DIR="${DIR}/chip/wl82/ipc_ac7916a/toolchain/jieli-linux-toolchains/pi32v2/bin"
    elif [ -x "${DIR}/../tools/jieli-toolchain/jieli-linux-toolchains-20260730.1/pi32v2/bin/lto-ar" ]; then
        JIELI_TOOL_DIR="${DIR}/../tools/jieli-toolchain/jieli-linux-toolchains-20260730.1/pi32v2/bin"
    elif [ -x "${DIR}/../../tools/jieli-toolchain/jieli-linux-toolchains-20260730.1/pi32v2/bin/lto-ar" ]; then
        JIELI_TOOL_DIR="${DIR}/../../tools/jieli-toolchain/jieli-linux-toolchains-20260730.1/pi32v2/bin"
    elif [ -x "${DIR}/../jieli-toolchain/pi32v2/bin/lto-ar" ]; then
        JIELI_TOOL_DIR="${DIR}/../jieli-toolchain/pi32v2/bin"
    elif [ -x "${DIR}/../../jieli-toolchain/pi32v2/bin/lto-ar" ]; then
        JIELI_TOOL_DIR="${DIR}/../../jieli-toolchain/pi32v2/bin"
    elif [ -x "/opt/jieli/pi32v2/bin/lto-ar" ]; then
        JIELI_TOOL_DIR="/opt/jieli/pi32v2/bin"
    fi
fi
if [ -z "${JIELI_TOOL_DIR:-}" ]; then
    echo "jieli_ranlib.sh: lto-ar not found; set JIELI_TOOL_DIR or run a build once to download the toolchain" >&2
    exit 1
fi
exec "${JIELI_TOOL_DIR}/lto-ar" s "$@"
