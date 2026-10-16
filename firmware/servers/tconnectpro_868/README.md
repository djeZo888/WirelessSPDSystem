# T Connect Pro 868 MHz receiver

Firmware for **LILYGO T Connect Pro V1.0**, ESP32-S3-R8, 16 MB flash / 8 MB OPI
PSRAM, 868 MHz SX1262, ST7796 LCD and CST226SE touch. It accepts both
[AC-DC](../../beacons/acdc_avr128da32) and
[battery](../../beacons/battery_avr128db32) beacon firmware using the
[20-byte authenticated protocol](../../../docs/protocol.md). The [API reference](../../../docs/api.md) documents the full-list response and controls.

Receiver firmware is **v0.2.5**, shown on the LCD as **v0.2.5-tconnpro**.

USB diagnostics are best-effort: a slow or missing serial reader cannot block
monitoring, mute controls or the web service. The **Logs** tab retains the session
log in RAM even when USB copies are dropped. Mute silences the relay; visual
warnings and packet reception continue.
See the [receiver changelog](CHANGELOG.md). This version is separate from beacon
HW/FW fields in LoRa packets and the repository's `v0.1.0` release.

The LCD shows **eight SPDs per page** and advances at the configured
`DISPLAY_PAGE_SECONDS` interval (**5 seconds** by default) when more than eight
are configured. Eight or fewer stay on one page. Up to **127
wireless beacons plus one local contact** can be configured. The optional local
contact is first, followed by beacons in `SPD_CONFIGS` order. Reception, alarms and the web
dashboard cover all configured SPDs, including those on another LCD page.
The first page interval starts after boot completes.

The web dashboard adapts to phones and desktops, with status summaries, search,
filters and expandable diagnostics. RSSI and SNR appear together; BAT is hidden
until a hardware-type-2 beacon is identified anywhere in the configured list. A persistent
MUTE/UNMUTE button controls alarm output from every tab; refresh timestamps
include date and 24-hour time. **Logs** shows startup, packet and operational
events from the current boot, newest first. It opens with 100 events; **Load more**
at the bottom adds each next 100 older events. See [session logging](../../../docs/session-logs.md).
The footer opens the full API as readable JSON in a new tab. See [Home Assistant setup](../../../docs/home-assistant.md)
for the full locally hosted **HA Integration** guide, MQTT discovery, REST
examples and a synchronized Home Assistant alarm mute switch, and [network firmware updates](../../../docs/network-updates.md)
for authenticated browser uploads after the first USB installation.

Station Wi-Fi restarts and retries every **60 seconds** by default after a lost
connection or IP lease (`WIFI_RETRY_INTERVAL_MS`: **60000–120000 ms**).
If boot cannot join the configured station within `WIFI_CONNECT_TIMEOUT_MS`, the
password-protected fallback AP stays available while station retries continue.
With no station SSID configured, the receiver uses AP-only mode. The LCD shows
**connecting** while a configured station has no address and no fallback AP
is ready (**offline** if no station is configured). The Logs tab records disconnect
reasons and reconnection/IP events; LoRa and local alarms keep running during
retries. A full Wi-Fi restart briefly cycles the fallback AP too. Recovery actions
wait until an active OTA upload ends.

## Build and flash

