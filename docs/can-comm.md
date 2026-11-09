# Application CAN communication

`can_comm` owns application PDU/signal state and TX scheduling. All CAN filters,
driver callbacks, controller start and raw sends remain in `can_if`. Diagnostics
continue to use `diag_uds` independently on the same controller.

## Configuration and generation

`dbc/automotive.dbc` owns CAN IDs, frame type, DLC, sender/receivers, signal layout,
signedness, scale, offset and physical bounds. DBC attributes can supply timing,
TX modes and signal triggers. An optional YAML policy selects the local node and
overrides those values, keyed by DBC names. TX direction follows the DBC sender;
all signals of an RX PDU must name the local node as a receiver. The demo retains
its full `dbc/can_policy.yaml` and generates the same C as before.

| PDU | CAN ID | DLC | Behavior |
| --- | --- | --- | --- |
| LedCommand | standard `0x120` | 4 | RX, 60 ms timeout |
| VehicleFeedback | extended `0x18FF0101` | 8 | RX, 100 ms timeout |
| StatusPeriodic | standard `0x220` | 2 | PERIODIC, 40 ms |
| StatusEvent | standard `0x221` | 3 | EVENT, 15 ms min-delay |
| StatusMixed | standard `0x222` | 3 | MIXED, 100 ms, 20 ms min-delay |

LedCommand contains `LedRequest` and Motorola signed `Demand`. VehicleFeedback
contains an Intel unsigned 32-bit `Counter` and Motorola signed `Temperature`.
The DBC is authoritative for their layout and scaling. Generated C names include
both PDU and signal names to disambiguate repeated names such as `Counter`.

Install the pinned Python tools and regenerate from the repository:

```sh
python -m pip install -r tools/requirements-codegen.txt
python tools/dbc_codegen.py
python tools/dbc_codegen.py --check
python -m unittest discover -s tests/codegen -v
```

The generator uses cantools 44.0.0 for strict DBC parsing. Its four outputs are
`components/can_comm/generated/can_cfg.[ch]` and `can_signals.[ch]`. Keep generated
sources in Git with the inputs. Ordinary CMake builds compile these sources and
do not require cantools. CI and `tools/run_ci.sh` check freshness and run the host
codec tests before Twister. `--check` compares bytes without modifying files;
output contains no timestamp, machine path or random data.

Supported subset: Classical CAN with DLC 1..8, standard/extended IDs, one sender,
1..16 non-overlapping integer signals per PDU, signal sizes 1..32 bits, Intel and
Motorola layouts, signed/unsigned values, positive finite scale, finite offset
and explicit finite physical bounds. At most 32 PDUs and 128 signals are allowed.
Unspecified initial raw values use zero clamped into the allowed raw interval;
explicit DBC initial values must fit. Unused TX bits are zero.

Multiplexing, CAN FD, container PDUs, floating-point signals, non-positive scales,
signals wider than 32 bits, invalid-value markers and ambiguous mappings are
rejected. Duplicate/unknown policy keys, malformed values and missing required RX timing
are errors. Policies may override only a subset of PDUs, fields and signals.
DBC choice values remain numeric raw values in C. The supported subset is
deliberately bounded; unsupported constructs are not approximated silently.

## DBC-only configuration and YAML overrides

For a custom DBC, select the local node explicitly and optionally supply YAML:

```sh
python tools/dbc_codegen.py --dbc network.dbc --node ECU --out generated
python tools/dbc_codegen.py --dbc network.dbc --node ECU --policy overrides.yml --out generated
python tools/dbc_codegen.py --dbc network.dbc --node ECU --out generated --check
```

The Python API is `generate(dbc, policy=None, node=None)` (and equivalently
`model`). No policy is discovered by the Python API or when CLI `--dbc` is
supplied, even if that path names the demo DBC. For backward compatibility only,
the CLI without `--dbc` selects `dbc/automotive.dbc` and, when present, the demo's
`dbc/can_policy.yaml`. An explicitly requested policy must exist.

Use `--node NAME` or YAML `node: NAME`. If both are specified they must agree.
Automatic selection is permitted only when `BU_` declares exactly one node;
zero or multiple declared nodes require an explicit choice. The selected node
must be declared, even if it is named by a sender/receiver elsewhere in the DBC.

