# ZephyrECU

Lightweight automotive ECU demo/framework on Zephyr RTOS, without AUTOSAR.
The authoritative scope and implementation phases are in [instructions.md](instructions.md).

Phase 1 provides the build skeleton, local module discovery and a minimal LED
service. CAN routing, communication, diagnostics and SWCs are implemented in later
phases. The bootstrap does not start the CAN controller or transmit frames.

## Workspace and prerequisites

Use a west T2 workspace; keep Zephyr and dependencies outside this repository:

```text
workspace/                 # not a Git repository
  .venv/
  .west/
  ZephyrECU/               # this manifest repository
  zephyr/
  modules/
  build/s32k148_evb/
  build/native_sim/
```

On Debian, install host prerequisites (administrative privileges required):

```sh
sudo apt install git python3-venv python3-dev cmake ninja-build gperf \
  device-tree-compiler build-essential gcc-multilib g++-multilib \
  iproute2 kmod can-utils
```

Use Python >= 3.12 and Zephyr SDK **1.0.1** with its `arm-zephyr-eabi` toolchain
and host tools. Install the SDK following the
[Zephyr SDK guide](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html).
The native target uses the host GCC compiler, including 32-bit development support
for the default `native_sim` target.

From the workspace root, after cloning this repository as `ZephyrECU`:

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install west
west init -l ZephyrECU
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt
west manifest --validate
```

`west.yml` pins Zephyr v4.4.2 and iso14229 to its selected SHA. Only `cmsis_6`
and `hal_nxp` are imported: both targets build with this list. The board uses
CMSIS 6 and the SDK-provided picolibc, so neither legacy `cmsis` nor the source
`picolibc` project is needed. `west-frozen.yml` records all resolved dependency
SHAs; regenerate deliberately with `west manifest --freeze > ZephyrECU/west-frozen.yml`.
The Python package snapshot used for validation is recorded under `docs/`.

## Build and run

Commands below run from the workspace root with the virtual environment active.
Set the SDK path to its actual installation directory:

```sh
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"
ZEPHYR_TOOLCHAIN_VARIANT=zephyr west build -b s32k148_evb ZephyrECU \
  -d build/s32k148_evb -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ZEPHYR_TOOLCHAIN_VARIANT=host west build -b native_sim \
  -S socketcan-native-sim ZephyrECU -d build/native_sim \
  -- -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

For a connected board, `west flash -d build/s32k148_evb` uses the configured
runner. Hardware boot and electrical behavior require a real board. The LED
service initializes the red RGB channel off using Devicetree `led0`; the upstream
CAN bitrate remains **125000 bit/s**. No custom overlays are needed.

For native simulation, create `vcan0` once per host session:

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan  # omit if already present
sudo ip link set dev vcan0 up
./build/native_sim/zephyr/zephyr.exe --can-if=vcan0 -rt
```

The upstream native argument parser requires **`--can-if=vcan0`**, with `=`.
The default interface is `zcan0`. The native LED backend keeps an in-memory state
and logs changes; it requires no GPIO hardware. Expected bootstrap logs include
`LED initialized: off (memory backend)` and `ZephyrECU bootstrap ready`.
Stop with Ctrl-C, or pass `-stop_at=1` for a finite smoke run.

Where unprivileged user/network namespaces are permitted and the vcan driver is
available, this isolated smoke run avoids changing host network interfaces:

```sh
unshare --user --map-root-user --net sh -c '
  ip link add dev vcan0 type vcan &&
  ip link set dev vcan0 up &&
  exec ./build/native_sim/zephyr/zephyr.exe --can-if=vcan0 -rt -stop_at=1
'
```

Any CAN tester must run in the same network namespace as the simulated ECU.
`CONFIG_ISOTP` is disabled for both targets. The local `can_if`, `can_comm` and
`diag_uds` Kconfig symbols are enabled to verify discovery; their CMake files
are stubs at this phase, and upstream iso14229 is fetched but not compiled yet.

See [the phase 1 validation report](docs/phase-1-validation.md) for observed
results, tool versions and limitations.
