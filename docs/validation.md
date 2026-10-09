# Validation

Compile-only checks completed on 9 October 2026 using synthetic credentials:

| Project | Flash | Global RAM | Result |
| --- | ---: | ---: | --- |
| AC-DC AVR128DA32 | 39,348 / 131,072 bytes | 1,504 / 16,384 bytes | Passed |
| Battery AVR128DB32 | 40,746 / 131,072 bytes | 1,534 / 16,384 bytes | Passed |
| T Connect Pro v0.1 · 24 wireless, 10-second pages | 859,809 / 3,145,728 bytes | 73,192 / 327,680 bytes | Passed |
| T Connect Pro v0.1 · 127 wireless + local | 865,005 / 3,145,728 bytes | 186,928 / 327,680 bytes | Passed |

Pinned cores, libraries and board options are listed in the flashing guide and
each project's `build-options.json`. Beacon
[compile evidence](../firmware/beacons/build-validation.json) includes source
hashes and missing/placeholder configuration guard checks. Server web RESET
was checked for keyed/unkeyed success, failed authentication and cancellation.
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
the actual LCD renderer, loop, alarm scan and reset functions run with host I/O
shims across 17 local/wireless configurations. Checks include complete row coverage,
partial pages, off-page alarms, 2/5/10-second timing, clock rollover, confirmation
pause, reset restart and the displayed version. Invalid page intervals are
rejected at compile time. Run `python3 tests/check_server_api.py` to check the
actual JSON builder and GET handler, then execute the embedded web rendering
in Node.js across 13 configurations, including all 128 rows. Every LCD page
returns the same complete API list; the web displays every row and the firmware
version. These checks also run in CI; AddressSanitizer/UBSan check host memory
access. Host shims do not verify physical LCD/touch, Wi-Fi transport or
performance at scale.
Pinned dependency code emits some compiler warnings; the builds still pass.

The original system was reported working by its author with both beacon variants.
This repository cleanup has not flashed or tested physical boards. Battery standby
current, fuse/clock/voltage operation, RF performance, actual contact/alarm behavior
and actual board wiring remain hardware acceptance checks. The hardware guide
uses the recovered full battery schematic and production Gerbers; the earlier
partial drawings and combined PDF have been removed.
