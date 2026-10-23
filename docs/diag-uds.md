# UDS server design

`diag_uds` compiles the west-managed iso14229 source and its bundled `isotp-c`.
It uses `can_if` for every CAN subscription and transmission. Zephyr ISO-TP stays
disabled. Public application handlers include only `diag_uds.h`; they do not use
CAN identifiers, the upstream server type or ISO-TP APIs.

## Build and ownership

The selected dependency is iso14229 tag `0.11.0`, pinned to
`8a7eb23d6ab9d52f66ce36b07ba456613709c78e`. Its version string still reports
`0.10.2`; integration follows the actual checked-out headers and source.
`components/diag_uds/CMakeLists.txt` resolves `ISO14229_ROOT` from `WEST_TOPDIR` and
fails with a `west update` hint when upstream source is missing.

Upstream and the adapter compile with identical `UDS_SYS=5` and
`UDS_CUSTOM_MILLIS=1`. The pinned header enables `UDS_TP_ISOTP_C` and its four-argument
`isotp_user_send_can(..., void *context)` ABI automatically. The upstream source
has a separate library target: first-party `-Wall -Wextra` is scoped to the adapter.
`_POSIX_C_SOURCE=200809L` exposes `strnlen` for upstream's strict-C17 native build.
Upstream sources remain unmodified, including unused service implementations.
The application admits only supported SIDs before invoking their parsers.

Startup order is:

1. Initialize the application LED service and `can_if`.
2. Call `diag_uds_init()` to register physical/functional RX routes.
3. Register DID and routine handlers through public APIs.
4. Start the CAN controller once through `can_if_start()`.
5. Call `diag_uds_start()` to start the diagnostic worker.

All lifecycle calls are thread-only, nonblocking on contention (`-EBUSY`). Startup
waits at most 100 ms for worker initialization. Repeated successful init/start
returns `-EALREADY`; handler registration freezes when start is requested.
DID/RID duplicates return `-EALREADY`, full registries `-ENOSPC`. Handler contexts
remain valid for application lifetime. A partially installed RX registry can be
retried after filter installation failure; existing identical routes are reused.
There is no runtime unregistration, diagnostic restart or concurrent tester API.

The worker alone initializes and mutates the upstream server, physical/functional
links, response cache and transport state. The CAN dispatcher only copies frames
into the diagnostic queue. Driver TX callbacks update atomics; the worker observes
completion failures and aborts the affected physical exchange. No library state
is changed by a CAN callback or application thread.

## Addressing, queues and timing

Defaults, configurable in Kconfig:

| Setting | Default |
| --- | --- |
| Physical request / response | standard IDs `0x7E0` / `0x7E8` |
| Functional request | standard ID `0x7DF` |
| Diagnostic RX queue | 32 copied `can_if_frame_t` objects |
| DID / routine handlers | 8 entries per registry |
| Worker stack / preemptive priority | 4096 bytes / 4 |
| Poll interval | 1 ms, configurable to 2 ms |
| Maximum handler output / routine input | 128 / 64 bytes |
| Maximum DIDs per read request | 32 |

IDs must be distinct; initialization rejects conflicting configuration. There
are two exact RX routes, each owned/shared by `can_if`. Both queues are bounded
and drop the newest incoming frame on overflow. The diagnostic queue's drops
are counted separately from the common CAN ingress drops.

The worker consumes at most eight queued frames per iteration, polls transport
and server, attempts pending output, then sleeps for the configured interval.
This bounds RX work between polls. Native driver priority 2, worker priority 4
and dispatcher priority 5 allow the driver to receive confirmations promptly,
while the worker sleeps between polls. Tick frequency must be at least 1000 Hz;
the supplied configuration uses 1000 Hz. Poll count, largest poll gap and gaps
of 5 ms or more are visible in diagnostic statistics. These are scheduling
observations, not on-target timing certification.

`UDSMillis()` returns absolute uptime milliseconds. `isotp_user_get_us()` converts
the absolute 64-bit uptime tick count into microseconds before reducing to
32 bits. It advances independently of call frequency, wraps naturally about every
71.6 minutes, and has 1 ms resolution with the supplied tick rate. It promises no
sub-millisecond scheduling. A tester's shorter STmin is conservatively exceeded
by the worker's interval. The bundled transport handles wrap-aware deadlines.

Advertised P2 is **50 ms**, P2* **5000 ms**, S3 **5100 ms**. P2 is a maximum
response time, not an intended delay. The pinned library rearms its `p2_timer`
after sending a response, so its internal `server.p2_ms` is set independently to
**1 ms**. Immediate handlers respond at the next eligible poll/TX opportunity;
the 50 ms deadline remains available for scheduling and temporary TX contention.
A pending response expires at the original request's P2 deadline. Every admitted
request in an extended session refreshes S3, including TesterPresent with positive
response suppression. S3 expiry returns the server to the default session.
Slow/asynchronous handlers and `0x78 ResponsePending` are not supported: handlers
must complete promptly, and unexpected handler statuses map to GeneralReject.
P2* is advertised but no pending-response exchange is exposed.

