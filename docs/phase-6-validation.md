# Phase 6 validation — 2026-10-30

## Authorization and scope

The user reviewed Phase 5 and coverage, requested a local commit without push,
and explicitly authorized the next phase. Phase 5 was committed as `81f06ca`
(`Add explicit-start application task manager and CAN task example`), including
its contract, tests and validation report. No push was performed. Prior local
instruction files/validation notes were left intact.

Read Phase 6 in `instructions_parte2.md` and inspected the pinned Zephyr NVS
source, flash APIs/drivers, generated board Devicetree/linker configurations and
existing application/thread ownership. Implemented Phase 6 only. Its changes
remain in the working tree for review; no subsequent-phase work, dependency
update, partition erase on hardware, hardware flash/program or push was performed.

## Delivered changes and decisions

- `components/nvm`: synchronous `ecu_nvm_init/read/write/delete`, one serialization
  mutex and bounded scratch buffer, private build-time NVS backend boundary.
  Public types have no NVS dependency; no ZMS implementation, async NVM API,
  manager worker/queue/cache, custom wear leveling or transaction framework.
- Manager-owned eight-byte little-endian `ECN1` envelope: payload version and
  exact size. Full supported records are CRC-checked before interpretation/copy.
  Failed reads leave caller buffers unchanged. `CONFIG_NVS_DATA_CRC` is selected;
  bad-region autoformat is prohibited. Existing NVS startup recovery remains NVS's
  responsibility and can perform flash operations.
- ID ranges: 1..0x0FFF framework, 0x1000..0xFFFE SWCs; 0 and NVS-internal 0xFFFF
  forbidden. Verified reserved-ID markers in the pinned NVS implementation.
- Distinct errno contract: absence, payload version, payload size, corruption,
  arguments, initialization/context, unavailable geometry/device, I/O and full
  storage. Unknown newer payload versions are rejected; caller owns recovery.
- The pinned NVS returns `-EIO` for both data CRC failure and flash-driver error.
  A private forwarding Zephyr flash device observes the real driver's failures;
  native CRC errors map to `-EBADMSG`, driver errors remain `-EIO`. All physical
  commands, page layout and NVS storage/recovery algorithms remain unchanged.
  This resolves an actual API ambiguity without patching third-party source.
- `app_led_config.c`: LED SWC owns a one-byte enable setting, ID 0x1000/version 1.
  Missing record defaults to enabled; other validation/I/O errors fail startup.
  A short setter updates state/generation under a spinlock and posts to the LED
  task. The task wakes the SWC-owned thread, which snapshots state, releases the
  lock, performs synchronous NVM write and reports completion/error. Changes
  arriving during write remain dirty; unchanged failed snapshots require explicit
  retry. No flash work runs in Task Manager, CAN/UDS callbacks or system workqueue.
- Optional diagnostic routine 0xF002 accepts the enable setting and reports
  enabled/pending/error flags, with positive acceptance distinct from durability.
  Existing 0xF001 and CAN TX scheduling/diagnostic ownership remain unchanged.
- Main initializes NVM and SWC configuration before application task start.
  Existing builds without NVM keep default LED behavior and log unavailable
  persistence. SWC worker default priority 8/stack 2048 is Kconfig-configurable.
- New NVM contract/tests, updated README/testing/UNO guide; Docker/GitHub workflows
  include the new raw SocketCAN application restart/persistence integration.

## Flash partitions, linker and board limitations

| Target | Partition change / support |
| --- | --- |
| UNO Q | Alias the existing `storage_partition` at offset 0xE0000, size 0x20000 (128 KiB); exclusive NVM ownership. No partition offsets/sizes changed. |
| native_sim | Alias existing storage at offset 0xFC000, size 0x4000 (16 KiB); simulated flash only. |
| S32K148 EVB | No partition/controller changes installed. NVM disabled by default; normal application still builds and explicitly logs unavailable persistence. |

