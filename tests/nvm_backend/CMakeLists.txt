cmake_minimum_required(VERSION 3.20.0)
list(APPEND EXTRA_ZEPHYR_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../../components/nvm)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nvm_backend_contract LANGUAGES C)
target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ../../components/nvm/src)
target_compile_options(app PRIVATE -Wall -Wextra)
# Keep the production backend intact; inject flash geometry and NVS outcomes at
# its external dependency boundaries, including driver failures via its proxy.
target_link_options(app PUBLIC
  -Wl,--wrap=flash_area_open
  -Wl,--wrap=z_impl_flash_get_page_info_by_offs
  -Wl,--wrap=nvs_mount -Wl,--wrap=nvs_read
  -Wl,--wrap=nvs_write -Wl,--wrap=nvs_delete)
