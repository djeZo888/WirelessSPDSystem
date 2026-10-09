# Wireless SPD System

LoRa monitoring for surge protective device (SPD) dry contacts. Beacons report
contact state and temperature; the receiver displays status, signals alarms and
serves a local web dashboard. Battery beacons also report supply voltage.

![Communication overview](docs/images/communication.svg)

## Supported firmware

| Role | Hardware | Arduino project | Packet HW/FW |
| --- | --- | --- | --- |
| TX | [AC-DC beacon](hardware/ac-dc/README.md), AVR128DA32 + Wio-SX1262 | [acdc_avr128da32](firmware/beacons/acdc_avr128da32) | 1 / 1 |
| TX | [Battery beacon](hardware/battery/README.md), AVR128DB32 + Wio-SX1262 | [battery_avr128db32](firmware/beacons/battery_avr128db32) | 2 / 1 |
| RX | LILYGO T Connect Pro V1.0, ESP32-S3, display, 868 MHz SX1262 | [tconnectpro_868 v0.1](firmware/servers/tconnectpro_868/README.md) | Accepts both TX types |

Example settings match: **865.3 MHz, SF10, 125 kHz, CR 4/5, sync 0x12**.
The 868 MHz label identifies the radio hardware; it is not the configured carrier.
Choose lawful frequency, airtime and transmit power for your installation.

## Start here

1. Follow [flashing and configuration](docs/flashing.md).
2. Choose a random nonzero four-byte secret, frequency and SF for the home;
   copy them to its receiver and beacons. Assign each beacon a unique production
   ID (10–127), register each ID/key in the receiver, and configure Wi-Fi/AP access.
3. Verify real OK/FAIL contact transitions, both telemetry types and alarms on
   the display and dashboard before deployment.

[Schematics and pin maps](hardware/README.md) ·
[Packet format and authentication](docs/protocol.md) ·
[Configuration ranges and shared radio settings](docs/radio-configuration.md) ·
[Build validation](docs/validation.md) ·
[T Connect Pro server guide](firmware/servers/tconnectpro_868/README.md)

The receiver displays **eight rows per page** when more than eight wireless/local
SPDs are configured. Set `DISPLAY_PAGE_SECONDS` in private `config.h` before
compiling; the default is **five seconds**. All SPDs remain monitored, and the web
dashboard and one `GET /api/v1/get` query provide the complete list. Live readings
and loss history are held in RAM and cleared by receiver reboot. Both beacon
firmware versions are now 1 and retain their nonce across power cycles using an
EEPROM journal, preserving their counter through ordinary restarts. See
[upgrade notes](docs/nonce-storage.md). Unknown/stale
rows are displayed but do not activate the alarm in this firmware.

Keep `config.h` and built firmware private: they contain keys. Use the web server
on a trusted local network; it has no TLS/login and mute is unauthenticated.
The existing signature uses a 32-bit key, with limitations documented in the
protocol guide. This is an SPD monitoring aid, not protective or safety equipment.

## Development

Pinned dependency setup and compile-only checks are available via
`python3 tools/firmware.py setup <acdc|battery|server>` and
`python3 tools/firmware.py validate <acdc|battery|server>`.
GitHub Actions compiles all three examples with synthetic credentials.
For a new hardware variant, add its own sketch folder, build options and schematic,
then add it to this table and the build matrix. See [contributing](CONTRIBUTING.md).

Project firmware and documentation: [MIT](LICENSE).
Dependencies retain their own terms; the beacon SHA implementation is fetched
locally rather than redistributed. See [third-party notices](THIRD_PARTY.md).