UNO Q upstream calls the storage node `partition@f0000`, but `reg` is 0xE0000;
Flash Map uses the reg value. Existing boot/image slots and storage remain
separate in the board's fixed partition map. Actual geometry validated by the
manager: UNO Q 16 × 8192-byte erase sectors / 16-byte programming; native_sim
4 × 4096-byte sectors / 1-byte programming.

The existing UNO Q application ignores its chosen code partition and links direct
boot at 0x08000000. Fixed partitions alone do not reserve NVM in the linker.
Added `boards/arduino_uno_q.conf` with `CONFIG_FLASH_LOAD_SIZE=0xE0000`, preserving
that origin while bounding code strictly before storage. The component asserts
that an XIP image on the same flash has an explicit positive nonoverlapping load
region. This does not install MCUboot or relocate the image to slot0. Final UNO
linker: origin 0x08000000, length 0xE0000 (896 KiB).

S32K148's upstream FTFC is disabled. A temporary build-only overlay enabled it and
reserved the last 16 KiB outside a bounded test image. The existing Zephyr MCUX
flash driver failed to compile because the pinned S32 HAL has no `fsl_flash.h`.
No probe overlay was installed and no flash was touched. Added an early explicit
CMake rejection for selecting ECU_NVM on S32K1, rather than claiming persistence
or adding a low-level driver. This is the material target limitation: NVM is
implemented on UNO Q/native_sim, **not available on S32K148 with these dependencies**.

## Automated results

Final complete regression command:

```sh
./tools/run_docker.sh
```

Same image as Phase 5:
`sha256:1e8917c9fb26e5b7fa7d8a1a21fa86e132b71afc0d76e7038c7f6075d8b3edfc`.
No manifest revisions, third-party sources or Python locks changed in Phase 6.

| Check | Final result |
| --- | --- |
| Frozen manifest / Python consistency | PASS |
| Generated-code freshness + four Python codec/generator tests | PASS |
| Twister native_sim | PASS, 7/7 configurations/cases, no warnings, 47.49 s with concurrent coverage run |
| NVM manager contract | PASS: arguments, IDs, ISR/init, no autoformat, envelope/version/size/CRC, I/O distinction, concurrency, unchanged suppression, GC/full storage |
| SWC NVM application contract | PASS: startup/default/rejection policy, worker ownership, held write + newer setting, manager progress, error visibility/retry |
| NVM native process reboot | PASS: real native_sim process restart, new manager state, file-backed retained records/tombstones, version/size checks |
| Existing raw CAN, can_comm + UDS, full ISO-TP UDS integrations | PASS |
| Real application startup smoke | PASS, initialized NVM then CAN/UDS and application tasks |
| New raw SocketCAN NVM application integration | PASS: UDS input validation, durability status, configuration restored in a fresh second process |
| UNO Q firmware | PASS, NVM enabled, no compiler warnings |
| S32K148 firmware | PASS, NVM explicitly disabled, no compiler warnings |
| Explicit S32K1 NVM build | Expected early failure; driver support absent |
| Python syntax, Bash syntax, workflow YAML and whitespace checks | PASS |

The final separate UDS exerciser observed maximum P2 first-frame latency 11.50 ms
and immediate response 5.94 ms. These are Linux SocketCAN timing reports, not
worst-case application persistence measurements or target flash timing guarantees.

Tests inject two different kinds of corruption deliberately: malformed manager
containers written through the private backend with valid NVS ATE/data CRC, and a
payload bit flip in the underlying simulator RAM at the actual NVS data address,
leaving its ATE untouched. Both are rejected with `-EBADMSG`; an injected simulator
write failure is instead `-EIO`. GC/storage-full tests are bounded. Metadata that
NVS discards as incomplete can legitimately recover an older value or absence;
no impossible guarantee to distinguish invisible/lost metadata from missing IDs
is claimed.

