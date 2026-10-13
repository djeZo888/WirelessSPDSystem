# T Connect Pro receiver changelog

Receiver versions are independent of beacon HW/FW packet fields and repository
releases. The LCD displays the receiver version with the `-tconnpro` suffix.

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
