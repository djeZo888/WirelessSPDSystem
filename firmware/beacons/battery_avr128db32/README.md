# Battery beacon firmware

Compatible with [hardware 2 · Battery beacon](../../../hardware/battery/README.md), using the AVR128DB32 and Wio-SX1262.

| Packet identifier | Value |
| --- | --- |
| Hardware type | 2 |
| Firmware version | 0 |

Reports SPD contact status, NTC temperature and cell voltage. Sleeps between measurements; the Force TX button requests a report.

Copy `config.example.h` to `config.h`, set a unique beacon ID and the shared authentication key, then follow the [setup and flashing guide](../../../docs/flashing.md). Keep `config.h` private.

[Arduino sketch](battery_avr128db32.ino) · [Configuration template](config.example.h) · [Build options](build-options.json)
