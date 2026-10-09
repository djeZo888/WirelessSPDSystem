# Validation

Compile-only checks use synthetic credentials:

| Project | Flash | Global RAM | Result |
| --- | ---: | ---: | --- |
| AC-DC AVR128DA32 | 40,970 / 131,072 bytes | 1,509 / 16,384 bytes | Passed |
| Battery AVR128DB32 | 42,354 / 131,072 bytes | 1,539 / 16,384 bytes | Passed |
| T Connect Pro v0.1 · 127 wireless + local | 862,429 / 3,145,728 bytes | 186,920 / 327,680 bytes | Passed |

Pinned cores, libraries and board options are listed in the flashing guide and
each project's `build-options.json`. Beacon
[compile evidence](../firmware/beacons/build-validation.json) includes source
hashes and missing/placeholder configuration guard checks.
Packet tests exercise both firmware encoders and the receiver tag verifier
against Python SHA-256, including signed temperatures, counter boundaries,
wrong keys and tampering in every frame byte. Host shims replace AVR flash reads
and the receiver SHA-256 API; physical packet reception is a separate check.

Reproduce synthetic builds with `python3 tools/firmware.py validate <target>`
after setup, and check publishable files with `python3 tools/check_repository.py`.
GitHub Actions repeats all three builds for pushes and pull requests.
Run `python3 tests/check_packet_compatibility.py` to repeat the host packet checks.
Server validation builds configure all 127 wireless IDs plus local ID 0, so the
build check exercises multiple pages and the maximum configured state array.
Run `python3 tests/check_server_display.py` for the paging regression checks:
the actual LCD renderer, loop, alarm scan and mute touch handler run with host I/O
shims across 17 local/wireless configurations. Checks include complete row coverage,
partial pages, off-page alarms, 2/5/10-second timing, clock rollover, touch
debounce and the displayed version. Invalid page intervals are rejected at
compile time. Run `python3 tests/check_server_api.py` to check the actual JSON
builder, HTTP routing and mute controls, then execute the embedded web rendering
in Node.js across 13 configurations, including all 128 rows. Every LCD page
returns the same complete API list; the web displays every row and the firmware
version, and its mute button works. These checks also run in CI;
AddressSanitizer/UBSan check host memory
access. Host shims do not verify physical LCD/touch, Wi-Fi transport or
performance at scale.
Pinned dependency code emits some compiler warnings; the builds still pass.

Persistent-nonce firmware 1 is tested with
`python3 tests/check_persistent_nonce.py`: 5,140 checks including 126 simulated
power cuts at first provisioning and journal wrap, corruption/readback failures,
reboot monotonicity and exhaustion. Tests run the actual identical headers used
by both beacon sketches. Eight offline fuse-preparation tests (`python3 tests/check_nonce_flash_fuses.py`) ensure EEPROM retention
and active BOD are established before flash upload. See
[persistent-nonce evidence](../firmware/beacons/nonce-validation.json).
The receiver's nonce comparison and 20-byte frame remain compatible.

The original system was reported working by its author with both beacon variants.
This repository cleanup has not flashed or tested physical boards. Battery standby
current, fuse/clock/voltage operation, RF performance, actual contact/alarm behavior
and actual board wiring remain hardware acceptance checks. The hardware guide
uses the recovered full battery schematic and production Gerbers; the earlier
partial drawings and combined PDF have been removed.

Radio configuration checks (`python3 tests/check_radio_configuration.py`) execute
the actual firmware guards: 151 compile cases accept 62 valid settings and reject
89 invalid settings, including fractional/wrapping integers, zero/oversized keys,
frequency endpoints, NaN/infinity, nonce bounds and obsolete channel configs.
Seventeen runtime cases check the actual receiver Wi-Fi validator. All three
sketches retain fixed modem settings and initialize the configured carrier
directly. See [configuration and build evidence](../firmware/beacons/radio-validation.json)
and [radio configuration](radio-configuration.md). No RF or programming operation
is performed by these tests.
