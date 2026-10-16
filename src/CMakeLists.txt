# ESP-IDF ignores PlatformIO's build_src_filter, so this repo's two MCU
# targets (src/s3/, src/esp32/) are separated here instead: each env passes
# -DAK_TARGET=s3|esp32 via board_build.cmake_extra_args in platformio.ini.

set(AK_REQUIRES
    driver
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_i2s
    esp_driver_ledc
    esp_driver_pcnt
    esp_driver_uart
    esp_lcd
    esp_lcd_st77916
    esp_lcd_touch_cst816s
    esp_new_jpeg
    libpng
    esp_timer
    bt
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_http_server
    # Weather and IP geolocation over HTTPS (online.c), app version for About.
    esp_http_client
    json
    esp_app_format
    # Firmware updates (s3/ota.c, esp32/fw_update.c).
    app_update
    lwip
    mbedtls
    mdns
    esp_audio_codec
    libsodium
)

# ESP-IDF runs every component's CMakeLists.txt once in a restricted "early
# expansion" pass (a separate `cmake -P` invocation that doesn't see our
# -DAK_TARGET) just to gather REQUIRES, before the real configure. REQUIRES
# must be declared identically in both passes, since the dependency graph is
# built from what this early pass captures, not from the real one.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES ${AK_REQUIRES})
    return()
endif()

if(NOT DEFINED AK_TARGET)
    message(FATAL_ERROR "AK_TARGET not set — pass -DAK_TARGET=s3|esp32 via board_build.cmake_extra_args")
endif()

if(AK_TARGET STREQUAL "s3")
    # lvgl is pulled in as a plain PlatformIO lib, not a component that
    # REQUIRES us, so its lv_conf_internal.h can't find our lv_conf.h unless
    # the project include/ dir is added to every component's compile flags.
    idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}/include" APPEND)
endif()

file(GLOB AK_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/${AK_TARGET}/*.c")
set(AK_INCLUDE_DIRS "${AK_TARGET}")
set(AK_EMBED_TXTFILES "")

if(AK_TARGET STREQUAL "s3")
    # main_native.c is the env:native (SDL/desktop) entry point only.
    list(REMOVE_ITEM AK_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/main_native.c")

    # The device web page (web_settings.c), served as is.
    list(APPEND AK_EMBED_TXTFILES "s3/web/index.html")

    # Shared by both receivers (s3/airplay/): raop.h, the receiver interface
    # airplay.c uses, the AirPlay 1 RTP receiver and session setup, DACP,
    # DMAP metadata and the esp_audio_codec decoders.
    file(GLOB AK_AIRPLAY_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/airplay/*.c")
    list(APPEND AK_SRCS ${AK_AIRPLAY_SRCS})
    list(APPEND AK_INCLUDE_DIRS "s3/airplay")

    if(CONFIG_AK_AIRPLAY2)
        # AirPlay 2 after shairport-sync, which takes AirPlay 1 senders on the
        # same port, with owntone's pair_ap on an mbedtls backend
        # (PAIR_MBEDTLS).
        file(GLOB AK_AIRPLAY2_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/airplay2/*.c")
        file(GLOB AK_PAIR_AP_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/airplay2/pair_ap/*.c")
        list(APPEND AK_SRCS ${AK_AIRPLAY2_SRCS} ${AK_PAIR_AP_SRCS})
        list(APPEND AK_INCLUDE_DIRS "s3/airplay2" "s3/airplay2/pair_ap")
        # Upstream style; unused parameters and signed/unsigned compares are its own.
        set_source_files_properties(${AK_PAIR_AP_SRCS} PROPERTIES
            COMPILE_DEFINITIONS "PAIR_MBEDTLS=1"
            COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-parameter;-Wno-unused-function")
    else()
        # AirPlay 1 only: the RAOP receiver on its own port.
        file(GLOB AK_AIRPLAY1_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/airplay1/*.c")
        list(APPEND AK_SRCS ${AK_AIRPLAY1_SRCS})
    endif()
endif()

idf_component_register(
    SRCS ${AK_SRCS}
    INCLUDE_DIRS ${AK_INCLUDE_DIRS}
    REQUIRES ${AK_REQUIRES}
    EMBED_TXTFILES ${AK_EMBED_TXTFILES}
)
