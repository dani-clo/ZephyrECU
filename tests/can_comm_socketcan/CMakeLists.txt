cmake_minimum_required(VERSION 3.20.0)
list(APPEND EXTRA_ZEPHYR_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/can_if
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/can_comm
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/diag_uds)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(can_comm_socketcan LANGUAGES C)
target_sources(app PRIVATE src/main.c)
target_compile_options(app PRIVATE -Wall -Wextra)
