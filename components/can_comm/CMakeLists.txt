if(CONFIG_CAN_COMM)
  zephyr_library()
  zephyr_library_sources(src/can_comm.c src/can_scheduler.c
    generated/can_cfg.c generated/can_signals.c)
  zephyr_include_directories(include generated)
  zephyr_library_include_directories(src)
  zephyr_library_compile_options(-Wall -Wextra)
endif()
