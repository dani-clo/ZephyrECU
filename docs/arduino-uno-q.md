# Arduino UNO Q

From this repository, with the workspace virtual environment active, build the
STM32U585 firmware with:

```sh
west build -p always -b arduino_uno_q
```

The board overlay in `boards/arduino_uno_q.overlay` is selected automatically.
The firmware is generated under `build/zephyr/` (`zephyr.elf`, `zephyr.bin` and `zephyr.hex`).

To flash a USB-connected UNO Q with ADB enabled, use OpenOCD on the board:

```sh
adb devices
adb push build/zephyr/zephyr.elf /tmp/zephyrecu.elf
adb shell '/opt/openocd/bin/openocd -d2 -s /opt/openocd -f openocd_gpiod.cfg -c "reset_config srst_only srst_push_pull; program /tmp/zephyrecu.elf verify reset exit"'
```

The console uses USART1 on D1/TX and D0/RX at 115200 baud, accessible with
an external 3.3 V USB-to-UART adapter and a common ground.

[Back to the README](../README.md).
