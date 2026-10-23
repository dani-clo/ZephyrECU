if(CONFIG_DIAG_UDS)
  if(NOT DEFINED WEST_TOPDIR)
    message(FATAL_ERROR "ZephyrECU requires a west workspace (WEST_TOPDIR missing)")
  endif()
  set(ISO14229_ROOT "${WEST_TOPDIR}/modules/lib/iso14229")
  if(NOT EXISTS "${ISO14229_ROOT}/iso14229.c")
    message(FATAL_ERROR "iso14229.c missing: run west update")
  endif()

  # Identical ABI/time macros for upstream and the private adapter sources.
  # UDS_SYS_ZEPHYR (5) enables UDS_TP_ISOTP_C in the pinned header itself.
  set(uds_definitions UDS_SYS=5 UDS_CUSTOM_MILLIS=1)
  zephyr_library_named(diag_uds_upstream)
  zephyr_library_sources(${ISO14229_ROOT}/iso14229.c)
  zephyr_library_include_directories(${ISO14229_ROOT})
  zephyr_library_compile_definitions(${uds_definitions} _POSIX_C_SOURCE=200809L)

  zephyr_library_named(diag_uds)
  zephyr_library_sources(src/diag_uds.c src/diag_uds_zephyr_port.c)
  zephyr_library_include_directories(${ISO14229_ROOT} src)
  zephyr_include_directories(include)
  zephyr_library_compile_definitions(${uds_definitions})
  zephyr_library_compile_options(-Wall -Wextra)
endif()
