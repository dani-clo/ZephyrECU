# Testing

Unit tests, SocketCAN integration tests, manual checks and GitHub Actions
for ZephyrECU. See the [README](../README.md) for cloning and firmware builds.

## Virtual test environment

For a Docker-based setup on Linux x86_64, run from this repository:

```sh
./tools/run_docker.sh
```

The launcher builds the `zephyrecu-ci` image, checks DBC generation, runs all native
Twister suites, raw CAN/can_comm/UDS integration tests and an application smoke test. It creates `vcan0`
inside the container and writes results to the workspace's `build/docker/`.
Use `./tools/run_docker.sh --unit-only` to run without virtual CAN support.
Only Docker is needed in userspace; integration tests also require the Docker
host kernel to support vcan and CAN ISO-TP. See [container CI](container-ci.md)
for setup, options, dependency pinning and limitations.

For a native setup, follow the instructions below.

The virtual tests run on Linux with Zephyr `native_sim` and SocketCAN. No ECU
hardware is required. Use Python >= 3.12 and a host GCC installation with 32-bit
development support for the default `native_sim` target.

On Debian, install the prerequisites:

```sh
sudo apt install git python3-venv python3-dev cmake ninja-build gperf \
  device-tree-compiler build-essential gcc-multilib g++-multilib \
  iproute2 kmod can-utils
```

Keep this repository and its west-managed dependencies in a workspace:

```text
workspace/
  .venv/
  .west/
  ZephyrECU/
  zephyr/
  modules/
  build/
```

