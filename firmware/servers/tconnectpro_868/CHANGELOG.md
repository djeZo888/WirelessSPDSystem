# T Connect Pro receiver changelog

Receiver versions are independent of beacon HW/FW packet fields and repository
releases. The LCD displays the receiver version with the `-tconnpro` suffix.

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