## Services and application handlers

Only these SIDs reach the upstream service parser:

| Service | Policy |
| --- | --- |
| `0x10` DiagnosticSessionControl | Exactly two bytes; default `0x01` and extended `0x03` sessions only |
| `0x3E` TesterPresent | Exactly two bytes; standard zero subfunction and positive suppression |
| `0x22` ReadDataByIdentifier | Odd request length, one to 32 DIDs; registered read-only handlers |
| `0x31` RoutineControl | Four to 68 bytes; physical requests only; registered immediate handlers |

Other services return NRC `0x11` on physical requests, before their upstream
parsers run. Invalid supported-service lengths return `0x13`; unknown DID/RID
returns `0x31`. Handler errors propagate through an explicit NRC allowlist.
Output length is checked before copying to the server response. The library
checks the final combined response size. Memory/flash access, SecurityAccess,
ECU reset and file transfer are not exposed.

The application demonstration registers:

| Identifier | Behavior |
| --- | --- |
| DID `0xF190` | ASCII `ZephyrECU-UDS-01`, without NUL; demo identification, not a production VIN |
| DID `0xF186` | Active session byte (`0x01` or `0x03`) |
| RID `0xF001`, start `0x01` | Toggle the diagnostic RGB channel; return one state byte |
| RID `0xF001`, results `0x03` | Read that channel without changing it |

The LED routine accepts no option bytes. Stop returns NRC `0x12`; unsupported
option data returns `0x13`. Devicetree `led1` selects the green channel on the
board, separately from the primary `led0`. Native simulation stores/logs both
states in memory. Application code registers handlers through `app_diag_register()`;
`diag_uds` has no dependency on the LED service or application demo.

Functional requests must be single-frame and fit seven UDS bytes. Functional
multi-frame requests are rejected without sending flow control. Supported
functional requests respond on the **physical response ID**, including segmented
read responses, with flow control on the physical request ID. RoutineControl is
physical-only and silent on functional addressing. NRCs `0x11`, `0x12`, `0x31`,
`0x7E` and `0x7F` are suppressed on functional addressing. Positive suppression is
handled by upstream. One outstanding tester request is supported; completed
unconsumed link data is retained and later overlapping data frames are counted
as busy drops. Request pipelining is not supported.

## Output, backpressure and errors

The adapter owns one full UDS response buffer. It accepts a copy from the server,
then submits it through bundled ISO-TP when CAN is available. A temporary initial
SF/FF submission failure is retried only until that request's P2 deadline.
Consecutive-frame backpressure maps to `ISOTP_RET_NOSPACE` so bundled `isotp-c`
retries on its next poll. Permanent submission failures are reported and abort
pending output. No unbounded queue or elaborate retry policy is added.

`isotp_user_send_can()` validates and forwards bytes through `can_if_send()`:
return 0 means accepted by the driver, `-EAGAIN` maps to `ISOTP_RET_NOSPACE`, other
failures to `ISOTP_RET_ERROR`. Completion is separate and counted by the driver
callback. The hook never reports success after failed submission.

The pinned library ignores its FlowControl send return value. For temporary
backpressure the adapter retains one failed FlowControl frame and retries it
from the worker until accepted or the 100 ms transport deadline expires. The
original failed hook still returns NOSPACE. RX consumption pauses during this
bounded retry so another flow-control request cannot overwrite it.

Malformed PCI/DLC, functional multi-frame traffic, queue drops, busy ingress,
transport failures, handler contract failures, TX submission/completion errors and
session timeouts are observable. Statistics are individually atomic, wrapping
snapshots. `suppressed` counts adapter-side NRC suppression; upstream suppression
of supported-service NRCs or positive responses is not included in that counter. Aborted physical sends become idle so a later valid tester exchange
can recover. This does not recover a native CAN driver that never issues TX
completion: `can_if` safely retains its slot until completion or process restart.
See [the CAN interface limitations](can-if.md).

## Dependency limitations

Upstream [OSS-Fuzz report #144](https://github.com/driftregion/iso14229/issues/144)
and [report #145](https://github.com/driftregion/iso14229/issues/145) are open as of
2026-10-23. Their public issue bodies link external reproducers without enough
detail to determine applicability to this adapter. Supported-service admission
and malformed-request tests do not establish that the dependency is free of
vulnerabilities. [Issue #117](https://github.com/driftregion/iso14229/issues/117)
concerns `0x38` file-transfer subfunctions, which this application rejects.
