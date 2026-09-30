set(JIELI_ADAPTER_NAME jieli_tuya_adapter)

if(CONFIG_CHIP_CHOICE STREQUAL "wl82")
    set(JIELI_SELECTED_CHIP_DEFINE JIELI_SELECTED_CHIP_WL82)
elseif(CONFIG_CHIP_CHOICE STREQUAL "wl83")
    set(JIELI_SELECTED_CHIP_DEFINE JIELI_SELECTED_CHIP_WL83)
    # The wl83 audio provider pulls vendor media headers (audio_dac.h ->
    # asm/cpu_dac.h, adc_file.h, ...) whose homes sit outside include_lib's
    # driver/system trees. Mirror the include set the staged audio profile
    # gives the same translation unit (tools/jieli_build/audio_profile.py).
    set(JIELI_ADAPTER_CHIP_INCLUDES
        ${JIELI_SDK_ROOT}/include_lib/media
        ${JIELI_SDK_ROOT}/include_lib/media/cpu/wl83
        ${JIELI_SDK_ROOT}/include_lib/media/cpu/wl83/asm
        ${JIELI_SDK_ROOT}/include_lib/media/framework/include
        ${JIELI_SDK_ROOT}/include_lib/media/cvp
        ${JIELI_SDK_ROOT}/audio
        ${JIELI_SDK_ROOT}/audio/cpu/wl83
        ${JIELI_SDK_ROOT}/audio/CVP
        ${JIELI_SDK_ROOT}/audio/common
        ${JIELI_SDK_ROOT}/audio/effect
        ${JIELI_SDK_ROOT}/audio/effect/spatial_effect
        ${JIELI_SDK_ROOT}/audio/framework/include
        ${JIELI_SDK_ROOT}/audio/interface
        ${JIELI_SDK_ROOT}/audio/interface/includes
        # The vendor headers reach app_config.h / board_config.h with quoted
        # includes. The staged Makefile resolves them to the demo app's copies
        # because the vendor INCLUDES block lists the demo dirs before the
        # profile's wifi_camera dirs; keep the same precedence here or the
        # wifi_camera board chain (lcd_board_cfg_template.h) leaks in.
        ${JIELI_SDK_ROOT}/apps
        ${JIELI_SDK_ROOT}/apps/demo/demo_hello/include
        ${JIELI_SDK_ROOT}/apps/demo/demo_hello/board/wl83
        ${JIELI_SDK_ROOT}/include_lib/system/os
        ${JIELI_SDK_ROOT}/apps/common/system
        ${JIELI_SDK_ROOT}/apps/wifi_camera/include
        ${JIELI_SDK_ROOT}/apps/wifi_camera/board/wl83
    )
else()
    message(FATAL_ERROR "JIELI requires CONFIG_CHIP_CHOICE to be wl82 or wl83")
endif()

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/adapter_sources.txt" JIELI_ADAPTER_RELATIVE_SRC
    REGEX "^[^#].*\\.c$")
set(JIELI_ADAPTER_SRC)
foreach(source IN LISTS JIELI_ADAPTER_RELATIVE_SRC)
    if(source MATCHES "^src/chip/(wl82|wl83)/")
        set(source_chip "${CMAKE_MATCH_1}")
        if(NOT source_chip STREQUAL "${CONFIG_CHIP_CHOICE}")
            continue()
        endif()
    endif()
    list(APPEND JIELI_ADAPTER_SRC "${CMAKE_CURRENT_LIST_DIR}/${source}")
endforeach()

file(GLOB JIELI_ADAPTER_INCLUDE_ENTRIES LIST_DIRECTORIES true
    "${CMAKE_CURRENT_LIST_DIR}/include/*")
set(JIELI_ADAPTER_INCLUDE_DIRS)
foreach(include_entry IN LISTS JIELI_ADAPTER_INCLUDE_ENTRIES)
    if(IS_DIRECTORY "${include_entry}")
        list(APPEND JIELI_ADAPTER_INCLUDE_DIRS "${include_entry}")
    endif()
