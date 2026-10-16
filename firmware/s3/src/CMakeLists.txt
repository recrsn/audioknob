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
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_http_server
    # Weather and IP geolocation over HTTPS (device/net/online.c), app version for About.
    esp_http_client
    json
    esp_app_format
    # Firmware updates (device/net/ota.c, device/net/esp32_fw.c).
    app_update
    lwip
    mbedtls
    mdns
    esp_audio_codec
    libsodium
)

# ESP-IDF runs every component's CMakeLists.txt once in a restricted "early
# expansion" pass just to gather REQUIRES, before the real configure.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES ${AK_REQUIRES})
    return()
endif()

# lvgl is pulled in as a plain PlatformIO lib, not a component that REQUIRES
# us, so its lv_conf_internal.h can't find our lv_conf.h unless the project
# include/ dir is added to every component's compile flags.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_SOURCE_DIR}/include" APPEND)

# Sources include each other by bare file name, so every directory is on the
# include path.
set(AK_DIRS ui ui/assets device/core device/drivers device/net device/airplay device/airplay/shared)
set(AK_INCLUDE_DIRS ${AK_DIRS})
set(AK_SRCS "")
foreach(dir ${AK_DIRS})
    file(GLOB dir_srcs "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.c")
    list(APPEND AK_SRCS ${dir_srcs})
endforeach()
list(APPEND AK_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/main.c")

# The device web page (device/net/web_settings.c), served as is.
set(AK_EMBED_TXTFILES "device/net/web/index.html")

if(CONFIG_AK_AIRPLAY2)
    # AirPlay 2 after shairport-sync, which takes AirPlay 1 senders on the
    # same port. Pairing is owntone's pair_ap, built from lib/ by PlatformIO.
    file(GLOB AK_AIRPLAY2_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/device/airplay/v2/*.c")
    list(APPEND AK_SRCS ${AK_AIRPLAY2_SRCS})
    list(APPEND AK_INCLUDE_DIRS "device/airplay/v2")
else()
    # AirPlay 1 only: the RAOP receiver on its own port.
    file(GLOB AK_AIRPLAY1_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/device/airplay/v1/*.c")
    list(APPEND AK_SRCS ${AK_AIRPLAY1_SRCS})
endif()

idf_component_register(
    SRCS ${AK_SRCS}
    INCLUDE_DIRS ${AK_INCLUDE_DIRS}
    REQUIRES ${AK_REQUIRES}
    EMBED_TXTFILES ${AK_EMBED_TXTFILES}
)
