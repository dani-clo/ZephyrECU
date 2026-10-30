# Phase 5 validation — 2026-10-30

## Scope and implementation plan

Read `instructions_parte2.md` and inspected the existing component modules,
startup, CAN/UDS priorities, anchored can_comm scheduling, and tests. Implemented
Phase 5 only, with this plan:

1. Add a standalone static Task Manager module with an explicit start boundary,
   one dedicated Zephyr thread, ISR-safe event posting and read-only statistics.
2. Define anchored scheduling, exact deadline boundaries, skip policy and stable
   simultaneous dispatch order; test production transitions with explicit ticks.
3. Integrate short CAN signal notification to application tasks without changing
   existing CAN/UDS thread ownership or the can_comm TX timing policy.
4. Run the full native CI, compile both project boards, document behavior and
   unverified hardware measurements, then stop for human review.

No Phase 6 API, flash partition, flash write, storage backend or persistence worker
was added. No commit, push, flash operation or hardware access was performed.
Pre-existing local instructions and validation reports were preserved.

## Delivered behavior

- `components/task_manager/`: Zephyr module/CMake/Kconfig, ROM iterable static
  descriptors, public `ecu_task_manager.h`, one dormant-until-start worker,
  atomic pending flags and semaphore notification/wait, coherent statistics.
- `ecu_task_id_t`: pointer to a registered constant descriptor. The definition
  macro exposes its named constant handle; a declaration macro shares it across
  files. Arbitrary/NULL IDs are rejected by pointer equality before dereference.
- Period 0 is event-only. First periodic release is start plus period. Both
  events and periods use one callback; coincident triggers invoke it once with
  accumulated bits. Zero mask is invalid; all 32 nonzero event bits are supported.
  Prestart posts are retained; posts during callback execution survive.
- Original phase is preserved. Due includes equality at dispatch. Older releases
  are skipped and the latest due release is represented once. Releases through
  callback completion, including equality, are skipped as well. Simultaneous
  tasks use Zephyr iterable-section lexical symbol order, documented explicitly.
- Per-task saturating uint64 counters expose invocations, missed periods and late
  invocations; lateness is relative to the latest represented periodic release,
  distinct from releases skipped and from callback duration.
- `app/src/app_tasks.c`: LED task at 100 ms and status-monitor task at 10 ms.
  Minimal can_comm signal callback posts a bit. LED processing obtains the latest
  synchronized signal sample, applies it, and periodically handles RX expiry.
  Monitor updates status caches; independent can_comm TX sends those PDUs.
- Main starts communication and application tasks explicitly after initialization
  and existing CAN/diagnostic startup. Existing diagnostic handlers and separate
  diagnostic LED retain their ownership.
- README, `docs/task-manager.md`, and testing guide document API, startup,
  timing/statistics, callback restrictions, event coalescing, priorities and stack
  measurement. No general lifecycle or scheduler abstraction was introduced.

These are concrete choices within the Phase 5 contract, not intended semantic
deviations. Nonblocking means callbacks must not intentionally wait or perform
long processing; existing synchronized signal getters hold only the bounded
can_comm cache mutex. There is no hard latency guarantee for mutex contention,
slow callbacks, or continuous event floods.

## Tests and build results

Final full run:

```sh
./tools/run_docker.sh
```

Docker image:
`sha256:1e8917c9fb26e5b7fa7d8a1a21fa86e132b71afc0d76e7038c7f6075d8b3edfc`.

| Check | Final observed result |
| --- | --- |
| Frozen manifest and Python package consistency | PASS |
| Generated DBC freshness | PASS |
| Four Python generation/codec tests | PASS |
| Twister native_sim | PASS, 4/4 configurations and cases, no warnings, 12.63 s |
| Task Manager contract test | PASS, 0.342 s simulated duration |
| Raw CAN SocketCAN integration | PASS |
| can_comm + concurrent UDS SocketCAN integration | PASS |
| Full UDS kernel ISO-TP integration | PASS, timing report mode |
| Application SocketCAN smoke | PASS, ready message and simulator exit |
| S32K148 EVB application build | PASS, no compiler warnings |
| Arduino UNO Q application build | PASS, no compiler warnings |
| Whitespace diff check | PASS |