endforeach()
list(APPEND JIELI_ADAPTER_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include/init/include")

add_library(${JIELI_ADAPTER_NAME} STATIC ${JIELI_ADAPTER_SRC})
target_compile_definitions(${JIELI_ADAPTER_NAME} PRIVATE "${JIELI_SELECTED_CHIP_DEFINE}=1")
target_include_directories(${JIELI_ADAPTER_NAME}
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${JIELI_ADAPTER_INCLUDE_DIRS}
        ${TOP_SOURCE_DIR}/tools/porting/adapter/system
        ${TOP_SOURCE_DIR}/tools/porting/adapter/uart
        ${TOP_SOURCE_DIR}/tools/porting/adapter/init/include
        ${TOP_SOURCE_DIR}/tools/porting/adapter/utilities/include
        ${TOP_SOURCE_DIR}/tools/porting/adapter/security
        ${TOP_SOURCE_DIR}/tools/porting/adapter/flash
        ${TOP_SOURCE_DIR}/tools/porting/adapter/adc
        ${TOP_SOURCE_DIR}/tools/porting/adapter/kws
        ${TOP_SOURCE_DIR}/tools/porting/adapter/media
        ${TOP_SOURCE_DIR}/tools/porting/adapter/vad
        ${TOP_SOURCE_DIR}/tools/porting/adapter/network
        ${TOP_SOURCE_DIR}/tools/porting/adapter/wifi
        ${TOP_SOURCE_DIR}/tools/porting/adapter/bluetooth
        ${TOP_SOURCE_DIR}/tools/porting/adapter/timer
        ${TOP_SOURCE_DIR}/src/common/include
        ${JIELI_SDK_ROOT}/include_lib
        ${JIELI_SDK_ROOT}/include_lib/btstack
        ${JIELI_SDK_ROOT}/include_lib/btstack/le
        ${JIELI_SDK_ROOT}/include_lib/btctrler
        ${JIELI_SDK_ROOT}/include_lib/btctrler/port/${JIELI_CPU}
        ${JIELI_SDK_ROOT}/include_lib/net
        ${JIELI_SDK_ROOT}/include_lib/driver
        ${JIELI_SDK_ROOT}/include_lib/driver/device
        ${JIELI_SDK_ROOT}/include_lib/driver/cpu/${JIELI_CPU}
        ${JIELI_SDK_ROOT}/include_lib/system
        ${JIELI_SDK_ROOT}/include_lib/utils
        ${JIELI_SDK_ROOT}/include_lib/utils/syscfg
        ${JIELI_SDK_ROOT}/include_lib/utils/event
        ${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0
        ${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0/lwip/src/include
        ${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0/lwip/src/include/compat
        ${JIELI_SDK_ROOT}/include_lib/net/lwip_2_2_0/lwip/port
)

# Implementation-private vendor trees (the chip's media/audio headers plus the
# demo app / wifi_camera dirs whose app_config.h chain those headers quote).
# They stay out of the PUBLIC set above: anything linking the adapter should
# see its public headers, not the vendor SDK's internals.
target_include_directories(${JIELI_ADAPTER_NAME} PRIVATE
    ${JIELI_ADAPTER_CHIP_INCLUDES}
)

# The board's audio_config.h carries the JIELI_AUDIO_* mic routing macros the
# wl83 capture path reads. The staged vendor Makefile force-includes it for
# this object (audio_profile.py per_file_flags); mirror that here. The board
# CMakeLists exports the header path as JIELI_BOARD_AUDIO_CONFIG.
# Missing it is not survivable: the capture path would compile against the
# fallback JIELI_AUDIO_MIC_CHANNEL_MAP -> AUDIO_ADC_MIC_CH (all channels),
# which trips the vendor's audio_adc_mic_open ch err check and reboots the
# board — so fail the configure step instead of silently degrading.
if(CONFIG_CHIP_CHOICE STREQUAL "wl83" AND NOT EXISTS "${JIELI_BOARD_AUDIO_CONFIG}")
    message(FATAL_ERROR
        "JIELI wl83 board did not export JIELI_BOARD_AUDIO_CONFIG: the wl83 "
        "capture path needs the board's audio_config.h mic routing macros")
endif()
if(JIELI_BOARD_AUDIO_CONFIG AND EXISTS "${JIELI_BOARD_AUDIO_CONFIG}")
    foreach(source IN LISTS JIELI_ADAPTER_SRC)
        if(source MATCHES "tkl_audio\\.c$")
            set_source_files_properties("${source}" PROPERTIES
                COMPILE_OPTIONS "-include;${JIELI_BOARD_AUDIO_CONFIG}"
                OBJECT_DEPENDS "${JIELI_BOARD_AUDIO_CONFIG}"
            )
        endif()
    endforeach()
endif()
