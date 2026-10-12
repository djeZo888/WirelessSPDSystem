# Battery beacon firmware

Compatible with [hardware 2 · Battery beacon](../../../hardware/battery/README.md), using the AVR128DB32 and Wio-SX1262.

| Packet identifier | Value |
| --- | --- |
| Hardware type | 2 |
| Firmware version | 1 |

Reports SPD contact status, NTC temperature and cell voltage. Sleeps between measurements; the Force TX button requests a report.

Firmware 1 keeps its 64-bit nonce across power cycles using an EEPROM journal.
The next nonce is committed and verified before each new report; repeated copies
of a FAIL report retain the same nonce. See [persistence and upgrade notes](../../../docs/nonce-storage.md).

Copy `config.example.h` to `config.h`. The first section holds the shared secret, carrier frequency and spreading factor for this receiver/home; these must match the receiver and its other beacons. The second section holds this SPD's unique ID, TX power, erased-EEPROM nonce seed and testing mode. Every field documents its type and limits; invalid values fail compilation. Follow the [setup and flashing guide](../../../docs/flashing.md) and keep `config.h` private.

`WSPD_PREPRODUCTION=1` selects randomized **20–40 second** reports for testing.
The default `0`, or an older config without the flag, retains this battery build's
production interval of **500–600 seconds**. The initial regular report still
follows a **5 second** startup delay. Return the flag to `0` before deployment.
The AVR128DB32 profile retains its **24 MHz internal clock**.

The receiver listens at one configured frequency and spreading factor. Set the frequency directly in MHz; the former channel ID/map has been removed. Fixed modulation settings remain in the sketch.

[Arduino sketch](battery_avr128db32.ino) · [Configuration template](config.example.h) · [Build options](build-options.json)
