# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS "."
                       REQUIRES esp_adc
                                esp_timer
                                console
                                esp_driver_usb_serial_jtag
                                vfs)
