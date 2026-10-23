cmake_minimum_required(VERSION 3.20.0)
list(APPEND EXTRA_ZEPHYR_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/can_if
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/diag_uds)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(diag_uds_socketcan LANGUAGES C)
target_sources(app PRIVATE src/main.c ../../app/src/app_led.c ../../app/src/app_diag.c)
target_include_directories(app PRIVATE ../../app/include)
target_compile_options(app PRIVATE -Wall -Wextra)
