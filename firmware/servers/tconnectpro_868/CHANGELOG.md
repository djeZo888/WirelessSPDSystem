# T Connect Pro receiver changelog

Receiver versions are independent of beacon HW/FW packet fields and repository
releases. The LCD displays the receiver version with the `-tconnpro` suffix.

## v0.2.5 — 2026-10-16

- Fixes multi-second mute/unmute, red-blink and HTTP stalls when a connected
  USB host stops reading diagnostics. USB mirroring now uses a fixed 4 KiB queue,
  zero write timeout and at most 64 bytes per loop; congested USB copies may be
  dropped while the complete session log remains in RAM.
- Applies relay GPIO changes before diagnostics, including immediate relay OFF
  when muted. Touch, web and Home Assistant retain the same shared mute state.
- Adds integrated control/web/packet scenarios with a slow or absent USB reader,
  partial writes and saturated buffers. Tests exercise the production logging,
  mute and receiver loop together and reject the previous blocking behavior.
- Requires regression cases alongside future receiver behavior changes.
  Beacon firmware remains **1**.
- Shows the newest 100 log events first, with **Load more** below the table to
  append each next 100 older events. History browsing pauses live refresh;
  **Back to latest** restores the live newest-100 view.

## v0.2.4 — 2026-10-16

- Retries a lost station connection or IP lease every 60–120 seconds (configured in `config.h`), with no association/DHCP
  wait loop during monitoring. Boot fallback keeps the password-protected AP available
  while station reconnection continues.
- Adds optional static IPv4/netmask with optional gateway/DNS, strict address
  validation and DHCP retained as the default.
- Displays **connecting/offline** when neither interface has an address; refreshes the LCD
  immediately on network state/address changes.
- Logs Wi-Fi disconnect reason codes, lost/acquired IP and retry outcomes from
  the main loop. Bounded event handling keeps callback work out of monitoring.
- Starts NTP after a delayed connection, supports MQTT in AP+station mode and
  refreshes Home Assistant's receiver link after an address change.
- Defers recovery actions during firmware updates. Existing private configuration
  and beacon firmware **1** remain compatible.

## v0.2.3 — 2026-10-14

- Hides BAT on the LCD and web table when no authenticated hardware-type-2
  beacon has been identified; expands RSSI/SNR space with explicit units.
  Battery detection covers all pages and filters, including stale devices.
- Orders tabs **Overview → Firmware → Logs → HA Integration**.
- Embeds a complete, locally served Home Assistant setup guide, downloadable
  offline HTML and REST YAML: broker/account setup, receiver build and updates,
  discovery, notifications, acceptance checks and troubleshooting.
- Adds a Home Assistant **Alarm muted** switch through MQTT Discovery and the
  REST example. ON mutes the relay; OFF restores its alarm pattern. Web, touch
  and HA controls share state; monitoring and visual warnings continue.
- Accepts only explicit ON/OFF MQTT commands on the receiver loop and clears
  obsolete retained commands before subscribing after reconnect.
- Removes the temporary paging-test registrations from the deployed private
  configuration. Retains receiver release **v0.2.3-tconnpro** and beacon firmware **1**.

## v0.2.2 — 2026-10-13

- Adds a fourth **Logs** tab with dated startup, packet, rejection, alarm,
  network and OTA events. Uses a configurable PSRAM circular buffer (2 MiB /
  4096 entries by default), bounded HTTP pages and older-entry browsing.
- Adds network clock synchronization; reconstructs startup dates after sync,
  shows time since boot before sync, and uses local **24-hour** browser times.
- Replaces OTA username/password with one configured **32–128 letter token**
  using Bearer authentication. Keeps image, SHA-256, request and slot checks.
- Shows RSSI and SNR together, keeps **MUTE/UNMUTE** immediately left of Refresh
  on every tab, and includes the date in the last-refresh indicator.
- Links the complete, pretty-printed plaintext JSON API from the footer.
- Records controlled OTA restart before reboot. Logs remain session-only and
  are cleared on restart; abrupt power loss cannot produce a shutdown event.
- Displays **v0.2.2-tconnpro** across LCD, dashboard and API. Beacon firmware
  remains **1**; its AC-DC green heartbeat now lasts **one second every 30 seconds**.

## v0.2.1 — 2026-10-13

- Fixes early OTA rejection responses that could reuse headers from a preceding
  request, causing conflicting Content-Length values and incomplete-response
  errors. Upload responses start with fresh headers in ESP32 cores 2 and 3.
- Adds regression coverage for retained response headers and authentication
  challenges during upload rejection.

## v0.2 — 2026-10-13

- Replaces the initial dashboard with responsive desktop tables and mobile cards,
  status summaries, search/filter controls, expandable diagnostics and explicit
  connection/error handling. All SPDs remain available in one API response.
- Adds an API schema version, stable receiver identity and integration/update
  diagnostics without changing the existing status and telemetry fields.
- Adds optional MQTT Discovery for Home Assistant: automatic SPD devices/entities,
  retained telemetry, freshness-aware availability and a broker worker that does
  not block LoRa monitoring. Provides a REST configuration alternative.
- Adds authenticated browser firmware updates using the existing dual application
  slots. Checks exact length, SHA-256, ESP32-S3 application format and receiver
  marker before activation. Failed/cancelled uploads resume monitoring; successful
  updates restart after the response. Updates are disabled until configured.
- Shows **v0.2-tconnpro** on the LCD, boot screen, API and dashboard. Keeps private
  deployment configuration separate; updates include the new compiled settings.
- Pins ESP32 core 3.3.12, Arduino_GFX 1.6.9 and PubSubClient 2.8; uses 460800
  baud uploads. Also validates the receiver with the older core 2.0.14.
- Adds dashboard, MQTT and OTA regression checks and documents USB recovery.

## v0.1 — 2026-10-09

First versioned receiver release, displayed as **v0.1-tconnpro**.

- Receives authenticated 20-byte packets from AC-DC and battery beacons; tracks
  status, telemetry, nonce and packet loss in RAM.
- Supports up to 127 wireless beacons plus an optional local contact.
- Displays eight SPDs per LCD page; `DISPLAY_PAGE_SECONDS` selects the interval
  before compiling (default 5 seconds). Eight or fewer use a single page.
- Shows all configured SPDs on the web dashboard and in one `GET /api/v1/get`
  response, independent of LCD paging.
- Provides relay alarms and mute controls. Alarms cover the full list.
- Removes the obsolete nonce-reset LCD, web and API functionality now that beacon
  firmware 1 retains its counter across power cycles. Receiver reboot clears its
  RAM history when recovery is needed.
- Keeps deployment settings in private `config.h`, copied from
  `config.example.h`; users configure and build without editing the `.ino`.

See the [build and flashing guide](README.md#build-and-flash).
