# Application Task Manager

`task_manager` executes short application callbacks sequentially on one dedicated
Zephyr thread. Enable `CONFIG_ECU_TASK_MANAGER`, add the component to
`EXTRA_ZEPHYR_MODULES`, and include `ecu_task_manager.h`. Existing component/SWC
threads and the can_comm TX scheduler retain their own scheduling and ownership.
There is no dynamic registration, allocation, per-task priority, stop API, or
shutdown lifecycle.

## Registration and startup

```c
static void monitor(uint32_t events) { /* short application processing */ }
ECU_TASK_DEFINE(monitor_task, monitor, 10);
/* In another file/header: ECU_TASK_DECLARE(monitor_task); */
```

`ecu_task_id_t` is `const struct ecu_task_descriptor *`, an opaque descriptor
handle. `ECU_TASK_DEFINE` emits a static descriptor, runtime storage, and the
public constant handle named by its first argument. Define each name once across
the application; callbacks must be non-NULL and periods compile-time constants
in milliseconds in [0, UINT32_MAX]. The macro's implementation structures are
visible for static storage but are private to the component; do not inspect,
modify or fabricate them. No task-count setting or dynamic registry is needed.
Zephyr's ROM iterable section retains descriptors and sorts them lexically by
symbol name. This is dispatch order, including simultaneous deadlines; it is
independent of C source order and applies across translation units.

Period 0 means event-only. Other periods are rounded up to whole Zephyr ticks.
The first periodic release is one period after the common start timestamp.
Callbacks never run during module initialization or registration. Application
startup initializes infrastructure and SWCs, registers CAN callbacks, starts
CAN/UDS/can_comm, then calls `ecu_task_manager_start()`. The demo implements this
order in `src/main.c`. Future required NVM initialization/configuration loads
would precede task start; NVM is not implemented by this component.

Start is thread-context only (`-EWOULDBLOCK` in ISR). Exactly one caller succeeds;
subsequent/concurrent calls return `-EALREADY`. Callbacks may execute immediately
once start releases the thread, possibly before start returns. Application data
must already be ready. Zero registered tasks is valid; the worker sleeps forever.

## Events and execution

`ecu_task_notify(task, mask)` works in ISR and thread context and never calls
application code or waits. It validates the pointer by equality against registered
descriptors **before dereferencing it**; NULL/unregistered handles and zero masks
return `-EINVAL`. Otherwise it atomically ORs all 32 mask bits and gives a bounded
Zephyr semaphore. Validation costs O(number of static tasks); keep registration
counts appropriate for ISR use. A semaphore token prevents a lost wake-up between
the worker's scan and wait. The worker waits for an event or the earliest absolute
periodic deadline using Zephyr timeout primitives.

Notifications before start remain pending and are delivered after start. Repeated
bits coalesce; flags are not a message queue and do not count posts. Pending bits
are atomically exchanged with zero before user code, so posts during a callback
remain available for the next scan. There is no per-SWC event wait API.

The same callback handles both trigger types:

| At the task's dispatch check | Result |
| --- | --- |
| Periodic release only | One invocation with `events == 0` |
| Pending events only | One invocation with accumulated nonzero bits |
| Both | One invocation with accumulated nonzero bits |
| Neither | No invocation |

Coincidence means both triggers are visible at that task's check; a post racing
after the atomic exchange belongs to a later invocation. An early event does not
move the periodic phase. A callback never runs concurrently with another manager
callback. Short nonblocking callbacks are required: no sleeps, waiting on queues,
flash writes/deletes, or long processing. A slow callback delays all tasks on the
thread. Event floods can keep it busy; there is no admission control or guarantee
of latency for individual SWCs. Components needing payload queues or blocking work
should own Zephyr queues/threads. Shared getters in the demo use can_comm's short
cache mutex, whose holders must remain bounded; this is not a hard latency bound.

## Anchored periods and diagnostics

Each task keeps an absolute next release in Zephyr uptime ticks. At dispatch,
`now >= next` is due. When several releases have passed, the latest release at
or before `now` is represented by one invocation; older releases are skipped.
Before running the callback the next deadline advances to the first release
strictly after `now`. After the callback, **all** remaining releases at or before
completion are skipped, including one exactly equal to completion. This also
applies when an event invocation spans a periodic release. No release is replayed
because a callback took too long, and the original phase is preserved.

