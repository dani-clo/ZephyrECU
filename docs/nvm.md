# Persistent data manager

`ecu_nvm.h` provides explicit initialization and synchronous read/write/delete of
versioned records. Enable `CONFIG_ECU_NVM`, add `components/nvm` to
`EXTRA_ZEPHYR_MODULES`, and define the `ecu-nvm` Devicetree alias to one exclusive
fixed flash partition with a working Zephyr flash driver. The implementation uses
Zephyr NVS, Flash Map and a Zephyr mutex; no dynamic allocation, background NVM
queue, cache, raw logging, custom wear leveling or multi-record transactions.

The public contract contains no NVS types. `src/nvm_private.h` is the build-time
backend boundary: a future ZMS adaptation must implement the same synchronous
operations and preserve the manager's envelope/API. ZMS is not implemented.

## API, initialization and ownership

```c
int ecu_nvm_init(void);
int ecu_nvm_read(uint16_t id, uint16_t expected_version,
                 void *data, size_t expected_size);
int ecu_nvm_write(uint16_t id, uint16_t version, const void *data, size_t size);
int ecu_nvm_delete(uint16_t id);
```

All calls are thread-context only. `ecu_nvm_init()` is explicit, idempotent after
success, and retryable after failure. It mounts existing storage without clearing
or formatting it on error. Standard NVS startup recovery can still perform flash
operations. Initialization therefore also needs a suitable execution window.

The application calls NVM init and required SWC configuration loads before
`ecu_task_manager_start()`. Main does this before starting CAN/UDS/can_comm. The
SWC's absent-record default does not automatically write flash during startup.
No callback should depend on configuration whose startup validation failed.

The mutex serializes complete operations, including the shared scratch buffer,
record validation, NVS access and final caller-buffer copy. Concurrent readers
wait for a writer; the mutex is not held by application code. There is no atomicity
across multiple API calls/records. Write input must remain stable until return.
A failed read leaves the entire caller buffer unchanged. Delete is idempotent.
Write returns 0 for both a successful update and unchanged data suppressed by NVS.

Payload sizes must be 1..`CONFIG_ECU_NVM_MAX_PAYLOAD_SIZE` (default 256, maximum
configurable 4096). All 16-bit payload versions, including zero, are legal. SWCs
own the payload's meaning and portable serialization; do not persist undocumented
C structure padding. The manager owns the envelope, exact length/version checks
and integrity validation, so SWCs do not add storage headers.

## IDs and errors

IDs are allocated at compile time by components, with no runtime allocator:

| ID range | Ownership |
| --- | --- |
| `0x0000` | Invalid/reserved by the framework |
| `0x0001..0x0FFF` | Framework components |
| `0x1000..0xFFFE` | SWCs; LED configuration owns `0x1000` |
| `0xFFFF` | Forbidden: NVS uses it for sector-close/GC/free markers |

The NVS restriction was checked in the pinned Zephyr v4.4.2
`subsys/kvss/nvs/nvs.c`, including sector-close and GC-done ATE construction. Do
not infer that all uint16 values are usable application IDs merely from NVS's
function signatures. Framework/SWC range ownership is a compile-time convention;
the manager accepts both legal ranges and does not implement access control.

| Return | Meaning |
| --- | --- |
| `0` | Success |
| `-ENOENT` | Missing or deleted record |
| `-EPROTONOSUPPORT` | Different payload version, including an unknown newer version |
| `-EMSGSIZE` | Structurally valid, CRC-checked record has a different payload size |
| `-EBADMSG` | Bad/unsupported envelope, inconsistent/truncated/oversized record, data CRC failure, or invalid NVS region at mount |
| `-EINVAL` | Reserved ID, NULL buffer, or zero/over-limit caller size |
| `-EACCES` | Read/write/delete before successful init |
| `-EWOULDBLOCK` | Called from ISR (checked first) |
| `-ENODEV` | Partition/device unavailable or geometry unsupported |
| `-EIO` | Underlying flash I/O or other backend failure |
| `-ENOSPC` | NVS has insufficient space for a write |

Version mismatch takes precedence over expected-size mismatch after complete
integrity/structural validation. A stored record beyond the configured maximum
is rejected as an unsupported container (`-EBADMSG`) without copying data; keep
this bound stable across firmware revisions that need to share records.

Unknown newer payload versions are rejected. The caller decides whether to
migrate explicitly, use defaults, or fail; the manager never silently overwrites
or downgrades records. The LED demo uses defaults **only** for `-ENOENT`, and
fails startup for version, size, corruption, I/O and invalid payload values.

## Private envelope and NVS integrity

The manager's v1 container has exactly eight header bytes followed by the payload:

| Byte offset | Encoding |
| --- | --- |
| 0..3 | Magic/format tag `ECN1` (`45 43 4E 31`) |
| 4..5 | Payload version, unsigned little endian 16-bit |
| 6..7 | Exact payload byte count, unsigned little endian 16-bit |
| 8.. | Payload bytes, without manager padding |

