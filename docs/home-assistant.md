# Home Assistant setup

This guide connects the T Connect Pro receiver to Home Assistant, creates SPD
entities, adds alarm mute control and notifications, and checks the result.
**MQTT Discovery is recommended:** it creates the devices automatically. The
REST alternative needs no broker and reads the complete SPD list in one request.

The full guide is stored inside the receiver at **`/ha-guide`** and embedded in
its **HA Integration** tab. Save that page as HTML, and download **`/ha-rest.yaml`**
for the REST example. All instructions and example code below can be read
without Internet; external reference links are optional. Installing Home
Assistant, broker/editor apps, container images, Arduino packages or phone apps
may require Internet. Download/install those prerequisites before an offline
installation. This guide assumes Home Assistant is already installed and its
initial owner account is working; it does not provision a new host operating system.

Use **Home Assistant Core 2025.8 or newer** for the complete guide, including the
REST mute switch, and prefer a current supported release. Check the Core version
in System information before installation. MQTT device discovery alone is
available from Core 2024.11.
[Device discovery introduction](https://www.home-assistant.io/blog/2024/11/06/release-202411/) ·
[REST template switch support](https://github.com/home-assistant/core/blob/2025.8.0/homeassistant/components/template/config.py)

## 1. Prepare the network and tools

You need the receiver and its LoRa antenna, the correct private beacon IDs/keys,
a 2.4 GHz Wi-Fi LAN, Home Assistant administrator access, and a computer with
this repository plus Arduino CLI and the Arduino dependencies already installed.
MQTT also needs one private broker. For REST, arrange access to Home Assistant's configuration
files. Back up the Home Assistant configuration and receiver `config.h` first.

**Before moving offline**, on the computer that will build the firmware, obtain
this repository and Arduino CLI, then run this from the repository root while
Internet is available:

```sh
python3 tools/firmware.py setup server
```

This setup command always refreshes online package indexes; do not run it as an
offline build step. Keep the complete checkout and its ignored **`.arduino/`**
folder, including installed packages/libraries, on that same computer. Keep
Arduino CLI available too. Also install Home Assistant, the broker, any File
editor app and optional phone app before moving offline. For Arduino IDE instead
of the CLI, install the exact core/libraries listed in section 4 beforehand.
The later build/upload commands use these prepared local dependencies.

For the separate Docker broker path, also fetch its image **on the Linux broker
host while online**, before running section 2B at the installation:

```sh
docker pull eclipse-mosquitto:2
docker image inspect eclipse-mosquitto:2 >/dev/null
```

Keep that image on the broker host. Section 2B uses `--pull never` so an absent
image fails locally instead of attempting an Internet download; if it is absent,
return online and complete this preparation first.

In Home Assistant open **Settings → System → Repairs → ⋮ → System information**
and read the installation type. **Home Assistant OS** supports apps, formerly
called add-ons. **Home Assistant Container** has no app store; run a separate
broker. A legacy Core/Python installation follows the separate-broker path.
The current supported installation choices are OS and Container.
[Installation types](https://www.home-assistant.io/installation/)

Use one LAN for the receiver, broker and Home Assistant. Guest Wi-Fi/client
isolation or VLAN rules may prevent them communicating. The receiver's fallback
access point alone has no route to a broker on your home LAN. MQTT requires
station Wi-Fi; REST also needs Home Assistant to reach the receiver's IP.

Find the Home Assistant/broker IP and the receiver IP in your router's client
list. The receiver LCD and API also show its address. Create **DHCP reservations**
for the broker and receiver in the router, using their listed MAC addresses.
Router menu names differ; use its DHCP/LAN reservation function. Use the broker's
LAN IP, not `127.0.0.1`, `localhost`, a Docker-internal address or
`core-mosquitto` in the ESP32 configuration. Examples below use broker
**192.168.1.20** and receiver **192.168.1.30**; replace both with yours.

## 2A. Broker on Home Assistant OS

If you already have a working broker, retain it and proceed to section 3.
If Mosquitto is not installed, **while online** open **Settings → Apps → Install
app**, find **Mosquitto broker**, and install it before moving offline. Older Home
Assistant versions label this **Settings → Add-ons → Add-on Store**. At the
installation open the **already installed** Mosquitto app, enable **Start on boot**,
and start it. Check its **Log** tab and keep TCP port **1883** exposed to the LAN
in its Network settings. Starting/configuring the installed app needs no app download.

Create a device account: open your user profile and enable **Advanced mode** if
needed; open **Settings → People → Users → Add user**. Use display name
**Wireless SPD receiver**, username **wirelessspd**, and a strong unique password.
Leave **Administrator OFF** and keep the account active. It needs no associated
person or location tracking. Never reuse the owner account. The Mosquitto app
accepts these Home Assistant user credentials. Do not use reserved usernames
`homeassistant` or `addons`.
[Official Mosquitto app instructions](https://github.com/home-assistant/addons/blob/master/mosquitto/DOCS.md)
[User account permissions](https://www.home-assistant.io/docs/configuration/user-configuration/)

## 2B. Separate broker for Container or legacy Core

Home Assistant users are **not** automatically broker users on a separate
Mosquitto server. Create broker logins instead: `wirelessspd` for the receiver
and `ha_mqtt` for Home Assistant. Use an existing managed broker if available.
The following complete example is for a **Linux Docker host with Docker already
installed**, not the Home Assistant OS app or a default macOS Docker setup.

In a new directory, create `config/` and `data/`, then save this as
`config/mosquitto.conf`:

```text
listener 1883
allow_anonymous false
password_file /mosquitto/config/passwords
persistence true
persistence_location /mosquitto/data/
log_dest stdout
```

Run these commands in that directory. The first two account commands ask for
passwords interactively; `-c` creates the file only on the first command. Do not
repeat `-c` when adding the second user, because it would replace the first.
Use an unused container name and port; keep any existing broker running until
its settings have been recorded.

```sh
mkdir -p config data
docker run --pull never --rm -it --user 0 -v "$PWD/config:/mosquitto/config" eclipse-mosquitto:2 mosquitto_passwd -c /mosquitto/config/passwords wirelessspd
docker run --pull never --rm -it --user 0 -v "$PWD/config:/mosquitto/config" eclipse-mosquitto:2 mosquitto_passwd /mosquitto/config/passwords ha_mqtt
sudo chown -R 1883:1883 config data
sudo chmod 600 config/passwords
docker run --pull never -d --name wireless-spd-mqtt --restart unless-stopped -p 1883:1883 -v "$PWD/config:/mosquitto/config" -v "$PWD/data:/mosquitto/data" eclipse-mosquitto:2
docker logs wireless-spd-mqtt
```

Use the Linux host's LAN IP for both clients. Permit TCP 1883 between those LAN
clients and the broker; do not forward it to the Internet. This example grants
an authenticated broker login access to all topics. If you already use ACLs,
the receiver needs **write** access to `wirelessspd/<gateway_id>/#` and its
`homeassistant/device/<gateway_id>*/config` topics, and **read** access to
`homeassistant/status` and its own command topic. It also writes its command
topic to clear an old retained command. Expand patterns into the exact topics
in your ACL's syntax; do not assume shell-style `*` is an MQTT wildcard.
[Official image](https://hub.docker.com/_/eclipse-mosquitto)
[Broker configuration](https://mosquitto.org/man/mosquitto-conf-5.html)
[Password-file tool](https://mosquitto.org/man/mosquitto_passwd-1.html)

## 3. Add MQTT to Home Assistant

Open **Settings → Devices & services → Integrations**. If Mosquitto appears under
**Discovered**, choose **Configure** and complete its prompts. Otherwise choose
**Add integration → MQTT**, enter the broker LAN IP and **1883**, and its Home
Assistant broker login. On OS the discovered setup manages its own integration
credentials; on the separate broker use **ha_mqtt**, not the receiver account.
Use TCP without TLS for this port; retain the integration's generated client ID.

On the MQTT integration card, open **Configure MQTT options** (some versions:
**Configure → Re-configure MQTT → Next**). Keep **discovery enabled**, discovery
prefix **homeassistant**, birth topic **homeassistant/status**, and birth payload
**online**. The receiver subscribes to that birth message to resend discovery.
Do not create individual MQTT sensors or a custom integration: this firmware
publishes their definitions.
[Home Assistant MQTT setup](https://www.home-assistant.io/integrations/mqtt/)

## 4. Configure and install the receiver image

Edit only **`firmware/servers/tconnectpro_868/config.h`**. For a fresh checkout,
copy adjacent `config.example.h` first; it contains intentionally invalid beacon
keys until you configure it. On an existing installation, add missing fields
from the current example and preserve Wi-Fi, all beacon registrations/keys,
radio, display, local-contact, log and update settings. Do not overwrite your
working private file with the example.

```cpp
static const char *WIFI_SSID = "YOUR_2_4_GHZ_LAN";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static const char *MQTT_HOST = "192.168.1.20";
constexpr auto MQTT_PORT = 1883;
static const char *MQTT_USERNAME = "wirelessspd";
static const char *MQTT_PASSWORD = "YOUR_DEVICE_ACCOUNT_PASSWORD";
static const char *MQTT_BASE_TOPIC = "wirelessspd";
constexpr auto MQTT_PUBLISH_SECONDS = 10;
```

Keep a valid fallback AP password of 8–63 characters. Each `SPD_CONFIGS` entry
must match its transmitter ID and nonzero four-byte secret. Frequency/SF must
match every beacon: defaults **865.3 MHz / SF10**. IDs 1–9 are for pre-production,
10–127 for production. Choose a stale threshold longer than the beacon's normal
reporting interval; the default **700 seconds** accommodates 5–10 minute reports.
Set **`CONFIGURED = true`** after filling the complete configuration. An empty
`MQTT_HOST` disables MQTT; a configured unreachable broker does not stop LoRa
or the local alarm. MQTT uses unencrypted TCP, and HTTP uses no TLS. Keep both
on your trusted LAN.

With the section 1 prerequisites already prepared, run the following from the
repository root to build and upload; these steps need no package downloads:

```sh
python3 tools/firmware.py build server
arduino-cli --config-file .arduino/arduino-cli.yaml board list
python3 tools/firmware.py flash server --port /dev/cu.usbmodemYOUR_PORT
```

If dependencies are missing, return online and complete the setup in section 1
before trying an offline build. Replace the USB port
with the board's detected port (`COMn` on Windows). Use a data cable. If the
bootloader does not start, hold BOOT, press/release RESET, release BOOT and retry.
Serial diagnostics use **115200 baud**. The build/flash helper checks that your
private configuration and image still match; rebuild after any edit.

In Arduino IDE use **ESP32 core 3.3.12**, **RadioLib 7.8.1**, **GFX Library for
Arduino 1.6.9**, and **PubSubClient 2.8**. Open the `.ino` with its adjacent config:

| Board option | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| Flash size / mode / frequency | 16 MB / QIO / 80 MHz |
| Partition Scheme | 16M Flash (3MB APP / 9.9MB FATFS) |
| PSRAM | OPI PSRAM |
| USB Mode / CDC On Boot | Hardware CDC and JTAG / Enabled |
| CPU / upload speed | 240 MHz / 460800 |

For an already OTA-enabled receiver, build the same application and open its
**Firmware** tab. Select **`tconnectpro_868.ino.bin`**, not a merged image,
bootloader or partition file; enter its installed update token and upload.
Monitoring pauses and the relay is OFF during updating/restart. The new image
contains the new configuration; retain `OTA_ENABLED` and the private `OTA_TOKEN`
if you want subsequent network updates. Current tokens use 32–128 ASCII letters
and no username. If the installed older image still asks for username/password,
use that image's old credentials for the one upgrade, or use USB. USB is also
the recovery path if the image boots incorrectly. Firmware binaries contain
credentials: keep them and `config.h` private.

## 5. Check discovered devices and add controls

After the receiver boots on your LAN, open its dashboard. In **HA Integration**
check that MQTT is enabled and connected. Open **`/api/v1/get`** and record
**`gateway.gateway_id`**. Its format is `wspd-` followed by twelve hexadecimal
hardware-identity digits; for example `wspd-aabbccddeeff`. It is independent of
the IP address. No keys/passwords are exposed by the read-only API.

In Home Assistant, open the MQTT integration's **Devices** list. Expect one
**Wireless SPD receiver** device and one device for **each configured SPD**,
including beacons not received yet. Wait roughly 30–60 seconds for a first
connection/discovery cycle. Friendly names come from `SPD_CONFIGS`.

| Device | Entities |
| --- | --- |
| Receiver | Configured, faulty, stale and unknown counts; uptime; alarm-muted diagnostic; **Alarm muted switch**. |
| Wireless SPD | Status, fault, temperature, battery voltage, RSSI, SNR, report age and packet loss. |
| Optional local contact | Status and fault; no radio/temperature/battery entities. |

Open each fault entity and its settings/gear to read its **entity ID**. Friendly
names can produce different IDs, so do not guess them from the numeric beacon
ID. For the three-SPD notification examples below, rename the selected fault
entities to **`binary_sensor.spd_1_fault`**, **`binary_sensor.spd_2_fault`**, and
**`binary_sensor.spd_3_fault`**, or replace both entity lists in each example with
your real IDs. Include every SPD you intend to monitor. With multiple receivers,
use distinct names such as `binary_sensor.garage_spd_1_fault`.

Open the receiver's **Alarm muted** switch. **ON = relay muted; OFF = relay
allowed to sound according to its configured alarm pattern.** Turning it off
does not force an alarm when no fresh fault exists. Add it to a dashboard from
its entity dialog or an Entities card. The LCD, web MUTE/UNMUTE button and this
switch control the same flag. Monitoring, red warnings and notifications
continue when muted. State feedback normally arrives within the publish interval
(default 10 s), often sooner; do not treat an unavailable switch as OFF. Muting
is cleared by receiver reboot. MQTT commands are nonretained, preventing an old
command being replayed on reconnect.

**Status** reads OK, FAIL, UNKNOWN or STALE. Fault is ON only for fresh FAIL and
OFF only for fresh OK. UNKNOWN/STALE fault entities are **unavailable**, not
healthy. Invalid/missing numeric readings are unavailable; telemetry needs a
fresh report, while age/loss diagnostics remain useful after a beacon was seen.
Gateway loss/OTA pause also makes the entities unavailable. Receiver restart
clears RAM readings and they return to UNKNOWN until new authenticated reports.

## 6. Add fault and missing-monitoring notifications

The following examples create Home Assistant sidebar notifications and require
no phone or cloud. They do not command the physical relay. To install each one,
open **Settings → Automations & scenes → Create automation → Create new
automation → ⋮ → Edit in YAML**, replace the editor contents with one complete
block, adjust both entity lists, then **Save** and enable it. Paste the second
block into a **separate** new automation. These blocks have no outer `automation:`
key because they are for the single-automation editor.
[Automation editor](https://www.home-assistant.io/docs/automation/editor/)

The first alerts on a fresh fault, with five seconds of debounce, and also
checks existing faults at Home Assistant startup:

<!-- ha-example:fault-notification.yaml -->
```yaml
# Paste this single automation into a NEW automation's Edit in YAML editor.
# First rename/select your real fault entities; change both lists consistently.
alias: Wireless SPD - fault notification
mode: restart
variables:
  spd_fault_entities:
    - binary_sensor.spd_1_fault
    - binary_sensor.spd_2_fault
    - binary_sensor.spd_3_fault
triggers:
  - trigger: state
    entity_id:
      - binary_sensor.spd_1_fault
      - binary_sensor.spd_2_fault
      - binary_sensor.spd_3_fault
    to: "on"
    for: "00:00:05"
  - trigger: homeassistant
    event: start
conditions:
  - condition: template
    value_template: >-
      {{ expand(spd_fault_entities) | selectattr('state', 'eq', 'on') | list | count > 0 }}
actions:
  - action: persistent_notification.create
    data:
      notification_id: wireless_spd_fault
      title: "SPD fault"
      message: >-
        Fault reported by: {{ expand(spd_fault_entities) |
        selectattr('state', 'eq', 'on') | map(attribute='name') | join(', ') }}.
        Check the receiver and the affected SPD. Muting silences the relay only;
        the fault remains active until the SPD reports healthy.
```
<!-- /ha-example:fault-notification.yaml -->

The second alerts after a fault entity stays unavailable/unknown for a minute;
it also waits a minute after Home Assistant startup before checking missing
entities. This is a separate monitoring warning and must not be interpreted as
proof of SPD failure or health:

<!-- ha-example:unavailable-notification.yaml -->
```yaml
# Paste into a SECOND new automation's Edit in YAML editor.
# Change both entity lists to match every SPD fault entity you intend to monitor.
alias: Wireless SPD - missing monitoring notification
mode: restart
variables:
  spd_fault_entities:
    - binary_sensor.spd_1_fault
    - binary_sensor.spd_2_fault
    - binary_sensor.spd_3_fault
triggers:
  - trigger: state
    entity_id:
      - binary_sensor.spd_1_fault
      - binary_sensor.spd_2_fault
      - binary_sensor.spd_3_fault
    to: "unavailable"
    for: "00:01:00"
  - trigger: state
    entity_id:
      - binary_sensor.spd_1_fault
      - binary_sensor.spd_2_fault
      - binary_sensor.spd_3_fault
    to: "unknown"
    for: "00:01:00"
  - trigger: homeassistant
    event: start
conditions: []
actions:
  - if:
      - condition: template
        value_template: "{{ trigger.platform == 'homeassistant' }}"
    then:
      - delay: "00:01:00"
  - condition: template
    value_template: >-
      {{ expand(spd_fault_entities) | selectattr('state', 'in', ['unknown', 'unavailable']) |
         list | count > 0 or expand(spd_fault_entities) | list | count < spd_fault_entities | count }}
  - action: persistent_notification.create
    data:
      notification_id: wireless_spd_monitoring_missing
      title: "SPD monitoring unavailable"
      message: >-
        At least one configured fault entity is unknown, unavailable or missing.
        Check receiver power, Wi-Fi/MQTT, beacon reporting and the selected entity IDs.
        Missing monitoring is not a healthy-SPD confirmation.
```
<!-- /ha-example:unavailable-notification.yaml -->

Read the notifications from Home Assistant's sidebar notification/bell area.
They remain until you dismiss them; dismiss only after checking the installation.
For a no-hardware acceptance test, open **Developer tools → Actions**, choose
**Persistent notification: Create**, provide a title/message and run it. To test
an automation's action path, use its **Run actions** menu; this bypasses triggers
and conditions and does not prove live fault detection.
[Persistent notifications](https://www.home-assistant.io/integrations/persistent_notification/)

For optional phone push, install/configure the Home Assistant Companion app,
allow notifications, and sign it into your instance. In **Developer tools →
Actions** find its actual `notify.mobile_app_...` action and send a test message.
Append this action to the first automation's `actions:` list and replace the
placeholder with that action ID. Phone push may need Internet/push services;
verify it separately rather than relying on it for offline local alerts:

<!-- ha-example:mobile-notification-action.yaml -->
```yaml
# Optional extra ACTION inside the fault automation's actions list.
# Replace notify.mobile_app_your_phone with your actual action from Developer tools.
- action: notify.mobile_app_your_phone
  data:
    title: "SPD fault"
    message: >-
      Fault reported by: {{ expand(spd_fault_entities) |
      selectattr('state', 'eq', 'on') | map(attribute='name') | join(', ') }}.
```
<!-- /ha-example:mobile-notification-action.yaml -->

For email or another notifier, set up that notification integration first,
verify its action in Developer tools, and use that action instead. The built-in
persistent notification remains a usable local alternative.
[Companion notifications](https://companion.home-assistant.io/docs/notifications/notifications-basic/)

## 7. Acceptance checks

1. Compare the dashboard, API and Home Assistant device list: every configured
   SPD ID/name should be present, even if it is UNKNOWN. The API returns all of
   them in one response, independent of LCD pages.
2. Wait for a normal beacon report. Its status and radio telemetry should update
   in Home Assistant within the MQTT publish interval after reception. Production
   reports may be 5–10 minutes apart; your pre-production beacon may be faster.
3. Using only the intended isolated SPD contact, verify a known healthy/fault
   transition changes receiver status and the corresponding HA fault entity.
   An unconnected contact can legitimately report FAIL. Do not inject live
   voltage into a contact input.
4. Test the Alarm muted switch ON, then OFF; compare the web/LCD mute indicator.
   Leave the desired final state and confirm red warnings remain during FAIL.
5. Confirm both automations' action paths and, if installed, phone delivery.
   Observe a real contact change to test the complete live trigger path.
6. During an agreed interruption, stop beacon reporting long enough to exceed
   the stale threshold. Status becomes STALE and fault unavailable. Restoring
   reports restores availability. Separately check receiver/network loss and
   reconnection; allow the MQTT keepalive/expiry time rather than expecting an
   instantaneous transition. Do not perform this during an essential alarm duty.

These checks verify the configured integration; no software-only test establishes
physical SPD protection, relay wiring or notification delivery for your site.

## 8. Troubleshooting

| Symptom | Check/action |
| --- | --- |
| Receiver MQTT disabled | `MQTT_HOST` was empty in the flashed image. Rebuild with its broker settings. |
| MQTT enabled but disconnected | Receiver must be on station Wi-Fi; check broker LAN IP/1883, login, broker log, guest isolation/VLAN/firewall, and DNS. Try a reserved IPv4 address. Retries back off 30 s. |
| MQTT connected, no devices | HA must use the same broker with discovery prefix `homeassistant` and birth topic `homeassistant/status`. In the MQTT integration's Configure page, listen to `homeassistant/device/#` and `wirelessspd/#`. Check broker ACLs. |
| Devices exist, fault unavailable | UNKNOWN/STALE or gateway offline is intentional. Check receiver Logs, beacon ID/key, frequency/SF, reporting interval and stale threshold. Invalid battery/temperature affects only that telemetry entity. |
| Temperature unavailable | Check ADC/NTC fault labels on the receiver; sentinels −128/−127/−126 are errors, not valid temperatures. |
| No notification | Check selected entity IDs, automation enabled state and Traces, trigger debounce, and the tested notifier action. Run actions tests only actions. |
| Mute switch unavailable/stale | Check gateway availability and MQTT. For REST wait for the next poll; request failure makes it unavailable. ON is muted. |
| Duplicate old devices | Old discovery remains retained after removing an SPD or changing topics; use section 10 to remove only the retired exact topics. |
| REST snapshot unavailable | Open the API from another LAN client, check reserved IP and secret URL; an HTTP timeout makes the source unavailable. |

## 9. REST alternative, including mute control

Choose REST instead of MQTT if you do not want a broker. Leave `MQTT_HOST` empty;
keep station Wi-Fi configured. Download **`/ha-rest.yaml`** locally or use the
complete block below. It performs **one GET every 10 s for the whole SPD list**,
creates example status/fault entities for ID 1, and adds an alarm-muted switch
using explicit POST mute/unmute commands. Template entities add no extra polls.

Open Home Assistant's configuration folder. On OS, open the **already installed
File editor** app (or another installed editor), then open
`/config/configuration.yaml` and adjacent `secrets.yaml`. If no editor is installed,
install one from Apps while online before proceeding offline. On Container edit
the host directory mounted at
`/config`. Its location is listed under System information. Back up the files.
In `secrets.yaml` add these two values using your receiver's reserved IP; the
base URL has **no trailing slash**:

```yaml
wireless_spd_api_url: "http://192.168.1.30/api/v1/get"
wireless_spd_base_url: "http://192.168.1.30"
```

Merge the block into `configuration.yaml`. **Do not duplicate** `rest:`,
`rest_command:`, `template:` or `recorder:` keys. If a key already exists, add its
new list/map entries under that existing key with correct indentation. If it
uses `!include`, edit the included file instead and omit that file's outer key
as appropriate. Leave your other integrations and `default_config:` intact.
The optional Recorder exclusion prevents saving the large aggregate snapshot on
every poll; individual entities still retain their history.

<!-- ha-example:rest-example.yaml -->
```yaml
# Merge these keys into configuration.yaml; do not create duplicate top-level keys.
# In secrets.yaml set wireless_spd_api_url and wireless_spd_base_url (see guide).
# One GET supplies every SPD. Templates and the mute switch do not add polling.
rest:
  - resource: !secret wireless_spd_api_url
    scan_interval: 10
    timeout: 5
    sensor:
      - name: "Wireless SPD snapshot"
        unique_id: wireless_spd_snapshot
        value_template: "{{ value_json.summary.configured }}"
        json_attributes:
          - gateway
          - summary
          - spds

rest_command:
  wireless_spd_mute:
    url: "{{ receiver_url }}/api/v1/mute?value=1"
    method: POST
    timeout: 5
  wireless_spd_unmute:
    url: "{{ receiver_url }}/api/v1/mute?value=0"
    method: POST
    timeout: 5

template:
  - sensor:
      - name: "SPD 1 status"
        unique_id: wireless_spd_1_status
        availability: "{{ has_value('sensor.wireless_spd_snapshot') }}"
        state: >-
          {% set rows = state_attr('sensor.wireless_spd_snapshot', 'spds') | default([], true) %}
          {% set matching = rows | selectattr('spd_id', 'eq', 1) | list %}
          {{ matching[0].status if matching else 'UNKNOWN' }}
    binary_sensor:
      - name: "SPD 1 fault"
        unique_id: wireless_spd_1_fault
        device_class: problem
        availability: >-
          {% set rows = state_attr('sensor.wireless_spd_snapshot', 'spds') | default([], true) %}
          {% set matching = rows | selectattr('spd_id', 'eq', 1) | list %}
          {{ has_value('sensor.wireless_spd_snapshot') and matching | count > 0
             and matching[0].status in ['OK', 'FAIL'] }}
        state: >-
          {% set rows = state_attr('sensor.wireless_spd_snapshot', 'spds') | default([], true) %}
          {% set matching = rows | selectattr('spd_id', 'eq', 1) | list %}
          {{ matching | count > 0 and matching[0].status == 'FAIL' }}
    switch:
      - name: "Wireless SPD REST alarm muted"
        unique_id: wireless_spd_rest_alarm_muted
        availability: >-
          {{ has_value('sensor.wireless_spd_snapshot') and
             state_attr('sensor.wireless_spd_snapshot', 'gateway') is mapping and
             state_attr('sensor.wireless_spd_snapshot', 'gateway').alarm_muted is boolean }}
        state: >-
          {% set gateway = state_attr('sensor.wireless_spd_snapshot', 'gateway') | default({}, true) %}
          {{ gateway.get('alarm_muted', false) }}
        turn_on:
          - action: rest_command.wireless_spd_mute
            data:
              receiver_url: !secret wireless_spd_base_url
        turn_off:
          - action: rest_command.wireless_spd_unmute
            data:
              receiver_url: !secret wireless_spd_base_url

# Optional: keep the large aggregate snapshot out of history. Individual entities
# retain their history. Merge with your existing recorder.exclude.entities list.
recorder:
  exclude:
    entities:
      - sensor.wireless_spd_snapshot
```
<!-- /ha-example:rest-example.yaml -->

For SPD 2 or 3, copy the `sensor` and `binary_sensor` entries, change each name
and `unique_id`, and change the numeric ID in **every** `selectattr('spd_id',
'eq', 1)` expression. Match by ID, never by array position. Add the new entries
under the same lists. Do not duplicate the receiver-wide mute switch. All raw
telemetry remains available in the snapshot sensor's `spds` attribute; MQTT is
the easier route when you want automatic separate telemetry entities for every SPD.

Open **Developer tools → YAML → Check configuration** (enable Advanced mode if
that view is hidden). Fix errors before using its **Restart** option; for
Container you can also restart the existing Home Assistant container. After
restart, open **Developer tools → States**, find `sensor.wireless_spd_snapshot`,
and inspect `gateway`, `summary` and `spds`. Confirm `sensor.spd_1_status`,
`binary_sensor.spd_1_fault` and `switch.wireless_spd_rest_alarm_muted` exist; an
existing name conflict may add a suffix, so use their actual entity IDs.
The switch's ON/OFF meaning is identical to MQTT and reflects the next poll.
If the snapshot is unavailable, fault/control availability is withheld rather
than assuming safe readings. The notification examples above work with these
fault entities after adjusting the selected IDs.
[REST](https://www.home-assistant.io/integrations/rest/)
[REST commands](https://www.home-assistant.io/integrations/rest_command/)
[Template entities](https://www.home-assistant.io/integrations/template/)
[Configuration files](https://www.home-assistant.io/docs/configuration/)
[Secrets](https://www.home-assistant.io/docs/configuration/secrets/)
[Recorder](https://www.home-assistant.io/integrations/recorder/)

## 10. Topic reference and retiring a device

Replace the example gateway ID below with `gateway.gateway_id` from your API.
With `MQTT_BASE_TOPIC = "wirelessspd"`, the root is
`wirelessspd/wspd-aabbccddeeff`:

| Topic | Payload/use |
| --- | --- |
| `wirelessspd/wspd-aabbccddeeff/availability` | Retained `online` / `offline` gateway availability. |
| `wirelessspd/wspd-aabbccddeeff/state` | Retained gateway counts, uptime and `alarm_muted` as `ON`/`OFF`. |
| `wirelessspd/wspd-aabbccddeeff/spd/1/state` | Retained SPD 1 JSON status/freshness/telemetry. |
| `wirelessspd/wspd-aabbccddeeff/command/alarm_muted` | Command exactly `ON` or `OFF`; **retain OFF** for commands. |
| `homeassistant/device/wspd-aabbccddeeff/config` | Retained receiver discovery. |
| `homeassistant/device/wspd-aabbccddeeff_spd_1/config` | Retained SPD 1 discovery. |

The receiver clears an old retained command before subscribing, resends discovery
on reconnect/HA birth and publishes current state before online availability.
Entity unique IDs combine gateway identity, SPD ID and metric. Friendly-name
changes preserve identity; replacing the receiver hardware changes identity.
No firmware-update controls or beacon secrets are published through MQTT.

To remove a retired SPD, first remove its registration from private config and
flash that image so it will not republish discovery. In Home Assistant's MQTT
integration **Configure → Publish a packet**, enter the **exact retired discovery
topic**, leave payload completely empty (not `""`, not `{}`), enable **Retain**,
and publish. This deletes the retained discovery and its entities. If an unused
device remains with no entities, remove it in Devices. You may similarly clear
its exact old state topic with an empty retained payload. For a base-topic
change, record and clear only the old receiver's exact discovery/state topics.
Do not publish deletion messages to `#`/`+` wildcards or erase other installations.

## References and maintenance

The linked references above are official Home Assistant/Mosquitto sources and
are not required to read this saved guide. Menu labels may differ slightly in
older Home Assistant versions. Preserve this HTML, the REST YAML, your private
receiver config and a Home Assistant backup before an offline installation.
The repository's canonical guide/examples generate `ha_guide.h` for the receiver.
The described MQTT/REST behavior is covered by host tests; your own Home Assistant
instance, network, contact and notification paths still require acceptance checks.
