# Validation

Compile-only checks use synthetic credentials:

| Project | Flash | Global RAM | Result |
| --- | ---: | ---: | --- |
| AC-DC AVR128DA32 · sleep, LED mode 1 | 42,636 / 131,072 bytes | 1,543 / 16,384 bytes | Passed |
| Battery AVR128DB32 | 42,354 / 131,072 bytes | 1,539 / 16,384 bytes | Passed |
| T Connect Pro v0.2.3 · core 3.3.12, 127 wireless + local | 1,185,518 / 3,145,728 bytes | 200,164 / 327,680 bytes | Passed |
| T Connect Pro v0.2.3 · core 2.0.14 compatibility | 1,016,065 / 3,145,728 bytes | 196,768 / 327,680 bytes | Passed |

The receiver also allocates its session log dynamically in PSRAM: **2 MiB /
4096 entries** by default, in addition to the LCD framebuffer. Global RAM in the
table excludes these dynamic buffers.

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
build check exercises multiple pages, the maximum state array and enabled OTA/MQTT paths with synthetic credentials.
Run `python3 tests/check_server_display.py` for the paging regression checks:
the actual LCD renderer, loop, alarm scan and mute touch handler run with host I/O
shims across 19 local/wireless configurations. Checks include complete row coverage,
partial pages, off-page alarms, 2/5/10-second timing, clock rollover, touch
debounce, the displayed version and adaptive BAT/RSSI/SNR layouts across all
pages, including unseen, stale and off-page battery reports. Invalid page intervals are rejected at
compile time. Run `python3 tests/check_server_api.py` to check the actual JSON
builder, HTTP routing and mute controls across 13 configurations, including all
128 rows. `python3 tests/check_server_dashboard.py` executes the web script in
Node.js, checking full row coverage, filters, escaping, connection/mute failures,
OTA progress, global hardware-type-2 battery visibility, exact tab order, guide
embedding and SHA-256 against standard and 3 MB test vectors. Every LCD page
returns the same complete API list; the web displays every row and the firmware
version, and its mute button works. These checks also run in CI;
AddressSanitizer/UBSan check host memory access. Host shims do not verify physical LCD/touch, Wi-Fi transport or
performance at scale.
Pinned dependency code emits some compiler warnings; the builds still pass.

`python3 tests/check_server_logs.py` exercises 17,000 writes into the actual
2 MiB / 4096-record ring, overwrite bounds, safe UTF-8 clipping/replacement,
64-bit boot times, date reconstruction and JSON formatting. API checks also
exercise streamed log paging, malformed cursors and plaintext pretty JSON;
dashboard checks cover older/live browsing, restart/overflow notices and clock
synchronization. All run in CI.

`python3 tests/check_server_ota.py` exercises the actual handler with OpenSSL-backed
host SHA-256 adapters for MbedTLS 2/3: 92 scenarios each, covering authentication,
CSRF, image/hash/length rejection, cancellation, writes/finalization failures,
multiple files, timeout rollover, immediate early-rejection connection closure
and activation/restart order. Response checks model retained headers from a
preceding API request and require one correct Content-Length and authentication
challenge. Another 48 checks
exercise the production validator's OTA token and MQTT configuration limits. Host shims do not
verify physical flash/bootloader behavior or real HTTP multipart parsing.
`python3 tests/check_server_mqtt.py` checks discovery, retained full-list telemetry,
availability, broker failure/backoff, Home Assistant birth and OTA pause across
seven configurations, including 128 rows and maximum strings. They also verify
strict ON/OFF mute commands, main-loop handoff, synchronized state, clearing
retained commands before subscription, subscription failures and prompt mute
acknowledgement during a long row publication cycle. Host checks do not replace
a deployed broker/Home Assistant test.

`python3 tests/check_home_assistant_guide.py` checks generated offline HTML and
embedded resources against the documentation and validates YAML/Jinja examples,
notification conditions, REST availability and mute actions. It requires pinned
PyYAML 6.0.3 and Jinja2 3.1.6; CI installs them in an isolated environment. The
guide has no external assets, scripts or fonts. Installing missing broker/HA/build
dependencies is explicitly an online preparation step; later reading and building
with prepared dependencies can be done offline.

