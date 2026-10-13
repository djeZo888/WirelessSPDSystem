# Session log

The **Logs** tab shows **date + time** and **information**, with 24-hour times in
the browser's local time zone. Entries cover system/reset reason, all sketch boot
messages, accepted and rejected radio packets, radio errors, local contact,
muting, alarm/relay state, stale reports, Wi-Fi/MQTT transitions and OTA activity.
Accepted packets include ID/name, HW/FW, status, temperature, voltage, RSSI/SNR,
nonce, skipped packets, loss window and raw frame. Credentials and beacon keys
are never logged.

`SESSION_LOG_BYTES` defaults to **2 MiB** in the board's **8 MB OPI PSRAM**:
4096 fixed 512-byte records, with messages up to 495 bytes. Long messages are
bounded; invalid UTF-8 bytes are replaced and complete characters are retained. Oldest records are
overwritten when full; the tab reports the count. If PSRAM allocation fails,
monitoring continues with a smaller internal-RAM fallback and reports its size.
The browser keeps a 1000-entry window; **Load older** and **Back to latest** browse
the retained log without fetching the whole buffer in one request.

`NTP_SERVER` defaults to `pool.ntp.org`. Station Wi-Fi needs a reachable NTP
server. Until synchronization, entries show time since boot; afterward the first
network-time anchor reconstructs startup dates. Wire timestamps are UTC epoch
milliseconds; browser rendering uses local dates. This receiver has no persistent
clock, so an offline boot cannot supply a trustworthy calendar date.

Logs live only in RAM and start fresh at each boot. The updater records its
controlled restart before reboot; fetch/export it before restarting to retain it.
Power removal or an unexpected reset cannot be logged before it happens. The new
session records the reset reason. ROM/bootloader output before the sketch starts
is outside this RAM log.

## API

`GET /api/v1/logs?tail=1&limit=100` returns the newest page in ascending sequence
order. Use `after=SEQ` for new entries or `before=SEQ` for older entries; both
cursors are exclusive. `limit` is **1–100**. Omit cursors for the oldest retained
page. Requests stream individual records and never allocate the full log as JSON.

| Field | Meaning |
| --- | --- |
| `boot_id` | Random session identifier; changes after reboot. |
| `capacity`, `bytes`, `psram` | Allocated record count, buffer bytes and storage type. |
| `dropped` | Entries overwritten since boot. |
| `clock_synced` | Whether network time has supplied a calendar date. |
| `oldest_seq`, `next_seq` | Retained lower bound and next sequence to be written. |
| `has_more` | Additional entries in the requested direction. |
| `entries` | `seq`, 64-bit `boot_ms`, UTC `unix_ms` (null before sync), `message`. |

Read access follows the same trusted-network model as the SPD API. Log text may
contain installation names and device diagnostics; it contains no configured
secrets. See [API access](api.md) and [network updates](network-updates.md).
