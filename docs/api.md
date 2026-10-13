# Receiver API

Base URL: `http://RECEIVER_IP`. Dashboard polling and Home Assistant use the same
**`GET /api/v1/get`** response: all configured SPDs, including unseen devices and
the optional local contact, in configuration order. There is no pagination.

```sh
curl --fail http://RECEIVER_IP/api/v1/get
```

| Field | Contents |
| --- | --- |
| `api_version` | Schema version **1**; independent of the receiver firmware release. |
| `generated_at_ms` | Receiver monotonic time when the snapshot was built; not UTC. |
| `gateway` | Stable `gateway_id`, firmware identity, IP/uptime, radio settings, packet/rejection counters, alarm state and OTA/MQTT diagnostics. |
| `summary` | Configured, seen, OK, FAIL, unknown, stale and low-battery counts. The legacy `unknown` count includes stale rows. |
| `spds` | Every configured ID/name, effective status, last reported status, telemetry, report age, packet/loss statistics and diagnostics. |

Match rows by **`spd_id`**, not their array index. `UNKNOWN` means no accepted
report yet; `STALE` means the last report is too old. Neither represents a healthy
SPD. Missing telemetry is `null`; local contacts have no LoRa/battery readings.
Temperature values -128/-127/-126 are legacy ADC/open/short NTC sentinels;
the dashboard labels them as errors. Use status and availability rules from the
[Home Assistant guide](home-assistant.md) for notifications.

| Endpoint | Behavior |
| --- | --- |
| `GET /` | Dashboard. |
| `GET /api/v1/get` | One JSON snapshot; `Cache-Control: no-store`. |
| `GET /healthz` | Web-handler liveness: `{"status":"ok"}`; not a certificate of fresh SPD readings. |
| `POST /api/v1/mute?value=1` | Mute relay; `value=0` unmutes, omitted value toggles. Visual/status warnings continue. |
| `POST /api/v1/firmware` | Authenticated multipart update; [upload contract and recovery](network-updates.md). |

Reads and mute retain the existing trusted-network access model; firmware updates
use a separate private login and request header. No credentials or beacon keys
are returned by the API. This receiver provides HTTP, without TLS or CORS access.