For each field, precedence is **explicit YAML > explicit DBC attribute > DBC
attribute default (`BA_DEF_DEF_`) > generator default**. An override replaces
only its own field: switching a cyclic message to EVENT also requires setting
its nonzero period to zero. Invalid or null YAML values are rejected; they do
not fall back to the DBC. A valid YAML override can replace an unsupported DBC
value, provided cantools can parse the DBC in the first place.

| Setting | DBC attribute (scope, type) | YAML field | Generator default |
| --- | --- | --- | --- |
| TX period | `GenMsgCycleTime` (`BO_`, `INT`) | `period_ms` | 0 |
| TX mode | `GenMsgSendType` (`BO_`, `ENUM`) | `mode` | PERIODIC if resolved period > 0, otherwise EVENT |
| TX minimum delay | `GenMsgDelayTime` (`BO_`, `INT`) | `min_delay_ms` | 0 |
| RX timeout | `ZephyrECU_RxTimeoutMs` (`BO_`, `INT`) | `timeout_ms` | Required; no inferred timeout |
| TX signal trigger | `ZephyrECU_SignalTrigger` (`SG_`, `ENUM`) | `signals.<signal>` | PENDING |

`GenMsgCycleTime` is widely used; the other names and the following enum mapping
are the **ZephyrECU convention**, not a promise of compatibility with every DBC
tool. `GenMsgSendType` labels map as follows: `Cyclic` → PERIODIC, `OnWrite` →
EVENT, `CyclicAndOnWrite` → MIXED. Enum indices are resolved through their DBC
definitions, so reordered labels work. Signal trigger labels must be `PENDING`
or `TRIGGERED_ON_CHANGE`. Other labels, types and invalid indices are rejected
when used. TX attributes are ignored on RX PDUs; RX timeout attributes are
ignored on TX PDUs. Unrelated DBC attributes are unaffected.

All times are integer milliseconds bounded by 2147483647. PERIODIC/MIXED require
a positive period, EVENT requires period 0, minimum delay allows zero, and RX
timeout must be positive. No default RX timeout is chosen because it determines
when received data becomes invalid. With no TX timing attributes/overrides, the
EVENT/PENDING defaults send only on explicit `can_comm_trigger_tx()` requests;
they create neither periodic traffic nor automatic sends on signal writes.

A partial override can be as small as:

```yaml
node: ECU
pdus:
  ECU_Periodic:
    period_ms: 75
    signals:
      Counter: TRIGGERED_ON_CHANGE
  Tester_Input:
    timeout_ms: 600
```

Root keys are limited to `node` and `pdus`. TX PDU keys are `mode`, `period_ms`,
`min_delay_ms`, `signals`; RX PDU keys are `timeout_ms`. Unknown PDU/signal names
and fields, duplicate keys, and non-mapping policies are errors. Missing PDU or
signal overrides are allowed; empty mappings preserve DBC/default values.

Runnable DBC-only examples live in
[`tests/codegen/fixtures/dbc_only`](../tests/codegen/fixtures/dbc_only). Tests
compile and round-trip both examples against cantools, alongside partial YAML,
attribute defaults, invalid configurations, node selection and CLI checks.

## Public API and lifecycle

Include `can_comm.h` for generic access and `can_signals.h` for generated typed
raw accessors. Initialize in this order:

```c
can_if_init();
can_comm_init();
/* Register can_comm callbacks and diagnostic handlers/routes here. */
can_if_start();
can_comm_start();
```

Check every result. `can_comm_init()` can resume after a route registration
failure without duplicating successful subscriptions. Lifecycle is one-shot;
there is no stop/reset/unregister operation. Registration freezes when the
communication worker starts. Duplicate callbacks return `-EALREADY`, exhausted
fixed registries return `-ENOSPC`, and registration after start returns `-EBUSY`.
Callbacks and contexts must remain alive for the application lifetime.

All public calls are thread-only; ISR calls return `-EWOULDBLOCK`. Initialization
must precede getters/setters. Invalid IDs/pointers return `-EINVAL`. TX-only
operations on RX data and explicit triggers on PERIODIC PDUs return `-EACCES`.
Raw setters reject values outside the DBC bit/physical bounds with `-ERANGE`.