Clone the repository as described in the [README](../README.md#clone-and-setup).
Then run these commands from the workspace root:

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install west
west init -l ZephyrECU
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt \
  -r zephyr/scripts/requirements-build-test.txt \
  -r zephyr/scripts/requirements-run-test.txt \
  -r ZephyrECU/tools/requirements-codegen.txt
west manifest --validate
```

`west.yml` selects Zephyr v4.4.2 and a fixed iso14229 commit; `west-frozen.yml`
records the resolved dependency SHAs. Run all commands below from the workspace
root, with the virtual environment active.

## Unit tests

Run the CAN interface, communication and diagnostic contracts through Twister:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host python zephyr/scripts/twister \
  -T ZephyrECU/tests -p native_sim \
  --outdir build/twister-contracts --inline-logs
```

These tests use Zephyr's fake CAN driver and require no virtual CAN interface.
They check filter sharing and capacity, frame validation, ISR/thread context,
buffer ownership, queue overflow, TX errors, bus-off notifications, diagnostic
registration, handler NRCs, backpressure and absolute-time wraparound. Twister
writes results under `build/twister-contracts/`. Communication tests also verify
signal snapshots, bounds, validity expiry, all three TX modes, coalescing,
min-delay, periodic phase anchoring and submission/completion errors.

Check the committed DBC output and independent codec oracle from the workspace:

```sh
python ZephyrECU/tools/dbc_codegen.py --check
python -m unittest discover -s ZephyrECU/tests/codegen -v
```

## SocketCAN integration tests

Create and bring up `vcan0` once per host session:

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan  # omit if already present
sudo ip link set dev vcan0 up
ip -details link show vcan0
```

Build the raw CAN exerciser and run the host tester:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west build -b native_sim -S socketcan-native-sim \
  ZephyrECU/tests/can_if_socketcan -d build/tests/can_if_socketcan \
  -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python ZephyrECU/tools/test_can_if_socketcan.py \
  build/tests/can_if_socketcan/zephyr/zephyr.exe --interface vcan0
```

The tester starts and stops its own ECU process. It exercises the real CAN
interface and native SocketCAN driver: standard/extended filtering, exactly-once
fanout, payloads, ordered TX, completion accounting, rejection and RX overflow.
A successful run prints `PASS` and a final statistics line with zero TX errors
and no pending TX. Do not run another exerciser on the same interface concurrently.

Where unprivileged user/network namespaces are permitted and the vcan driver is
available, the same test can run with an isolated interface:

```sh
unshare --user --map-root-user --net sh -c '
  ip link add dev vcan0 type vcan &&
  ip link set dev vcan0 up &&
  exec .venv/bin/python ZephyrECU/tools/test_can_if_socketcan.py \
    build/tests/can_if_socketcan/zephyr/zephyr.exe
'
```

Any CAN tester or capture tool must run in the same network namespace as the ECU.

## Application PDU integration test

With `vcan0` up and other exercisers stopped, run:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west build -b native_sim -S socketcan-native-sim \
  ZephyrECU/tests/can_comm_socketcan -d build/tests/can_comm_socketcan
python ZephyrECU/tools/test_can_comm_socketcan.py \
  build/tests/can_comm_socketcan/zephyr/zephyr.exe --interface vcan0
```

The dedicated harness exercises the generated IDs and bytes, signed and extended
RX, PDU callbacks independently of raw signal changes, periodic/event/mixed TX,
invalid payload rejection and timeout/revalidation. It also exchanges UDS
TesterPresent on the same controller using raw CAN, without kernel ISO-TP.
It does not drive GPIO or install application SWCs. See the
[communication contract](can-comm.md) for the DBC and runtime APIs.

## Manual CAN test

With the automated tester stopped and host `vcan0` up, launch the exerciser:

```sh
./build/tests/can_if_socketcan/zephyr/zephyr.exe --can-if=vcan0 -rt
```

In a second terminal, capture traffic:

```sh
candump vcan0
```

In a third terminal, send a request:

```sh
cansend vcan0 123#01020304
```

The response is `321#01020304`. Standard ID `0x123` has two consumers sharing one
RX filter. Extended ID `0x123` responds on extended `0x1ABCDE`. ID `0x124`
deliberately blocks the test dispatcher for 200 ms to exercise overflow. These
raw test behaviors belong to the exerciser, not the main application.

## UDS integration test

The UDS test requires Linux kernel ISO-TP support (`CONFIG_CAN_ISOTP`) and Python's
`socket.CAN_ISOTP`. The ECU itself uses bundled `isotp-c`, with Zephyr ISO-TP
disabled. With `vcan0` up, build the real-stack diagnostic exerciser and run:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west build -b native_sim -S socketcan-native-sim \
  ZephyrECU/tests/diag_uds_socketcan -d build/tests/diag_uds_socketcan \
  -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python ZephyrECU/tools/test_uds_socketcan.py \
  build/tests/diag_uds_socketcan/zephyr/zephyr.exe --interface vcan0
```

This test takes about 15 seconds, including real session timeout waits. It checks
session control, TesterPresent, read-only DIDs, a benign LED routine, NRCs,
functional addressing, requests and responses spanning multiple CAN frames,
malformed traffic, transport timeout recovery and concurrent raw application CAN.
It checks first-response P2 against 50 ms, verifies consecutive immediate
responses within 10 ms, and reports the largest worker poll gap.
The default `--timing strict` enforces the host latency and worker-gap thresholds.
For shared CI runners, pass `--timing report`: latency observations are printed
without failing the functional test. Protocol responses, NRCs, payloads, traffic
ordering, queue/error counters and session behavior are still checked. The
simulated-time pacing regression remains mandatory in ztest.

The exerciser starts/stops automatically; use an isolated `unshare` shell as in
the raw CAN test if desired. Stop other ECU processes using the same IDs first.

## Application smoke test

Build and run the application with its in-memory LED backend:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west build -b native_sim -S socketcan-native-sim \
  ZephyrECU -d build/native_sim -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
./build/native_sim/zephyr/zephyr.exe --can-if=vcan0 -rt -stop_at=1
```

Expected logs include `LED initialized: off (memory backend)` and
`ZephyrECU CAN and diagnostics ready`. The application initializes diagnostic
routes and demonstration handlers without raw echo routes. The application PDU
scheduler is exercised by its separate integration harness. Zephyr's
`CONFIG_ISOTP` remains disabled.

To try diagnostics manually, omit `-stop_at=1` and run `candump vcan0` in another
terminal. Send `cansend vcan0 7E0#023E00` for TesterPresent; expect
`7E8#027E00`. Send `cansend vcan0 7E0#043101F001` to toggle the diagnostic LED;
the positive response ends in its state byte. Physical request/response IDs are
`0x7E0`/`0x7E8`, functional request ID `0x7DF`. DID `0xF190` returns the demo
identity and DID `0xF186` returns the session byte; use an ISO-TP tester to receive
segmented identity responses. Full DID/RID contracts are in the
[diagnostic design](diag-uds.md).

The native argument parser requires `--can-if=vcan0`, with `=`; its default
interface is `zcan0`. Keep the selected interface up. The pinned native driver
can leave TX pending after a host write failure; restart the simulator if that
occurs. Physical bus-off and electrical behavior require hardware testing.

## GitHub Actions

### Windows firmware build

The `windows-build` job in `.github/workflows/ci.yml` cross-compiles the
application on `windows-2022` for two boards in independent matrix jobs:
`s32k148_evb` (NXP, the reference target) and `arduino_uno_q`.
Both use Python 3.12, Ninja and the ARM GNU toolchain from the Zephyr SDK. The SDK version comes from the pinned
Zephyr checkout's `SDK_VERSION`. Git long-path support is enabled before checkout,
and the SDK uses a short installation path (`C:/zephyr-sdk`).

The job validates the west manifest and checks committed DBC output before
building. Each board has its own artifact (`windows-s32k148_evb` or
`windows-arduino_uno_q`) containing the firmware
(`zephyr.elf`, `zephyr.bin`, `zephyr.hex`), generated configuration/devicetree and
available build logs, including on failure. It checks compilation on Windows;
flashing and hardware behavior require a connected board.
Matrix fail-fast is disabled so a failure on one board does not cancel the other.

`native_sim`, SocketCAN/vcan and kernel ISO-TP tests run in the Linux job.
The codec oracle test also remains there: it currently invokes host GCC to build
a shared library using Unix flags. The Windows job only runs the portable DBC
generation check.

The repository's `.gitattributes` preserves LF line endings in generated CAN
files on Windows too, since the DBC freshness check compares exact bytes.

### Linux tests

Twister runs with coverage enabled. The `native-sim-component-coverage` artifact
contains `index.html` with per-file HTML details and `coverage.txt` with the
line coverage table. Download and extract the entire artifact to view the HTML
report with its supporting files. The CI log also reports total line, function
and branch coverage.

These reports include only `components/`, excluding tests and generated code.
They measure the code exercised by the Twister suites; the separate SocketCAN
integration tests and Python codec tests are not included. Raw Twister coverage
data remains available in `native-sim-test-results` under `twister-contracts/`.

After all Linux and Windows jobs succeed on `main`, the `publish-coverage` job
publishes the report to <https://dani-clo.github.io/ZephyrECU/> using GitHub Pages.
The README badge shows line coverage from the same filtered report and links to
its HTML index. `coverage.txt` and `summary.json` are published alongside it.
Pull requests only generate artifacts; they do not update the public report.

The workflow in `.github/workflows/ci.yml` checks DBC generation and codec tests,
runs unit tests, raw CAN/can_comm and UDS integration tests, and the application
smoke test on `ubuntu-24.04` for pushes
to `main` and pull requests. It also supports manual dispatch once registered on
the default branch. Workflow changes and the test scripts must be committed and
pushed before GitHub can execute them.

The runner sets up `vcan0` with sudo. If the vcan module is missing, it installs
`linux-modules-extra` for the running kernel, then verifies that the normal test
user can bind a raw SocketCAN socket. This support is required for the CAN
integration and application smoke tests.

A separate socket probe checks kernel ISO-TP support, including support built
directly into the kernel. If the protocol is unavailable, only execution of the
UDS integration tester is skipped, with a warning, probe logs and a job summary
entry. The UDS exerciser is still built, and all unit tests, raw CAN/can_comm
integration and application smoke tests remain required. Unexpected probe errors
and failures in enabled tests fail the job. The local Docker runner still
requires ISO-TP support for its full test run.

UDS host timing uses report mode; functional assertions and deterministic ztest
timing checks remain enabled.
The smoke test requires the application readiness log, since returning an error
from Zephyr main does not necessarily make the simulator process fail.

The `native-sim-test-results` artifact includes Twister reports and build/runtime
logs from `build/logs/`, including failures. A successful local run does not verify
module availability or package installation on the GitHub runner image.
