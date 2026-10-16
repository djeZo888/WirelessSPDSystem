# Receiver regression tests

Run these checks for receiver changes. New functionality needs a matching test;
removed functionality needs its obsolete UI, API and configuration checks removed
or changed to verify rejection. CI runs the host suites before a release.

## Main-loop scenarios

```sh
python3 tests/check_server_scenarios.py
```

Requires Python 3 and a C++ compiler supporting AddressSanitizer and
UndefinedBehaviorSanitizer. No device, credentials or optional Python packages
are required.

The harness executes the production receiver loop, mute handlers, touch debounce,
packet decoder/tag verifier, relay control, session logging and HTTP routes.
It uses 17 wireless registrations, with and without the local input, and a
deterministic clock. This covers simultaneous work, rather than testing the
controls with logging replaced by an empty stub.

| Scenario | Required result |
|---|---|
| One fresh OK and one fresh FAIL, including while muted | FAIL remains visible; red/white blinking, packet reception and HTTP continue. |
| Web mute/unmute, repeated explicit values and toggle | State is correct; mute turns the relay off; another HTTP request is served on the next iteration. |
| Touch press, held press, brief empty sample, missing sample, bounce and press outside the button | One action per deliberate, debounced press; no repeated toggling during a held press. |
| Home Assistant mute/unmute callback | Uses the same relay, log and display behavior as web/touch control. |
| Web action and received packet in the same iteration | Both finish; the radio returns to receive mode and the next API snapshot includes the packet. |
| USB absent, attached but unread, full, limited capacity, draining, partial/zero write, and capacity changing before the write | No blocking retry; at most one 64-byte diagnostic write per iteration. |
| Diagnostic queue full or oversized messages, wraparound and reader return | Whole lines may be dropped from optional USB diagnostics; session records remain available through the log API. Remaining queued bytes retain FIFO order. |
| Authenticated hardware 1/2, OK/FAIL, low battery, battery error sentinel, skipped reports, stale boundary and recovery | Status, alarm priority, telemetry, counters and complete API rows match accepted reports. Stale/unknown entries do not alarm. |
| Fault on another LCD page | The fault alarms globally; paging and web/API access continue. |
| Wrong packet length, wrong tag, unconfigured ID, repeated/old nonce, radio read failure and RX restart failure | Rejection is logged, accepted state is preserved and subsequent work continues. |
| First nonce zero, maximum nonce and wrapped/repeated values | First zero and the first maximum are accepted; accepted nonce never moves backwards. |
| Snapshot, logs, health, dashboard, wrong-method/unknown routes and malformed log queries | Complete JSON/HTML responses or the documented 400/404 response; rejected reads cannot change mute state. |
| Local input debounce and fault recovery | Correct alarm priority and mute behavior alongside wireless beacons. |

Every iteration has a 100 ms **simulated blocking budget** and assertions that
blink, HTTP and radio work make progress. A negative control restores the old
synchronous USB logger and default transmit timeout: the same test must detect
the resulting multi-second stall. This guards against accidentally stubbing out
the path that caused the regression.

## Focused suites

| Test | Additional coverage |
|---|---|
| `check_server_api.py` | Complete list/summary and log pagination across 13 configurations, up to all 128 rows; pretty JSON and integration downloads. |
| `check_server_display.py` | Actual renderer geometry, battery-column layouts, paging, footer and touch behavior. |
| `check_server_dashboard.py` | Browser script, filtering/escaping, network failures, controls, firmware upload and offline guide embedding. |
| `check_server_mqtt.py` | Actual broker client state machine, discovery, strict command validation, retained-command protection, reconnects and synchronized state. |
| `check_server_wifi.py` | DHCP/static configuration, disconnect/restart recovery and time/attempt boundaries. |
| `check_server_logs.py` | 2 MiB ring retention, wrap, timestamp reconstruction, bounded UTF-8 messages and JSON formatting. |
| `check_server_ota.py` | Authentication, image validation, upload failures, monitoring pause and reboot lifecycle. |
| `check_packet_compatibility.py` | Actual TX encoding and SHA-256/RX verification against independent vectors and every-byte tampering. |
| `check_home_assistant_guide.py` | Offline guide generation and YAML/template examples. |

Run each with `python3 tests/<filename>`, except the Home Assistant guide check
needs the pinned dependencies shown in the CI workflow. Packet compatibility
needs the pinned beacon dependencies installed by
`python3 tools/firmware.py setup acdc`.

## Device release checks

Host shims cannot measure real LCD rendering, socket latency, USB-driver behavior,
RF reception or physical relay operation. Before deploying a receiver change:

1. Build with both supported ESP32 core versions and the documented T-Connect Pro
   board settings. Keep deployment credentials out of tracked files and artifacts.
2. With one reporting OK beacon and one reporting FAIL beacon, close the serial
   monitor but leave USB connected. Repeatedly mute/unmute through web and touch.
   Confirm the red display continues blinking and web/API requests remain prompt.
3. Repeat with serial monitoring open, then close/reopen it. Confirm both real
   beacon counters/nonces continue increasing and session logs retain events.
4. Verify the installed version and the intended registration list. Test actual
   MQTT controls when that feature changes. For changes to Wi-Fi, OTA, paging or
   battery handling, repeat the corresponding physical acceptance checks.
5. Record measured results and any untested physical/site behavior in
   [validation](validation.md). A passing host test alone is not physical
   qualification of an alarm output or installation.

The current HTTP transport is a synchronous Arduino WebServer. Its handling of a
slow or incomplete client connection can still delay the receiver loop; host
route tests do not simulate socket timeouts. The USB regression checks establish
that optional diagnostics add no blocking delay, rather than guaranteeing a
100 ms wall-clock response for every network condition.