`can_comm_get_signal()` returns `can_comm_sample_t`: raw integer, physical value
(`raw * scale + offset`), `received`, `valid`, and RX age in milliseconds. The
setter is explicitly named `can_comm_set_signal_raw()` and accepts an unscaled
integer. Generated setters/getters use the smallest suitable signed/unsigned
8/16/32-bit C type, for example:

```c
can_signal_status_event_event_value_set_raw(-123);
can_signal_led_command_led_request_get_raw(&raw, &valid);
```

TX samples describe the initialized cache and are valid before transmission;
they do not confirm bus delivery. RX samples are invalid before first reception
and at age >= timeout. A getter still succeeds for stale data and retains the
last value: callers must inspect validity. Age starts when the dispatcher accepts
the PDU; the raw frame interface carries no hardware RX timestamp. Invalid PDUs
do not refresh the age.
There is no asynchronous timeout callback; consumers needing a stale-data action
must check validity at a suitable interval. Revalidation alone does not count as
a raw signal change.

## RX notifications and synchronization

`can_if` copies driver frames into its bounded queue and invokes the registered
RX handler in its single dispatcher thread. `can_comm` requires exact DLC and
valid raw values for every signal. A malformed PDU is rejected as a whole.
Decode completes into temporary storage before the entire cache is updated under
one mutex. No client callback or driver send runs with that mutex held.

PDU callbacks run for every accepted reception, followed by signal callbacks for
raw values that changed. First reception counts as a change even if the value is
zero. A borrowed PDU snapshot contains the original bytes and raw values in the
generated contiguous signal order (`first_signal`, `signal_count`). It remains
immutable throughout the callback. Signal callbacks receive their own value
snapshot. Other threads may use getters and TX setters concurrently; separate
getter calls do not constitute one multi-signal transaction.

Callbacks may use public getters/setters/trigger operations but must stay brief:
they share the CAN dispatcher with diagnostic enqueueing. Long callbacks can
overflow the common CAN queue. That queue's limits/drop counters are documented
in [can-if.md](can-if.md). There is no additional communication RX queue.

## TX scheduler and errors

One suspended-at-boot thread starts through `can_comm_start()`, requests a 1 ms
sleep between passes, and uses absolute 64-bit uptime deadlines. The component
requires at least 1000 ticks/second. Default priority is 6, below the CAN dispatcher
(5) and diagnostics (4), with a 2048-byte stack. Actual dispatch latency depends
on RTOS ticks, other work and the host when using native_sim.

- PERIODIC: setters update cache; only periodic deadlines request sends.
- EVENT: explicit `can_comm_trigger_tx()` or a changed TRIGGERED_ON_CHANGE signal
  requests the whole PDU. PENDING writes and unchanged values never request TX.
- MIXED: periodic deadlines and events share one send path. Events never move
  the next periodic deadline. Simultaneously due requests produce one frame.

Event requests coalesce into one flag, and transmission copies a coherent newest
PDU snapshot. Events arriving during submission or in-flight TX remain pending
for a later send. There is at most one in-flight send per PDU. The common `can_if`
TX slot still arbitrates all application and diagnostic traffic.

Minimum delay starts at the last submission attempt returning anything other
than `-EAGAIN`; it applies to both periodic and event sends. A minimum delay can
defer a periodic send, but the next deadline stays on its original grid. Missed
periods coalesce; the scheduler skips catch-up bursts. PDU traversal rotates to
avoid favoring the first configured PDU when the shared TX slot is busy.

`-EAGAIN` retains the outstanding event/periodic request for a later pass using
the latest data. Other submission errors consume that attempt and are counted;
only the next periodic deadline or a fresh event can retry. Completion errors
are counted separately and do not replay stale data. Driver completion callbacks
only publish atomic status to the worker, including when called inline or in ISR
context. No blocking work runs there.

`can_comm_get_stats()` provides a mutex-consistent snapshot of wrapping 32-bit
RX accepted/invalid/timeout counters and TX submitted/completed/submission-error/
completion-error/busy counters. A missing driver completion leaves the PDU in
flight; there is no speculative retry that might duplicate a physical send.
The pinned native driver's missing-completion limitation remains applicable.

The main application currently starts CAN/UDS and diagnostic demo handlers.
The dedicated communication exerciser tests this component without application
SWCs or GPIO behavior. Native tests do not establish hardware CAN electrical
behavior, target scheduling latency or stack high-water margins.
