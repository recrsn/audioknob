file(GLOB_RECURSE AK_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

idf_component_register(
    SRCS ${AK_SRCS}
    INCLUDE_DIRS .
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_pcnt
        esp_driver_uart
        esp_timer
        bt
        nvs_flash
        esp_app_format
        app_update
        mbedtls
)
