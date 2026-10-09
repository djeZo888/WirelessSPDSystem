# AC-DC beacon firmware

Compatible with [hardware 1 · AC-DC beacon](../../../hardware/ac-dc/README.md), using the AVR128DA32 and Wio-SX1262.

| Packet identifier | Value |
| --- | --- |
| Hardware type | 1 |
| Firmware version | 1 |

Reports SPD contact status and NTC temperature. Cell voltage is marked unavailable.

Firmware 1 keeps its 64-bit nonce across power cycles using an EEPROM journal.
The next nonce is committed and verified before each new report; repeated copies
of a FAIL report retain the same nonce. See [persistence and upgrade notes](../../../docs/nonce-storage.md).

Copy `config.example.h` to `config.h`. The first section holds the shared secret, carrier frequency and spreading factor for this receiver/home; these must match the receiver and its other beacons. The second section holds this SPD's unique ID, TX power and erased-EEPROM nonce seed. Every field documents its type and limits; invalid values fail compilation. Follow the [setup and flashing guide](../../../docs/flashing.md) and keep `config.h` private.

The receiver listens at one configured frequency and spreading factor. Set the frequency directly in MHz; the former channel ID/map has been removed. Fixed modulation settings remain in the sketch.

[Arduino sketch](acdc_avr128da32.ino) · [Configuration template](config.example.h) · [Build options](build-options.json)
