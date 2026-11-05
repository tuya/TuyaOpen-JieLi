message(STATUS "[JIELI] platform selected for ${CONFIG_BOARD_CHOICE} (${CONFIG_CHIP_CHOICE})")

if(CONFIG_CHIP_CHOICE STREQUAL "wl82")
    set(JIELI_CPU wl82)
    set(JIELI_SELECTED_CHIP_DEFINE JIELI_SELECTED_CHIP_WL82)
    set(JIELI_SDK_PROBE "apps/demo/demo_hello/board/wl82/Makefile")
elseif(CONFIG_CHIP_CHOICE STREQUAL "wl83")
    set(JIELI_CPU wl83)
    set(JIELI_SELECTED_CHIP_DEFINE JIELI_SELECTED_CHIP_WL83)
    set(JIELI_SDK_PROBE "apps/demo/demo_hello/board/wl83/Makefile")
else()
    message(FATAL_ERROR "JIELI requires CONFIG_CHIP_CHOICE to be wl82 or wl83")
endif()

if(DEFINED ENV{JIELI_SDK_ROOT})
    set(JIELI_SDK_ROOT "$ENV{JIELI_SDK_ROOT}")
elseif(CONFIG_CHIP_CHOICE STREQUAL "wl83" AND EXISTS "${PLATFORM_PATH}/chip/wl83/AC792_SDK/sdk/Makefile")
    set(JIELI_SDK_ROOT "${PLATFORM_PATH}/chip/wl83/AC792_SDK/sdk")
elseif(CONFIG_CHIP_CHOICE STREQUAL "wl82" AND EXISTS "${PLATFORM_PATH}/chip/wl82/AC79_AIoT_SDK")
    set(JIELI_SDK_ROOT "${PLATFORM_PATH}/chip/wl82/AC79_AIoT_SDK")
elseif(CONFIG_CHIP_CHOICE STREQUAL "wl82" AND EXISTS "${PLATFORM_PATH}/AC79_AIoT_SDK")
    set(JIELI_SDK_ROOT "${PLATFORM_PATH}/AC79_AIoT_SDK")
else()
    if(CONFIG_CHIP_CHOICE STREQUAL "wl82")
        set(JIELI_SDK_ROOT "${PLATFORM_PATH}/../../../AC79_AIoT_SDK")
    else()
        message(FATAL_ERROR "WL83 SDK not found under ${PLATFORM_PATH}")
    endif()
endif()
if(NOT EXISTS "${JIELI_SDK_ROOT}/${JIELI_SDK_PROBE}")
    message(FATAL_ERROR
        "Jieli SDK profile does not match CONFIG_CHIP_CHOICE=${CONFIG_CHIP_CHOICE}: ${JIELI_SDK_ROOT}")
endif()
file(TO_CMAKE_PATH "${JIELI_SDK_ROOT}" JIELI_SDK_ROOT)
set(JIELI_SDK_ROOT "${JIELI_SDK_ROOT}" CACHE PATH "JieLi SDK root" FORCE)

# The legacy Jieli toolchain's lto-ar must create the archive index. A host
# ranlib cannot index pi32v2 bitcode archives used by the full TuyaOpen image.
if(CMAKE_HOST_WIN32)
    set(JIELI_RANLIB "${PLATFORM_PATH}/jieli_ranlib.cmd")
    set(CMAKE_AR "${PLATFORM_PATH}/jieli_llvm_ar.cmd" CACHE FILEPATH "Jieli archive tool" FORCE)
else()
    set(JIELI_RANLIB "${PLATFORM_PATH}/jieli_ranlib.sh")
endif()
set(CMAKE_RANLIB "${JIELI_RANLIB}" CACHE FILEPATH "Jieli archive indexer" FORCE)
set(CMAKE_C_COMPILER_RANLIB "${JIELI_RANLIB}" CACHE FILEPATH "Jieli C archive indexer" FORCE)
set(CMAKE_CXX_COMPILER_RANLIB "${JIELI_RANLIB}" CACHE FILEPATH "Jieli C++ archive indexer" FORCE)