Install [Arduino CLI](https://arduino.github.io/arduino-cli/installation/),
Python 3.9+ and Git. Clone this repository and run these commands from its root:

```sh
git clone https://github.com/djeZo888/WirelessSPDSystem.git
cd WirelessSPDSystem
python3 tools/firmware.py setup server
cp firmware/servers/tconnectpro_868/config.example.h firmware/servers/tconnectpro_868/config.h
```

Edit **only `config.h`** before building; deployment settings belong there and
the `.ino` does not need editing:

| Setting | Required configuration |
| --- | --- |
| `SPD_CONFIGS` | One `{ ID, "name", key }` per beacon. Unique IDs **1–127** (production **10–127**) and nonzero four-byte keys must match the beacon configurations. |
| `LORA_FREQ_MHZ`, `LORA_SF` | Match every beacon: carrier **863.0625–869.9375 MHz**, SF **5–12**. Defaults **865.3 MHz / SF10**. Bandwidth, coding rate, sync word, preamble and CRC are fixed in the sketch. |
| `LORA_RX_TX_DBM` | Radio initialization power: integer **−9 to +22 dBm**, default **10**. Does not have to match beacon TX power; this receiver does not send acknowledgements. |
| `WIFI_SSID`, `WIFI_PASSWORD` | Optional station credentials. Empty SSID uses the fallback access point. |
| `WIFI_CONNECT_TIMEOUT_MS`, `WIFI_RETRY_INTERVAL_MS` | Initial join timeout (default **15000 ms**) and repeated Wi-Fi restart interval (**60000–120000 ms**, default **60000**). |
| `WIFI_STATIC_IP`, `WIFI_SUBNET_MASK` | Empty IP selects **DHCP**. Otherwise choose an unused LAN IPv4 address and a contiguous subnet mask. |
| `WIFI_GATEWAY`, `WIFI_DNS1`, `WIFI_DNS2` | Optional static-mode router/DNS addresses. Empty or `0.0.0.0` means none. A gateway is required to reach other subnets; DNS is required to resolve hostnames. |
| `FALLBACK_AP_SSID`, `FALLBACK_AP_PASSWORD` | Choose an AP name and a password of **8–63 characters**, even when using station Wi-Fi. |
| `SPD_LOCAL_ID` | Leave **-1** to disable the local input, or choose an unused ID **0–127**. ID 0 is available only for the local contact, allowing 128 total rows. |
| `DISPLAY_PAGE_SECONDS` | Seconds on each LCD page: **1–3600**, default **5**. The page size stays at eight rows. |
| `DISPLAY_PERIODIC_REFRESH_MS`, `TOUCH_*_MS` | Optional LCD refresh and touch timing adjustments; keep the example defaults unless needed. |
| `OTA_ENABLED`, `OTA_TOKEN` | Updates default to disabled. When enabled, configure a private **32–128 letter** token (`A–Z`, `a–z`); no username. |
| `SESSION_LOG_BYTES`, `NTP_SERVER` | Default **2 MiB** PSRAM session log; configurable **64 KiB–4 MiB**. NTP supplies dates; browser displays local 24-hour time. Empty NTP server disables clock sync. |
| `MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME`, `MQTT_PASSWORD` | Optional Home Assistant broker on the station LAN. An empty host disables MQTT; port 1–65535, default 1883. |
| `MQTT_BASE_TOPIC`, `MQTT_PUBLISH_SECONDS` | Discovery/state root (1–64 bytes, no `+` or `#`); publish interval 1–3600 seconds, default 10. |
| `CONFIGURED` | Set to **true** after completing the configuration. |

Use the same random four-byte secret for all beacons in the home and repeat it in their registrations. Existing per-beacon keys remain supported. Friendly
names identify the installation; the LCD displays their first 18 characters.
`config.h`, build images and private build manifests contain deployment secrets
and are ignored by Git. Keep them private.

All configuration limits are annotated in `config.example.h`; see the
[shared radio and per-SPD reference](../../../docs/radio-configuration.md).
Frequency and spreading factor must match: this SX1262 receives one of each at
a time. Existing configs must remove `LORA_BW_KHZ`, `LORA_CR`, `LORA_SYNCWORD`,
`LORA_PREAMBLE` and `LORA_CRC_ENABLED`, now fixed in the sketch.

When upgrading to v0.2.4, also add `WIFI_RETRY_INTERVAL_MS`, `WIFI_STATIC_IP`,
`WIFI_SUBNET_MASK`, `WIFI_GATEWAY`, `WIFI_DNS1` and `WIFI_DNS2` from the example.
Leave the static address empty to retain DHCP. Preserve all existing private settings.

For earlier upgrades, add the OTA and MQTT fields from `config.example.h` to
private `config.h`, preserving your Wi-Fi credentials, beacon list and display
settings. Replace the former OTA username/password fields with `OTA_TOKEN` and add the log/clock settings from the example. Keep updates disabled until a private token is configured. These settings are applied when compiling; rebuild
and flash after changing the page interval.

Connect the board's ESP32 USB port using a **data cable**. Find its port, build
your configured image, then upload:

```sh
arduino-cli --config-file .arduino/arduino-cli.yaml board list
python3 tools/firmware.py build server
python3 tools/firmware.py flash server --port /dev/cu.usbmodemYOUR_PORT
```

Replace the port with the detected device (`COMn` on Windows). The flash command
checks that source, configuration and image still match the private build. After
editing any configuration, build again before flashing. If upload cannot enter
the bootloader, hold **BOOT**, press/release **RESET**, release BOOT, then retry.
Open Serial Monitor at **115200 baud** for boot and packet diagnostics.

The setup script isolates pinned dependencies in ignored `.arduino/`:
**ESP32 core 3.3.12**, **RadioLib 7.8.1**, **GFX Library for Arduino 1.6.9** and **PubSubClient 2.8**.
Touch uses `Wire` directly; no separate touch library is needed.
`python3 tools/firmware.py validate server` compiles an isolated synthetic
configuration with 127 wireless beacons plus the local contact for development
checks; its images are never selected by `flash`. Run
`python3 tests/check_server_display.py` to check paging and off-page alarms, and
`python3 tests/check_server_api.py` to check the complete API/web list on the
host without a connected board. These development tests require a C++ compiler;
the API/dashboard tests also require Node.js. OTA host checks require OpenSSL
development headers; these are not device dependencies.

### Arduino IDE

Install those same core/library versions through Boards Manager and Library
Manager. Add `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
to Additional Boards Manager URLs for the ESP32 core.
Open [tconnectpro_868.ino](tconnectpro_868.ino) with your adjacent
`config.h`. Select the USB port and these board options, then **Upload**:

| Option | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| Flash | 16 MB, QIO 80 MHz |
| Partition Scheme | 16M Flash (3MB APP / 9.9MB FATFS) |
| PSRAM | OPI PSRAM |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Enabled |
| Upload Speed | 460800 |
| CPU Frequency | 240 MHz |

The CLI uses the exact FQBN in [build-options.json](build-options.json). Its
isolated packages are separate from an IDE using a different sketchbook/data
directory. Board settings and pin assignments follow the
[official LILYGO V1.0 documentation](https://github.com/Xinyuan-LilyGO/T-Connect-Pro#softwaredeployment).

## Display, network and API

At boot the receiver connects to station Wi-Fi, waiting up to 15 seconds by
default. If credentials are absent or connection fails, it starts the configured
fallback AP. Join that AP or the same station network, then open **`http://IP/`**
using the IP shown on the LCD/Serial. The dashboard refreshes every five seconds
and shows all SPDs in one table. One **`GET /api/v1/get`** request returns every
configured SPD, independent of the current LCD page or its switching interval;
there are no API pages to fetch. This firmware uses Wi-Fi; Ethernet is unused.

| HTTP endpoint | Purpose |
| --- | --- |
| `GET /` | Dashboard and MUTE/UNMUTE controls. |
| `GET /api/v1/get` | JSON: `gateway` diagnostics, `summary` counts and every entry in `spds`. Includes status, telemetry, age, nonce, raw packet and packet-loss statistics. |
| `POST /api/v1/mute?value=1` | Mute the relay; use `value=0` to unmute. Omitting `value` toggles mute. |
| `POST /api/v1/firmware` | Authenticated, checked multipart application update; see the network update guide. |
| `GET /healthz` | Returns `{"status":"ok"}` while the web handler is running. |

The HTTP server has no TLS or login; dashboard reads and mute are unauthenticated. Firmware uploads require their
separate private login and request header.
Use it on a trusted network.

## State and alarms

Wireless rows are **UNKNOWN** until their first accepted packet, then **OK** or
**FAIL**. They become **STALE** after more than `STALE_AFTER_SECONDS` without a
packet (default **700 s**). Packet loss is inferred from nonce gaps using a
rolling window (default **1,000 expected packets** per beacon).
The default loss window uses about 1 KB of RAM per wireless beacon. The supported
ID count is a configuration limit; check reception and web response times with
your installation's beacon count and reporting intervals.

The receiver keeps the latest reading, nonce and loss window for each beacon
**in RAM only**. It does not store a persistent database or a full packet log.
Receiver reboot clears this state. Both beacon hardware versions at firmware 1
retain their nonce in EEPROM across normal power cycles. Migrating RAM-only
firmware or replacing/erasing beacon EEPROM requires a safe start above the last
received nonce, or a receiver restart to clear its RAM history; see
[persistent nonce notes](../../../docs/nonce-storage.md).

With the default settings:

| Condition | Indication |
| --- | --- |
| Any fresh FAIL, including local input | Red LCD flashing; relay active **5 s per minute**. |
| Any fresh battery reading below **2.95 V** | Highlighted row and relay active **500 ms per minute**. FAIL takes priority. |
| UNKNOWN or STALE | Informational status; no relay alarm. |

MUTE disables the relay while status and visual warnings continue; it does not
persist across receiver reboot. Alarms are
evaluated across the whole configuration, independent of the current LCD page.
Relay periods and the battery threshold are configurable in `config.h`.

The optional local input uses **3.3 V → isolated SPD dry contact → GPIO15**:
closed/HIGH means OK, open/LOW means FAIL. Default debounce is 250 ms. Connect
only the isolated contact; a live voltage must not be applied to this input.

## Board pin map and first check

| Function | ESP32-S3 GPIOs |
| --- | --- |
| Shared LCD / LoRa SPI | MOSI **11**, MISO **13**, SCK **12** |
| LCD | CS **21**, DC **41**, backlight **46** |
| SX1262 | CS **14**, RESET **42**, BUSY **38**, DIO1 **45** |
| Touch | SDA **39**, SCL **40**, RESET **47**, interrupt **3** |
| Relay | **8**, active LOW |
| Optional local contact | **15**, active HIGH |

Fit the intended LoRa antenna. Confirm the boot log completes, the LCD and web
dashboard show every configured ID/name, and a genuine contact transition
changes OK/FAIL. Check temperature, battery readings and relay/mute behavior.
For more than eight rows, verify every page appears and an off-page FAIL still
raises the alarm. A rejected packet in Serial usually identifies mismatched
radio settings, an unregistered ID, a wrong key or an old nonce; compare both
device configurations. Follow the migration notes when replacing or erasing
beacon EEPROM.