Task Manager tests cover prestart inactivity despite posts, duplicate start,
ISR notification/start rejection, invalid/NULL IDs and output, periodic-only and
event-only invocation, high/all event bits, OR coalescing, notification from inside
a callback, shared thread identity and distinct periodic rates. They suspend the
manager while TX continues, inject an actual fake-driver IRQ, verify pending LED
notification without inline processing, resume the worker and verify LED on and
periodic expiry back to off. The production scheduling transitions are then
exercised at explicit ticks for exact boundaries, coincident triggers, preserved
phase, skipped releases, slow event callbacks, simultaneous order and saturation.
No alternate production clock or public stop/reset API was added.

The final UDS integration observed maximum first-frame latency 11.85 ms and
immediate-response latency 5.87 ms. These are Linux SocketCAN exerciser measurements,
not hardware results or measurements of worst-case application callback load.
The separate application smoke and fake-CAN Task Manager integration cover the
new startup and application tasks.

Board commands (workspace venv on PATH):

```sh
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR=/home/daniele/zephyr-sdk-1.0.1 \
  west build -b s32k148_evb . -d ../build/phase5-s32k148 \
  -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
ZEPHYR_SDK_INSTALL_DIR=/home/daniele/zephyr-sdk-1.0.1 \
  west build -b arduino_uno_q . -d ../build/phase5-uno-q \
  -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

| Board | Flash used | RAM used |
| --- | ---: | ---: |
| S32K148 EVB | 77,632 B | 44,160 B |
| Arduino UNO Q | 58,084 B | 41,352 B |

Both actual generated board configurations have UDS priority 4, CAN RX priority
5, can_comm TX priority 6 and Task Manager priority 7, with a 2048-byte Task Manager
stack. Lower numeric priorities are more urgent. Thus the manager does not preempt
these CAN/UDS threads at the delivered configuration.

## Resolved validation issues

The first standard Docker run stopped at its frozen-manifest consistency check:
`west.yml` already selected STM32/Infineon HAL modules but `west-frozen.yml` omitted
them. Regenerated only the frozen manifest to include the already selected and
checked-out HAL revisions. No existing dependency revision, manifest selection,
third-party source or package lock changed, and no dependency update was run.

An initial local test build found reversed codec arguments in the test injection;
corrected them. Docker's GCC 13 warnings-as-errors then caught a constant-zero
period bounds comparison in the registration macro. Replaced the check with a
64-bit upper-bound expression which also rejects negative periods. Final native
CI and both board rebuilds pass without these failures or compiler warnings.

## Hardware-only validation and checkpoint

Still **unverified**: physical boot and CAN traffic, callback/dispatch latency
under worst observed CAN/UDS load, thread stack high-water/headroom on either
board, and application behavior under target driver/logging contention. The stack
size is a documented starting budget, not a measured bound. Enable Zephyr stack
initialization/info and thread analyzer, exercise deepest callback/error paths,
measure under load, and adjust the stack with margin as described in the contract.
No hardware timing or memory-safety guarantee is inferred from native_sim or a
successful board link. Windows/GitHub Actions execution awaits a future push.

Full Docker results are in workspace `build/docker/`, including per-stage logs
and Twister JSON/XML/handler reports. Local task test artifacts are in workspace
`build/tests/task_manager/`; board artifacts are in `build/phase5-s32k148/` and
`build/phase5-uno-q/`. Session logs are `/tmp/zephyrecu-phase5-*`.

**STOP: Phase 5 is implemented, tested and documented. Request human review and
approval as mandated by `instructions_parte2.md` before any Phase 6 work.**

## Additional coverage measurement — 2026-10-30

Ran all four native_sim suites again with GitHub Actions coverage options
(`--coverage --coverage-tool gcovr --coverage-formats html,txt`), then applied the
same component-only filter, excluding generated code/tests. All suites passed
without warnings. Component totals: 725/794 lines (**91.3%**), 60/63 functions
(**95.2%**), and 484/628 branches (**77.1%**).

Task Manager: **66/66 lines, 9/9 functions, 52/58 branches (89.7%)**. Its six
uncovered branch outcomes are four Zephyr iterable-section assertion failure
paths, the no-periodic-tasks forever-wait path, and an outcome in Zephyr's
absolute-timeout macro. No exclusions were added. The application task example
has 16/16 measured lines covered. UDS contains most remaining uncovered lines.
Coverage measures native_sim instrumentation, not target GPIO behavior or the
separate SocketCAN/Python runs; 100% measured lines does not imply all build
configurations or races have been exercised.

Detailed findings and standalone HTML/JSON/text reports are in workspace
`build/phase5-coverage/README.md`, `build/phase5-coverage/components/`, and
`build/phase5-coverage/twister/`. No production code/test changes were made during
this coverage check. Phase 6 remains at the human approval checkpoint.
