# Phase 1 validation — 2026-10-23

## Delivered scope

- west T2 manifest, frozen revisions and verified minimal import allowlist.
- Three local Zephyr module discovery stubs with dependency-aware Kconfig.
- C17 app with `-Wall -Wextra`, startup logging and LED initialization.
- Shared application LED service: Devicetree `led0` on S32K148EVB, atomic
  in-memory state and logging on native_sim. Initialization precedes consumers;
  future consumers must serialize concurrent writes if ordering matters.
- Build/run guide and Python environment snapshot. No later-phase CAN/UDS
  functionality is implemented.

## Environment

Debian Linux x86_64; Python 3.13.5; west 1.5.0; CMake 3.31.6; Ninja 1.12.1.
Zephyr SDK 1.0.1 at `/home/daniele/zephyr-sdk-1.0.1`;
ARM GCC `arm-zephyr-eabi-gcc (Zephyr SDK 1.0.1) 14.3.0`.
Native compiler: `gcc (Debian 14.2.0-19) 14.2.0`, host toolchain, default
32-bit `native_sim/native` target. Dependency SHAs are in `west-frozen.yml`;
Zephyr v4.4.2 resolved to `dccb09599635bdff17633fa7e9dab014b91dce90`.

The existing checkout was initialized in place: workspace root is
`/home/daniele/workspace/myProjects`, with the application at `ZephyrECU/`.
The sibling `.west`, `.venv`, `zephyr`, `modules` and `build` directories are
workspace artifacts outside the application Git repository.

## Commands and observed outcomes

Commands below are relative to the workspace root, with `.venv/bin` on PATH.

| Check | Result |
| --- | --- |
| `west init -l ZephyrECU` | Passed; workspace initialized |
| `west update --narrow -o=--depth=1` | Passed; all manifest dependencies resolved |
| `west manifest --validate` | Passed |
| `west manifest --freeze > ZephyrECU/west-frozen.yml` | Full dependency SHA snapshot recorded |
| `ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR=/home/daniele/zephyr-sdk-1.0.1 west build -p always -b s32k148_evb ZephyrECU -d build/s32k148_evb -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` | Passed; flash 55,872 B, RAM 8,704 B |
| `ZEPHYR_TOOLCHAIN_VARIANT=host west build -p always -b native_sim -S socketcan-native-sim ZephyrECU -d build/native_sim -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` | Passed |
| Isolated `vcan0` native smoke run from README, `-stop_at=0.1` | Exit 0; LED memory backend initialized off; bootstrap ready; simulator stopped at 0.110 s |

Both targets were first built with the suggested four imported projects, then
rebuilt pristine with only `cmsis_6` and `hal_nxp`. Both minimal builds passed
without compiler warnings. The final board configuration selects
`CONFIG_PICOLIBC_USE_TOOLCHAIN=y`. Both configurations have `CONFIG_STD_C17=y`,
all three local component symbols enabled and `CONFIG_ISOTP` disabled. The native
configuration enables `CONFIG_CAN_NATIVE_LINUX` through the upstream snippet.

`clang-tidy` 19.1.7 inspected `src/main.c` and `app/src/app_led.c` using the native
`compile_commands.json`. GCC-only options `-fno-freestanding`,
`-fno-reorder-functions`, and `-fno-defer-pop` were removed from a temporary
analysis copy of the database under `build/clang-tidy-native/`; build flags were
unchanged. No first-party source findings. The tool reports an unused GCC
`--param=min-pagesize=0` argument and suppresses diagnostics from upstream headers.
The direct unadapted run failed on unsupported GCC arguments; the adapted run
completed successfully. `cppcheck` is unavailable, so it was not run. Static
analysis of the hardware backend remains for later quality validation.

## Limitations and next-phase notes

- No physical board boot, GPIO level or electrical CAN verification was done.
- The host has no preexisting `vcan0` and passwordless sudo is unavailable.
  User/network namespaces worked, so the smoke run used a real isolated vcan
  interface without host network changes. No application CAN traffic is expected
  until phase 2; this check proves startup, not RX/TX behavior.
- The selected iso14229 tag and commit match the requirements, but both its
  amalgamated header and `src/version.h` report `UDS_LIB_VERSION "0.10.2"`.
  The dependency SHA was preserved. Verify actual APIs in phase 3 instead of
  relying on this version string.
- `--can-if vcan0` from instructions.md is rejected by the pinned native parser;
  the verified form is `--can-if=vcan0`. The authoritative instructions file
  was preserved unchanged.
- Modules are discovery stubs; bundled ISO-TP is not compiled until phase 3.
