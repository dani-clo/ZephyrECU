cmake_minimum_required(VERSION 3.20.0)
list(APPEND EXTRA_ZEPHYR_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/can_if
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/diag_uds)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(diag_uds_contract LANGUAGES C)
target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../../components/diag_uds/src
  ${WEST_TOPDIR}/modules/lib/iso14229)
target_compile_definitions(app PRIVATE UDS_SYS=5 UDS_CUSTOM_MILLIS=1)
target_compile_options(app PRIVATE -Wall -Wextra)