The reboot suite enables native_sim reboot support and uses simulated reset
cause/file-backed flash; it actually execs a fresh process. The initial weak
reboot stub exited without reboot, so this support was enabled and verified
before declaring the test passed. Local `-flash_in_ram` runs apply only to suites
that do not need retained state across a process restart.

## Firmware builds and memory

Workspace venv on PATH; Zephyr SDK 1.0.1 / ARM GCC:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR=/home/daniele/zephyr-sdk-1.0.1 \
  west build -p always -b arduino_uno_q . -d ../build/phase6-uno-q \
  -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR=/home/daniele/zephyr-sdk-1.0.1 \
  west build -b s32k148_evb . -d ../build/phase6-s32k148 \
  -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

| Board | Linked flash | RAM | Persistence |
| --- | ---: | ---: | --- |
| UNO Q | 66,284 B of bounded 896 KiB | 43,944 B | NVM enabled |
| S32K148 EVB | 77,840 B of 1536 KiB | 44,160 B | Disabled: unsupported driver |

The UNO image region bound, data CRC and SWC worker options were verified in the
actual generated `.config`/linker. A cached native application build initially
missed the newly added board overlay; a pristine native rebuild selected the
alias, after which the complete standard Docker command passed. Early compiler
warnings about signed comparisons and an undefined helper macro in the new
routine were corrected; final board and warnings-as-errors Twister builds pass.

## Coverage

Ran the same Twister/gcovr command/filter as GitHub Actions, in a separate output
directory. All seven suites passed with coverage enabled and no warnings.

| Scope | Lines | Functions | Branches |
| --- | --- | --- | --- |
| All components, excluding generated code/tests | 831/903 = 92.0% | 77/80 = 96.2% | 575/738 = 77.9% |
| NVM public core `ecu_nvm.c` | 54/54 = 100% | 6/6 = 100% | 54/54 = 100% |
| NVS adaptation `nvm_nvs.c` | 52/55 = 94.5% | 11/11 = 100% | 37/56 = 66.1% |
| Whole NVM component | 106/109 = 97.2% | 17/17 = 100% | 91/110 = 82.7% |
| LED SWC persistence example (outside component total) | 51/53 = 96.2% | 6/6 = 100% | 24/36 = 66.7% |

Three uncovered backend executable lines are the generic unexpected-backend-error
fallback and geometry rejection returns. Additional unexercised branch outcomes
include unavailable devices, incompatible page/alignment geometry, non-normal
backend errors and defensive results. The native tests use one working flash
geometry. No coverage exclusions were added to inflate the results. Hardware-only
paths and the separate SocketCAN/Python runs are not included in Twister coverage;
100% core coverage is not a proof of races, hardware timing or electrical integrity.

Reports: workspace `build/phase6-coverage/components/index.html`, `coverage.txt`,
`summary.json`, `coverage.json`; complete measured source report under
`build/phase6-coverage/twister/coverage/`.

## Hardware validation still outstanding

No hardware boot, physical reset/power cycle, flash write/erase latency or stack
high-water measurement was performed. UNO's storage and image both occupy bank 1:
flash operations may stall instruction fetch and delay other threads and CAN
ISRs; driver IRQ masking can add a separate delay. Moving writes to a low-priority
SWC worker does not make them real-time-safe. Target measurements must include
GC/erase, CAN RX loss/ISR timing, TX spacing/lateness, UDS poll/P2/transport impact,
concurrent settings, deletion/version/size behavior after power loss, and worker
stack headroom. These are explicitly **unverified**, not assumed safe. S32K148
requires supported flash integration before any persistence measurements at all.

Main test logs/artifacts remain in workspace `build/docker/`, the standalone
local NVM builds in `build/tests/nvm*`, board builds in `build/phase6-*`, and
session logs `/tmp/zephyrecu-phase6-*`. Windows/GitHub workflow execution awaits
a future push. No push was made.

Phase 6 implementation and available automated validation are complete within
the documented board support limits. Stop here; do not implement later phases.
