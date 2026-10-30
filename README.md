# ZephyrECU

[![CI](https://github.com/dani-clo/ZephyrECU/actions/workflows/ci.yml/badge.svg)](https://github.com/dani-clo/ZephyrECU/actions/workflows/ci.yml)
[![Coverage (components)](https://img.shields.io/endpoint?url=https%3A%2F%2Fdani-clo.github.io%2FZephyrECU%2Fbadge.json)](https://dani-clo.github.io/ZephyrECU/)
[![License: Apache-2.0](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](LICENSE)

ZephyrECU is a lightweight, non-AUTOSAR automotive ECU framework built on
Zephyr RTOS. It brings together CAN communication, DBC-generated signals
and UDS diagnostics through a modular architecture, without the complexity
of a full AUTOSAR stack.

See the [CAN interface](docs/can-if.md), [communication](docs/can-comm.md)
and [diagnostic](docs/diag-uds.md) contracts for API details and limitations.

## Features

- **CAN interface:** RX filtering, callback dispatching and asynchronous TX.
- **DBC-based communication:** generated PDUs/signals, periodic and event-driven TX.
- **UDS diagnostics:** ISO 14229 via iso14229 with bundled ISO-TP.
- **Automated testing:** Zephyr native_sim, SocketCAN integration and Linux/Windows CI.

## Clone and setup

Use Python >= 3.12, Git, CMake, Ninja, gperf, a devicetree compiler and a
Zephyr-compatible toolchain. From the directory where you want the workspace:

```sh
mkdir zephyrecu-workspace
cd zephyrecu-workspace
git clone https://github.com/dani-clo/ZephyrECU.git ZephyrECU
python3 -m venv .venv
. .venv/bin/activate
pip install west
west init -l ZephyrECU
west update --narrow -o=--depth=1
pip install -r zephyr/scripts/requirements-base.txt \
  -r ZephyrECU/tools/requirements-codegen.txt
west zephyr-export
west sdk install
```

The repository's `west.yml` selects Zephyr and its required modules. Dependencies
are downloaded alongside `ZephyrECU` in the workspace.

## Build and board support

From `ZephyrECU`, with the workspace virtual environment active:

```sh
west build -p always -b <board>
```

ZephyrECU can target any Zephyr-supported board with a CAN controller and driver.
The devicetree must define `zephyr,canbus` under `/chosen`, pointing to an enabled
CAN controller. If it is missing, add a board overlay, as done in
[`boards/arduino_uno_q.overlay`](boards/arduino_uno_q.overlay), configuring the
controller, pins and bitrate. The demo application also requires `led0` and
`led1` GPIO aliases on hardware boards.

For a new HAL family, add its module (for example, `hal_stm32`) to
`projects[name=zephyr].import.name-allowlist` in [`west.yml`](west.yml), then run
`west update` from the workspace root before building.

The reference target is NXP S32K148 EVB (`s32k148_evb`). Build for it or
Arduino UNO Q with:

```sh
west build -p always -b arduino_uno_q
west build -p always -b s32k148_evb
```

The board overlay is selected automatically. Firmware outputs are in
`build/zephyr/`. Flash a board using west with:

```sh
west flash
```

See the [UNO Q guide](docs/arduino-uno-q.md) for flashing and
console setup for this specific board without a west runner and using adb instead.

## Docker and testing

A Docker image is provided through [`Dockerfile.ci`](Dockerfile.ci). Build it and
run the unit tests, integration tests and application smoke test with:

```sh
./tools/run_docker.sh
```

See [Testing](docs/testing.md) for Docker and native virtual environment setup,
unit tests, integration tests, manual CAN checks and GitHub Actions.
