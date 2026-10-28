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
list(APPEND PLATFORM_PUBINC
    "${JIELI_ADAPTER_PATH}/include"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/system"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/mcu8080"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/uart"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/init/include"
    # JIELI vendors the utilities module - tuya_cloud_types.h and its siblings -
    # inside the adapter, the way the T5AI / ESP32 / GD32 platforms do. The
    # shared tools/porting tree stays untouched; see the note beside the copy.
    "${JIELI_ADAPTER_PATH}/include/utilities/include"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/security"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/flash"
    # libtuyaos.a aggregates the components as objects, which drops the
    # PUBLIC include interface each component target declared. The adapter
    # exposes adc and gpio to its own sources that way, so anything built
    # outside it - every example - needs them listed here instead. Both are
    # declared in boards/JIELI/TKL_Kconfig, and a declared capability whose
    # header is unreachable is a claim the build cannot honour.
    "${TOP_SOURCE_DIR}/tools/porting/adapter/adc"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/gpio"
    # pinmux is not a declared capability here (wl82 cannot back it, and a
    # platform-level switch has no chip dimension), but the examples that do
    # call tkl_io_pinmux_config - the i2c and uart ones - need the contract on
    # the include path to compile at all.
    "${TOP_SOURCE_DIR}/tools/porting/adapter/pinmux"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/i2c"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/pwm"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/spi"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/network"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/wifi"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/bluetooth"
    "${TOP_SOURCE_DIR}/tools/porting/adapter/timer"
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
