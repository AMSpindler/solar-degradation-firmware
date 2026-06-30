# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/include"
                       REQUIRES esp_adc
                                esp_timer
                                esp_driver_i2c
                                esp_driver_gpio
                                esp_driver_usb_serial_jtag
                                nvs_flash
                                console
                                vfs
                                esp_event
                                # Phase B: Wi-Fi transport + SD logging
                                esp_wifi
                                esp_netif
                                lwip
                                esp_driver_spi
                                esp_driver_sdspi
                                sdmmc
                                fatfs
                                # MQTT — esp-mqtt vendored in components/mqtt
                                mqtt
                       )