Serialization uses Zephyr byte-order helpers, not packed/native structures. No
second copy of NVS metadata or duplicate CRC is added. `CONFIG_NVS_DATA_CRC=y` is
selected by the component. The NVS CRC-32 protects all stored manager header and
payload bytes; NVS's ATE CRC-8 protects metadata/completion independently. The
manager requests the entire supported record into its own scratch buffer so
NVS actually checks the data CRC; partial NVS reads do not perform that check.
The manager validates the tag and length, then version and expected size, before
copying the payload. CRC is accidental corruption detection, not authentication.

The pinned NVS returns `-EIO` for both data CRC failure and device I/O failure.
A small **private forwarding Zephyr flash device** observes errors returned by
the real driver's read/write/erase functions. It forwards operations and page
layout unchanged; NVS retains all storage algorithms. Under the manager mutex,
a native NVS CRC `-EIO` without a driver failure becomes `-EBADMSG`, whereas an
observed driver failure becomes public `-EIO`. This preserves distinguishable
errors without patching Zephyr or reimplementing its record lookup/CRC/recovery.
This adapter is not a runtime backend plugin or a physical flash driver.

NVS ignores incomplete or invalid **metadata** during recovery and may retain an
older valid value, or report absence if none remains. The manager cannot identify
a lost record whose metadata is no longer visible as a record; it does not claim
to distinguish that case from a never-written ID. Corruption of a visible
record's manager container/data is rejected distinctly. Enabling NVS data CRC on
an area previously written without it is incompatible; use an exclusively
allocated area and an explicit migration/erase procedure, never autoformat.

The [Zephyr NVS documentation](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html)
explains full-read CRC validation, interrupted-write behavior, unchanged-data
suppression and sector requirements; pinned local source is authoritative for
this implementation's errno mapping.

## Partition and linker configuration

Runtime initialization checks the whole partition: uniform power-of-two physical
erase pages up to 64 KiB; exact page-aligned offset and size; 2..65535 sectors;
power-of-two program alignment up to 32 bytes; maximum record plus NVS CRC and
four aligned ATEs fitting one sector. One NVS sector equals one erase page. Invalid
geometry fails with `-ENODEV`. Missing alias/driver fails at build or init.
`CONFIG_NVS_INIT_BAD_MEMORY_REGION` is forbidden by a build assertion.

| Board | Exclusive partition (offset within flash) | Geometry | Application support |
| --- | --- | --- | --- |
| Arduino UNO Q | Existing `storage_partition`, `0xE0000..0x100000`, 128 KiB | 16 × 8192-byte sectors; 16-byte programming | NVM enabled by application default |
| native_sim | Existing `storage_partition`, `0xFC000..0x100000`, 16 KiB | 4 × 4096-byte sectors; 1-byte programming | NVM enabled; simulated persistence |
| S32K148 EVB | None allocated or modified | DT describes 4096-byte erase / 8-byte program, but driver unavailable | NVM disabled; startup explicitly warns |

UNO Q's upstream node name is `partition@f0000`, but its **reg value** is
`0xE0000`, which is the actual partition offset. The alias reuses that existing
area exclusively. Boot (`0..0x10000`), image-0 (`0x10000..0x78000`), image-1
(`0x78000..0xE0000`) and storage partition offsets/sizes are unchanged.

Fixed partitions alone do not constrain the Zephyr linker. The existing UNO Q
application uses direct boot at `0x08000000`, without
`CONFIG_USE_DT_CODE_PARTITION` or MCUboot. `boards/arduino_uno_q.conf` now bounds
`CONFIG_FLASH_LOAD_SIZE` to `0xE0000`, preserving that image origin and preventing
a growing linked image from entering NVM. This does **not** install/enable a
bootloader or relocate the application into the declared MCUboot image slot.
For a future MCUboot build, select/use its code partition and appropriate boot
configuration separately. An XIP build on the same internal flash must provide
a positive, nonoverlapping linker load region; the component asserts this at
build time. No hardware was flashed to exercise this change.

On S32K148 the upstream FTFC controller is disabled. A temporary build-only probe
that enabled it and reserved a nonoverlapping test partition failed in Zephyr's
`drivers/flash/soc_flash_mcux.c`: the pinned S32 HAL does not provide `fsl_flash.h`.
No probe overlay was installed in the project. The normal board still builds
with NVM disabled and reports unavailable persistence. Explicitly selecting
`CONFIG_ECU_NVM=y` on S32K1 fails early with a clear CMake error. Implementing a
new low-level driver or modifying HAL dependencies is outside this phase.

Other boards must opt in, supply the exclusive partition/alias, reserve it in the
linker and provide a supported Zephyr flash device. Sharing this area with
settings/filesystems/bootloader data is unsupported.