set(JIELI_ADAPTER_PATH "${PLATFORM_PATH}/tuyaos/tuyaos_adapter")

# The adapter tree owns the TKL contract headers on this platform. Modules
# resolve them through PLATFORM_TKL_ROOT.
set(PLATFORM_TKL_ROOT "${JIELI_ADAPTER_PATH}/include")

# TuyaOpen's own src/libtls/port/tuya_tls_config.h is the mbed TLS configuration
# here. Naming it as MBEDTLS_CONFIG_FILE replaces the upstream defaults that
# build_info.h would otherwise layer on top, which select the POSIX/Windows
# timing, socket and file backends (and PSA ITS storage) the PI32V2 target
# cannot build.
add_compile_definitions(MBEDTLS_CONFIG_FILE=\"tuya_tls_config.h\")

add_compile_definitions(MBEDTLS_AES_ROM_TABLES)

list(APPEND PLATFORM_PUBINC
    "${JIELI_ADAPTER_PATH}/include"
    "${JIELI_ADAPTER_PATH}/include/uart"
    "${JIELI_ADAPTER_PATH}/include/spi"
    "${JIELI_ADAPTER_PATH}/include/i2c"
    "${JIELI_ADAPTER_PATH}/include/pwm"
    "${JIELI_ADAPTER_PATH}/include/adc"
    "${JIELI_ADAPTER_PATH}/include/gpio"
    "${JIELI_ADAPTER_PATH}/include/pinmux"
    "${JIELI_ADAPTER_PATH}/include/timer"
    "${JIELI_ADAPTER_PATH}/include/flash"
    "${JIELI_ADAPTER_PATH}/include/system"
    "${JIELI_ADAPTER_PATH}/include/network"
    "${JIELI_ADAPTER_PATH}/include/wifi"
    "${JIELI_ADAPTER_PATH}/include/bluetooth"
    "${JIELI_ADAPTER_PATH}/include/security"
    "${JIELI_ADAPTER_PATH}/include/mcu8080"
    "${JIELI_ADAPTER_PATH}/include/mipi_dsi"
    "${JIELI_ADAPTER_PATH}/include/media"
    "${JIELI_ADAPTER_PATH}/include/vad"
    "${JIELI_ADAPTER_PATH}/include/kws"
    "${JIELI_ADAPTER_PATH}/include/init/include"
    "${JIELI_ADAPTER_PATH}/include/utilities/include"
    "${TOP_SOURCE_DIR}/src/common/include"
)

list(APPEND PLATFORM_PUBINC
    "${JIELI_SDK_ROOT}/include_lib/driver/device"
    "${JIELI_SDK_ROOT}/include_lib/driver/cpu/${JIELI_CPU}"
    "${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0"
    "${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0/lwip/src/include"
    "${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0/lwip/src/include/compat"
    "${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0/lwip/port"
    "${JIELI_SDK_ROOT}/include_lib"
    "${JIELI_SDK_ROOT}/include_lib/btstack"
    "${JIELI_SDK_ROOT}/include_lib/btstack/le"
    "${JIELI_SDK_ROOT}/include_lib/btctrler"
    "${JIELI_SDK_ROOT}/include_lib/btctrler/port/${JIELI_CPU}"
    "${JIELI_SDK_ROOT}/include_lib/net"
    "${JIELI_SDK_ROOT}/include_lib/system"
    "${JIELI_SDK_ROOT}/include_lib/system/generic"
    "${JIELI_SDK_ROOT}/include_lib/utils"
    "${JIELI_SDK_ROOT}/include_lib/utils/syscfg"
    "${JIELI_SDK_ROOT}/include_lib/utils/event"
)

set(PLATFORM_NEED_LIBS "")
