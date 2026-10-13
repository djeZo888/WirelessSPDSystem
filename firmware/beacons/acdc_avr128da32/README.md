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

Copy `config.example.h` to `config.h`. The first section holds the shared secret, carrier frequency and spreading factor for this receiver/home; these must match the receiver and its other beacons. The second section holds this SPD's unique ID, TX power, erased-EEPROM nonce seed and testing mode. Every field documents its type and limits; invalid values fail compilation. Follow the [setup and flashing guide](../../../docs/flashing.md) and keep `config.h` private.

`WSPD_PREPRODUCTION=1` selects randomized **20–40 second** reports for testing.
The default `0`, or an older config without the flag, selects production's
**300–600 seconds**. The initial regular report still follows a **5 second**
startup delay. Return the flag to `0` before deployment.

The no-bootloader AVR128DA32 profile uses the **4 MHz internal clock**, PICkit 4,
and EEPROM retention. The build and flash tools use this same profile.

Between reports, the MCU uses **STANDBY** with a one-shot RTC compare, and the
SX1262 uses **warm sleep** with its TCXO off. Contact changes wake the MCU;
FAIL reports retain their immediate transmission and two identical repeats.
The ADC and SPI are disabled during the wait. `WSPD_LED_MODE=1` (default) blinks
green for **1 second every 30 seconds**, keeps red **on continuously during
SPD fault**, and gives a short yellow TX indication. The MCU sleeps while green
is illuminated, waking briefly at its on/off deadlines. Select **0**
for the earlier steady green/red behavior or **2** to disable normal LEDs.
All modes retain the EEPROM fault indication. See [power management and hardware
current limits](../../../docs/power-management.md).

The receiver listens at one configured frequency and spreading factor. Set the frequency directly in MHz; the former channel ID/map has been removed. Fixed modulation settings remain in the sketch.

[Arduino sketch](acdc_avr128da32.ino) · [Configuration template](config.example.h) · [Build options](build-options.json)