`python3 tests/check_server_wifi.py` runs the actual station setup, full restart,
main loop, event handling and IPv4 parser with host I/O shims: **68 scenarios**
across ESP32 core 2/3 APIs and 60/120-second retry intervals, plus **11** compile
checks for literal interval limits. Coverage includes repeated failures then
success, boot/runtime fallback, DHCP/IP loss, static host/mask/gateway/DNS checks,
AP+STA MQTT readiness, OTA deferral, timer rollover and bounded event logging.
Core 3 tests also model the legacy `WiFi.config` mask-order heuristic, which the
firmware avoids with the native station API. Full framework stop/start calls can
briefly block internally; the application contains no association/DHCP wait loop
during recovery. Device recovery/static-address results are recorded in the
[v0.2.4 release notes](https://github.com/djeZo888/WirelessSPDSystem/releases/tag/v0.2.4-tconnpro).

On 16 October 2026, the v0.2.3 bench receiver was unreachable and displayed
0.0.0.0 while still accepting LoRa packets. An intentional restart restored
10.156.100.129 using unchanged credentials, and both beacons resumed reporting.
The original trigger could not be determined: v0.2.3 did not record Wi-Fi
disconnect reason codes. Source review confirmed missing application retries and
an inactive-AP address being displayed after station/IP loss; v0.2.4 addresses
both conditions. No router or Mac Wi-Fi settings were changed.

The v0.1 receiver was flashed with Arduino IDE 2.3.10 / ESP32 core 3.3.12 /
Arduino_GFX 1.6.9 at 460800 baud on 13 October 2026. Display/touch/AP/LoRa boot
passed, and five authenticated ID 1 packets arrived 23.5–39.7 seconds apart with
consecutive nonces and 0% observed loss. The user confirmed the LCD. This evidence
is separate from the subsequent network-update checks below.

On 13 October 2026, a private three-beacon v0.2 build was installed over USB
using core 3.3.12 / Arduino_GFX 1.6.9 at 460800 baud. Station Wi-Fi, the full-list
API and the exact served dashboard HTML passed. Fresh authenticated reports from
AC-DC beacons 1 and 2 were accepted; beacon 3 remained UNKNOWN.
An authenticated HTTP update installed v0.2.1 in about seven seconds, returned
202 and restarted into the new version. Both beacons resumed reporting with
increasing nonces and at least three accepted packets each. Live rejection tests
passed for missing authentication, early multipart authentication, a missing
update header, oversized declarations and an incorrect SHA-256. Each response
had one correct Content-Length; reception continued afterward without reboot.
Independent USB reads of OTA metadata confirmed app0 → app1 and sequence 1 → 2
with the new image marked VALID. Every byte of the application read from app1
matched the network-uploaded image. These read operations reset the receiver
separately from the earlier successful network-update reboot.
A subsequent private three-beacon **v0.2.2** build (1,129,554 bytes flash /
62,884 bytes global RAM) was installed over the network. The receiver allocated
**2,097,152 bytes of PSRAM / 4096 log records**, synchronized NTP time and
reconstructed startup dates. Live log cursors/bounds, dated boot/packet events,
MUTE/UNMUTE actions, exact served HTML and the complete pretty plaintext API
passed. Desktop and 320/390-pixel browser views passed without overflow or script
errors; the footer opened JSON in a new tab. A second full update using the new
Bearer token returned 202 and rebooted successfully. The session ID changed and
old logs cleared. Both beacons again supplied at least three accepted reports
with increasing nonces and 0% observed loss. Seven live negative cases rejected
old Basic auth, missing/wrong tokens, early unauthenticated multipart data,
missing request header, oversized size and bad SHA-256; monitoring resumed with
no unplanned restart. Private images/configuration and bench evidence are not
published. This release was verified by network activation/boot, without another
USB image readback.

The operator confirmed the physical LCD paging test with 19 registrations
(8 / 8 / 3 rows). The normal private configuration was restored to the original
three registrations for **v0.2.3**; authenticated reports from hardware-type-1
beacons 1 and 2 hide BAT globally and leave additional space for RSSI/SNR.

On 13 October 2026, a private v0.2.3 build was installed by authenticated OTA
for an actual MQTT test with a temporary, password-protected Mosquitto broker.
Discovery included the **Alarm muted** switch. ON/OFF commands changed the
receiver's API state and produced authoritative retained MQTT acknowledgements
in about 1–1.6 seconds. An invalid command was ignored, web controls updated the
same MQTT state, and session logs recorded the MQTT actions. An old retained ON
command, seeded before connection, was cleared without muting the receiver.
The final normal build (1,179,954 bytes flash / 62,908 bytes global RAM) was then
installed by OTA on 14 October with MQTT disabled until the user's site broker is configured.
The receiver rebooted into v0.2.3, and both real beacons resumed authenticated
reports with increasing nonces. The complete guide and REST YAML were served
locally and matched the repository resources. Desktop/narrow browser checks
covered the final dashboard, adaptive BAT table, tab order and guide navigation.
Private configurations, binaries and broker credentials are excluded.

A real Home Assistant installation and site broker configuration remain untested;
the broker/device protocol and host examples are verified separately. Battery
hardware, physical alarm wiring and maximum-list RF/network performance remain
hardware acceptance checks.

Persistent-nonce firmware 1 is tested with
`python3 tests/check_persistent_nonce.py`: 5,140 checks including 126 simulated
power cuts at first provisioning and journal wrap, corruption/readback failures,
reboot monotonicity and exhaustion. Tests run the actual identical headers used
by both beacon sketches. Eight offline fuse-preparation tests (`python3 tests/check_nonce_flash_fuses.py`) ensure EEPROM retention
and active BOD are established before flash upload. See
[persistent-nonce evidence](../firmware/beacons/nonce-validation.json).
The receiver's nonce comparison and 20-byte frame remain compatible.

The original system was reported working by its author with both beacon variants.
An earlier AC-DC preproduction build has passed flash readback, fuse and nonce-journal
checks. The operator confirmed TX LED timing and SPD OK/FAIL LED indications.
The maintained sleep revision was flashed onto AC-DC beacon 2 through PICkit 4
on 13 October 2026: AVR128DA32 signature, all 131,072 flash bytes, BOD and EEPROM
retention fuses verified. ID 2, 8 dBm and a 20–40 second preproduction interval
were configured; a valid EEPROM nonce journal and authenticated reception were
observed. Sleep current, full RF coverage and physical power-loss
checks remain open. Battery standby
current, fuse/clock/voltage operation, RF performance, actual contact/alarm behavior
and actual board wiring remain hardware acceptance checks. The hardware guide
uses the recovered full battery schematic and production Gerbers; the earlier
partial drawings and combined PDF have been removed.

Radio configuration checks (`python3 tests/check_radio_configuration.py`) execute
the actual firmware guards: 234 compile cases accept 80 valid settings and reject
154 invalid settings, including fractional/wrapping integers, zero/oversized keys,
frequency endpoints, NaN/infinity, nonce bounds and obsolete channel configs.
Seventeen runtime cases check the actual receiver Wi-Fi validator. All three
sketches retain fixed modem settings and initialize the configured carrier
directly. See [configuration and build evidence](../firmware/beacons/radio-validation.json)
and [radio configuration](radio-configuration.md). No RF or programming operation
is performed by these tests.

Preproduction checks also verify six actual interval selections (flag omitted,
0 and 1 for both sketches), the 4 MHz AC-DC profile and EEPROM retention.
The local programming station passed 25 offline checks and its 4 MHz testing
compile. See [preproduction validation](../firmware/beacons/preproduction-validation.json).
Private deployment keys, target backups and firmware images are excluded.

AC-DC sleep checks (`python3 tests/check_acdc_sleep.py`) passed **253,962** native
assertions across all six LED/timing combinations with AddressSanitizer/UBSan.
They execute the actual scheduler, main loop, contact/RTC ISRs, packet/TX helpers
and peripheral shutdown paths using offline I/O shims. All three LED modes also
compiled on AVR; the station's low-power preproduction build passed at
42,636 bytes flash / 1,543 bytes RAM, with all **27** offline workflow checks.
The packet/nonce/fuse tests and all three compatibility builds passed.
See [sleep validation evidence](../firmware/beacons/sleep-validation.json) and
[power management](power-management.md). Physical wake timing, sleep current,
LED waveform and power-cycle behavior for this revision remain open. The
one-second heartbeat image was verified on preproduction beacon 2 with its
existing EEPROM nonce journal retained.
