# ZephyrECU

Lightweight automotive ECU framework on Zephyr RTOS, without AUTOSAR.
The CAN interface provides exact RX filtering, software fanout, bounded RX dispatch,
nonblocking raw TX and error counters. See the [CAN interface contract](docs/can-if.md)
for API details and limitations.

## Virtual test environment

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

After cloning this repository as `ZephyrECU`, run these commands from the
workspace root:

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install west
west init -l ZephyrECU
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt \
  -r zephyr/scripts/requirements-build-test.txt \
  -r zephyr/scripts/requirements-run-test.txt
west manifest --validate
```

`west.yml` selects Zephyr v4.4.2 and a fixed iso14229 commit; `west-frozen.yml`
records the resolved dependency SHAs. Run all commands below from the workspace
root, with the virtual environment active.

## Unit tests

Run the CAN interface contract test through Twister:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host python zephyr/scripts/twister \
  -T ZephyrECU/tests/can_if -p native_sim --outdir build/twister-can-if --inline-logs
```

This test uses Zephyr's fake CAN driver and requires no virtual CAN interface.
It checks filter sharing and capacity, frame validation, ISR/thread context,
buffer ownership, queue overflow, TX submission/completion errors and bus-off
notifications. Twister writes results under `build/twister-can-if/`.

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

## Application smoke test

Build and run the application with its in-memory LED backend:

```sh
ZEPHYR_TOOLCHAIN_VARIANT=host west build -b native_sim -S socketcan-native-sim \
  ZephyrECU -d build/native_sim -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
./build/native_sim/zephyr/zephyr.exe --can-if=vcan0 -rt -stop_at=1
```

Expected logs include `LED initialized: off (memory backend)` and
`ZephyrECU CAN interface ready`. The main application initializes CAN without
registering raw echo routes. `can_comm` and `diag_uds` are discovery stubs;
iso14229 is fetched but not compiled. Zephyr's `CONFIG_ISOTP` remains disabled.

The native argument parser requires `--can-if=vcan0`, with `=`; its default
interface is `zcan0`. Keep the selected interface up. The pinned native driver
can leave TX pending after a host write failure; restart the simulator if that
occurs. Physical bus-off and electrical behavior require hardware testing.