For example, a 10 ms task due at 10 runs there and finishes at 27: release 20 is
missed and its next release is 30. If first dispatched at 35, releases 10/20 are
missed, release 30 is represented by a late invocation, and next is 40. Finishing
at exactly 40 skips that release and advances to 50. Each scan visits each task
at most once and uses fresh time for each task, so a long callback cannot produce
a catch-up burst. Equal deadlines follow lexical registration order; if an
earlier callback is slow, later callbacks see the elapsed time and skip releases.

`ecu_task_get_stats(task, &stats)` returns a coherent, read-only snapshot under a
short Zephyr spinlock, from ISR or thread context. Invalid IDs or NULL output
return `-EINVAL`. Statistics are zero before start and never reset:

- `invocations`: callbacks selected for execution, counted before entry.
- `missed_periods`: periodic releases skipped before dispatch and through callback
  completion. An invocation that is merely late does not itself count as missed.
- `late_invocations`: periodic invocations dispatched strictly after their latest
  represented release. Exact equality is on time; event-only invocations are not
  late. This measures dispatch lateness, not callback runtime.

All counters saturate at `UINT64_MAX`. Queries during a callback include its
invocation but may not yet include releases skipped at its completion.

## Demo, priorities and stack

`app/src/app_tasks.c` registers a 100 ms LED task and a 10 ms monitor task.
The can_comm signal callback only posts `ECU_EVENT_CAN_RX`; it does not retain the
borrowed signal sample. The LED task reads a synchronized current sample with
`can_comm_get_signal()` and applies valid LED requests. Its periodic checks turn
the LED off after RX expiry. Coalescing intentionally processes the latest state.
The monitor updates generated status cache fields; independent can_comm periodic
TX publishes them. Existing diagnostic LED control remains on its separate GPIO.

Default preemptive priorities (smaller number is more urgent) are:

| Thread | Kconfig | Default |
| --- | --- | --- |
| UDS worker | `DIAG_UDS_PRIORITY` | 4 |
| CAN RX dispatcher | `CAN_IF_DISPATCH_PRIORITY` | 5 |
| CAN TX scheduler | `CAN_COMM_PRIORITY` | 6 |
| Application tasks | `ECU_TASK_MANAGER_PRIORITY` | 7 |

These are the existing board application defaults; board configs should be
checked again whenever priorities change. Application callbacks are preemptible
by CAN/UDS at these priorities. They must not hold locks needed by those threads
for long or disable interrupts. No numeric choice alone proves hardware timing.

`CONFIG_ECU_TASK_MANAGER_STACK_SIZE` defaults to 2048 bytes. Budget for the
**deepest callback call chain**, including getters, drivers, logging and error
paths, plus preemption/architecture overhead; sequential callbacks do not require
the sum of all callback stacks. This default is a starting budget, not a measured
hardware bound. On each board enable `CONFIG_INIT_STACKS`,
`CONFIG_THREAD_STACK_INFO`, and the Zephyr thread analyzer in a diagnostic build.
Exercise every callback and error path under maximum CAN/UDS load and inspect the
manager's maximum stack use with `thread_analyzer_print()` / thread analyzer
reporting. Adjust stack size with margin and repeat on the release configuration;
add instrumentation for callback start/finish and CAN/UDS latency as appropriate.
Target runtime headroom and scheduling latency still require hardware validation.

## Verification

`tests/task_manager` runs on `native_sim`, including real Zephyr threads, ISR
offload, the fake CAN driver and the application SWCs. It checks startup, invalid
handles, all event bits, prestart coalescing, notification during dispatch, shared
thread identity, distinct periodic rates and real IRQ CAN ingress to deferred LED
processing while can_comm TX continues. Timing assertions use broad bounded waits.
It then suspends only the manager thread and drives the production scheduling
transitions with explicit absolute ticks to verify boundaries, missed releases,
phase preservation, simultaneous order and counter saturation without host timing
assumptions. This private test access is not a public lifecycle/time API.

Run all suites using the existing Twister command in [Testing](testing.md), or
`./tools/run_docker.sh` for CAN/UDS integration and application smoke coverage.