## LED SWC example and persistence ownership

`app_led_config.c` owns one enabled flag (ID `0x1000`, version 1, one payload byte
0/1). The normal default is enabled, preserving CAN LED behavior. The SWC validates
its byte's semantic range after manager validation. The application startup loads
it before the Task Manager begins.

`app_led_config_set_enabled()` updates synchronized desired state and a generation
under a short spinlock, then posts `ECU_EVENT_CONFIG_CHANGED` to the LED task. The
task gives a coalescing semaphore to the **LED SWC-owned persistence thread**.
That thread snapshots the payload and generation under the same spinlock, releases
the lock, calls synchronous `ecu_nvm_write()`, then records success/error. It runs
at preemptive priority 8 with a configurable default 2048-byte stack, separate
from both the manager and the system workqueue.

Changes arriving during a write remain dirty and request another pass. If an
old write fails while a newer change exists, the newer snapshot is still tried.
An unchanged failed snapshot is retained with its error and pending status;
retry requires another setter call (including the same desired value). No endless
retry loop or silent drop is introduced. Multiple intermediate settings can
coalesce intentionally into the latest desired state; this is state persistence,
not a history log. Clean unchanged values cause no NVM request; NVS also suppresses
unchanged envelopes. No high-frequency CAN counters/LED transitions are persisted.

`app_led_config_status()` exposes pending durability and the last write errno.
The sample diagnostic RoutineControl `0xF002`, present when NVM is enabled,
accepts start/control 1 with one byte (0/1), or result/control 3 with no input.
Its three output bytes are enabled, pending, and a nonzero last-error flag. A
positive start response accepts the in-memory setting; it is **not** a durability
acknowledgment. The handler only updates state/posts an event; it never writes
flash. As with the existing benign demo routine, production authorization policy
must be supplied by the application. Existing `0xF001` diagnostic LED is unchanged.

## Flash timing, wear and hardware validation

Write/delete/mount can block for a substantial time, including GC/erase. Never
invoke writes/deletes from Task Manager, CAN RX, UDS dispatch callbacks, ISR or
the system workqueue. The SWC-owned worker prevents software serialization of
those callbacks behind flash work, but cannot eliminate hardware flash stalls.

On single-bank controllers, programming/erase can stall instruction fetch across
threads and even delay CAN ISRs. Some drivers also disable interrupts: the pinned
MCUX driver documents and contains IRQ-masked flash operations. UNO Q's NVM area
is in bank 1, which also contains the direct-boot image; do not assume dual-bank
hardware makes these accesses independent. CAN reception, can_comm TX schedules
and UDS deadlines can all be affected. No real-time-safe write guarantee is made.

Before enabling persistence in a real ECU workload, measure on the target:

1. Observed read/write/delete and erase/GC worst latency, including a nearly-full
   partition, unchanged writes, and concurrent SWC requests.
2. CAN ISR/RX drops, TX spacing/lateness, UDS poll gaps/P2/transport timeouts during
   those operations; use target timestamps/GPIO or trace instrumentation.
3. Persistence and deletion after reset and full power removal, plus version and
   size mismatch policy on the next boot.
4. Persistence worker/Task Manager/CAN/UDS stack headroom through all error paths.

All of these target measurements remain **unverified** in this delivery. Native
flash simulation validates software behavior, not hardware flash stalls, wear,
power-loss electrical behavior, bootloader compatibility or timing guarantees.
Plan flash endurance and write frequency from the real device specifications.

## Automated verification

- `tests/nvm`: arguments/IDs/ISR rejection, explicit initialization, invalid-region
  rejection without autoformat and retry, version/size/unchanged writes, malformed
  envelopes stored with valid NVS metadata/data CRC, simulator data bit corruption
  with unchanged ATE, distinct driver I/O failure, concurrent callers, sector
  rotations/GC, full-storage error and read-only remount checks.
- `tests/nvm_app`: required init ordering, missing defaults and explicit rejection
  of newer/invalid configuration, real application callbacks handing work to the
  SWC thread, updates while a simulated write is held, manager progress during
  that write, no clean unchanged writes, visible error and explicit retry.
- `tests/nvm_reboot`: native_sim software reboot actually restarts the process
  with file-backed flash; a fresh manager mounts records and checks data,
  tombstones, version and size errors. This is not a physical power-cycle test.

The existing Twister discovery and GitHub component coverage include these suites
and the new component automatically. `./tools/run_docker.sh` also exercises the
existing SocketCAN integrations and the application startup smoke test.

`tools/test_nvm_socketcan.py` additionally runs the real application twice over
raw SocketCAN with one temporary file-backed flash. It verifies RoutineControl
input validation, configuration acceptance/durability status and the restored
setting in the second fresh process. Docker CI and GitHub's Linux workflow run
this after the application smoke test; no kernel ISO-TP socket is required.
