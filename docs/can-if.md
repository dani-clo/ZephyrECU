# CAN interface contract

`components/can_if/include/can_if.h` is the public API. `can_if` owns
`DT_CHOSEN(zephyr_canbus)`, normal-mode configuration, controller start, RX hardware
filters, raw RX dispatch and raw TX. No client calls Zephyr CAN APIs.

## Lifecycle and routing

Call `can_if_init()`, register all RX consumers, then call `can_if_start()` from
thread context. Device readiness and driver errors are checked. Repeating a
successful init/start returns `-EALREADY`; a failed operation may be retried.
Concurrent lifecycle operations return `-EBUSY` rather than waiting. Successful
start freezes the registry; there is no runtime unregister, stop/restart or
software recovery API. The application is initialized once for
its entire process/boot lifetime.

`can_if_register_rx(id, extended, callback, context)` supports **exact** 11-bit or
29-bit identifiers. There is no wildcard mask API. Standard and extended frames
with the same numeric ID are distinct routes. Each distinct route installs one
full-mask hardware filter, using a shared ingress callback. Additional consumers
of that route are called in software, in registration order. Registering the
same callback/context twice returns `-EALREADY`; different contexts are allowed.
Registry exhaustion or per-route consumer exhaustion returns `-ENOSPC`. Failed
hardware filter installation consumes no registry entry. Contexts must remain
valid for the application's lifetime.

The implementation accepts Classical CAN **data** frames only. Invalid IDs,
DLC > 8, RTR, FD, BRS, ESI or unknown flags are rejected before clients are called.
Drivers may discard unsupported frame types before ingress; these cannot be
included in `can_if`'s invalid-frame count. Zero-length frames are valid.
Bitrate and controller selection remain Devicetree responsibilities; the board
keeps its upstream 125 kbit/s setting.

## RX context, memory and backpressure

The driver's callback, possibly in ISR context, copies the complete Zephyr frame
into a fixed `k_msgq` with `K_NO_WAIT`. It increments an atomic accepted/drop
counter. It does not decode, log, block, allocate or call application code.
On overflow the **new incoming frame is dropped**; queued frames retain FIFO
order. Overflow is observable through `can_if_get_stats()`, without logging each
frame in an ISR.

One dispatcher thread starts only after the controller successfully starts.
It blocks on an empty queue and otherwise drains continuously, validating and
routing each frame. It does not process just one frame per polling interval.
Decoded raw-frame metadata/data are copied into a zero-initialized
`can_if_frame_t`; bytes beyond DLC are zero. The callback borrows this object
only until it returns and must copy it to retain it. All consumers of the route
receive that frame exactly once. Callbacks run in dispatcher thread context,
must be brief and must offload slow work. The registry is immutable while the
thread runs, so dispatch needs no registry lock.

Default bounded resources (Kconfig configurable):

| Resource | Default |
| --- | --- |
| RX queue | 32 complete frames |
| Distinct routes / hardware filters | 8 |
| Consumers per route | 4 |
| Dispatcher stack | 2048 bytes |
| Dispatcher preemptive priority | 5 |
| Accepted outstanding TX frames | 1 |

The native driver RX thread uses upstream priority 2, so it can promptly enqueue
while the priority-5 dispatcher runs. Hardware ISR latency, stack high-water
marks and timing remain on-target validation tasks. Slow callbacks can cause
queue drops; the SocketCAN test deliberately demonstrates this.

## TX submission and completion

`can_if_send(frame, callback, context)` is thread-only. It validates the ID/DLC
and copies the frame into one retained static TX slot. It serializes driver
submission with a nonblocking mutex and calls `can_send()` with `K_NO_WAIT` and
an internal completion callback. It never uses Zephyr's synchronous NULL-callback
path, which waits indefinitely for completion.

- Return **0** means **driver submission succeeded**, not bus transmission.
- An outstanding TX or concurrent submission returns `-EAGAIN`, with no internal
  queue or retry. Callers choose retry/coalescing policies. Rejected attempts
  have no ordering guarantee; successfully submitted frames cannot overtake one
  another because only one is outstanding.
- A driver submission failure is returned unchanged and releases the slot.
  No client callback follows a rejected submission.
- Successful completion increments `tx_completed`; failed completion increments
  `tx_completion_errors` and records the error. The optional client callback
  receives the driver status unchanged. It may run in ISR context, may run
  before `send()` returns, and must not block or call `send()` recursively.
- The slot stays occupied until the client completion callback returns. The
  caller may reuse its frame after `send()` returns. Callback context must remain
  valid until completion returns. Use a long-lived context where completion can
  be absent due to driver/controller failure.
- No caller waits for a mailbox, a mutex or a completion. There is no speculative
  timeout-based reuse of the slot: a late driver completion must never reference
  a reused payload/context. If a driver never reports completion, `tx_pending`
  stays true and further sends return `-EAGAIN` until restart. This is observable,
  but automatic recovery is out of scope.

State-change callbacks only update atomic state, bus-off event count and driver
RX/TX error counts. Initial state is sampled after start. Driver submission and
completion errors are counted separately. `last_tx_error` retains the most recent
error and is not erased by success. No logging or application callbacks execute
in the driver's state callback. FlexCAN bus-off recovery policy stays with the
upstream normal-mode driver; no custom automatic retry/restart policy is added.

`can_if_get_stats()` is available in thread/ISR context. Fields are independently
atomic snapshots, not a transactional view across all fields; counters wrap at
32 bits. `rx_queued` counts successful ingress copies; `rx_dispatched` counts
valid routed frames after all callbacks return; `rx_callbacks` counts consumer
calls. `rx_unmatched` is a defensive software routing count, normally zero with
exact hardware filters. A source already dropped by hardware never reaches these
counters. `tx_busy` counts local contention separately from driver rejections.

## Native driver limitations

Pinned Zephyr v4.4.2's native driver does not implement state-change notifications
and reports error-active/zero error counts while started. It cannot validate
physical bus-off or electrical errors. Unit tests inject those events through
Zephyr's fake driver; physical verification requires the board.

In `drivers/can/can_native_linux.c`, `can_native_linux_send()` logs a failed host
write but returns 0 and can leave its TX semaphore occupied without completion.
Consequently, interface-down/write failures may appear as submission success
followed by a permanently pending TX. `can_if` faithfully returns the driver's
result, reports actual completions separately, and does not claim this is a
confirmed transmission. Upstream sources are unmodified. Keep `vcan0` up for
integration testing; restart the simulator after such a host interface failure.
