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
    lwip
    mbedtls
    mdns
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

if(AK_TARGET STREQUAL "s3")
    # main_native.c is the env:native (SDL/desktop) entry point only.
    list(REMOVE_ITEM AK_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/main_native.c")

    # AirPlay receiver: the RAOP port (s3/raop/) and Apple's ALAC decoder,
    # vendored unmodified (s3/alac/).
    file(GLOB AK_RAOP_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/raop/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/s3/raop/*.cpp")
    file(GLOB AK_ALAC_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/s3/alac/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/s3/alac/*.cpp")
    list(APPEND AK_SRCS ${AK_RAOP_SRCS} ${AK_ALAC_SRCS})
    list(APPEND AK_INCLUDE_DIRS "s3/raop" "s3/alac")

    # Apple's endianness sniffing only recognises x86/Win32 hosts; without
    # this it byte-swaps everything on Xtensa. Warnings are upstream's.
    set_source_files_properties(${AK_ALAC_SRCS} PROPERTIES
        COMPILE_DEFINITIONS "TARGET_RT_LITTLE_ENDIAN=1"
        COMPILE_OPTIONS "-Wno-error;-w")
endif()

idf_component_register(
    SRCS ${AK_SRCS}
    INCLUDE_DIRS ${AK_INCLUDE_DIRS}
    REQUIRES ${AK_REQUIRES}
)
