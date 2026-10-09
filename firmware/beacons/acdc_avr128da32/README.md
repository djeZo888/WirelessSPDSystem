# AC-DC beacon firmware

Compatible with [hardware 1 · AC-DC beacon](../../../hardware/ac-dc/README.md), using the AVR128DA32 and Wio-SX1262.

| Packet identifier | Value |
| --- | --- |
| Hardware type | 1 |
| Firmware version | 1 |

Reports SPD contact status and NTC temperature. Cell voltage is marked unavailable.

Copy `config.example.h` to `config.h`, set a unique beacon ID and the shared authentication key, then follow the [setup and flashing guide](../../../docs/flashing.md). Keep `config.h` private.

[Arduino sketch](acdc_avr128da32.ino) · [Configuration template](config.example.h) · [Build options](build-options.json)
