# Home Assistant

Use **MQTT Discovery** for automatic SPD devices and entities. The receiver's
HTTP API remains available without a broker: one `GET /api/v1/get` returns the
gateway, summary and every configured SPD, regardless of the LCD page.

## MQTT Discovery

1. Run a private MQTT broker, such as Home Assistant's Mosquitto app, and add the
   **MQTT** integration in **Settings → Devices & services**. Keep discovery
   enabled with the default `homeassistant` prefix and the default birth topic
   `homeassistant/status`. [Home Assistant MQTT documentation](https://www.home-assistant.io/integrations/mqtt/)
2. Connect the receiver to the same LAN using `WIFI_SSID` and `WIFI_PASSWORD`.
   The fallback AP alone does not provide a route to your LAN broker.
3. Set these options in the receiver's private `config.h`, rebuild and flash:

```cpp
static const char *MQTT_HOST = "192.168.1.20"; // Broker IP or hostname; empty disables MQTT.
constexpr auto MQTT_PORT = 1883;
static const char *MQTT_USERNAME = "wirelessspd";
static const char *MQTT_PASSWORD = "YOUR_BROKER_PASSWORD";
static const char *MQTT_BASE_TOPIC = "wirelessspd";
constexpr auto MQTT_PUBLISH_SECONDS = 10;
```

Use the broker's device login, not your Home Assistant web login. MQTT uses
unencrypted TCP on the trusted LAN; this firmware does not provide MQTT TLS.
An unreachable broker does not stop LoRa reception or the local alarm: MQTT
network work runs in a separate task and reconnect attempts back off for 30 s.

Home Assistant discovers one receiver device and one device per configured SPD:

| Device | Entities |
| --- | --- |
| Receiver | Configured, faulty, stale and unknown counts; uptime; alarm muted. |
| Wireless SPD | Status, fault, temperature, battery voltage, RSSI, SNR, report age and packet loss. |
| Optional local contact | Status and fault; no invented radio or battery readings. |

Status is **OK**, **FAIL**, **UNKNOWN** or **STALE**. The fault binary sensor is
available only for a fresh reading; UNKNOWN/STALE become **unavailable**, not
“no fault.” Freshness is also required for temperature, voltage, RSSI and SNR;
age/loss diagnostics remain available once a beacon has been seen. Missing
battery readings are unavailable. Receiver disconnection withdraws availability
through its MQTT last will. Entities also expire if publication stops.

IDs combine the receiver's hardware identity and SPD ID, so changing a friendly
name preserves Home Assistant history. Discovery and current state are retained;
the receiver refreshes them on connection and Home Assistant's birth message.
After reconnecting, it sends current states before marking the gateway online.
Publication is incremental, including with 128 configured rows.

Topics use `<MQTT_BASE_TOPIC>/<gateway_id>`:

| Topic | Retained payload |
| --- | --- |
| `.../availability` | `online` / `offline` |
| `.../state` | Gateway counts and diagnostics. |
| `.../spd/<id>/state` | One SPD's status and telemetry as JSON. |
| `homeassistant/device/<gateway_id>/config` | Receiver discovery. |
| `homeassistant/device/<gateway_id>_spd_<id>/config` | SPD discovery. |

If an SPD is removed from `SPD_CONFIGS`, delete its old retained discovery topic
with an empty retained payload, then remove the unused entity/device in Home
Assistant. Changing the base topic also requires clearing the old retained
topics. For troubleshooting, listen to `wirelessspd/#` in Home Assistant's MQTT
integration and check the receiver's MQTT connection indicator.

## HTTP polling alternative

The [REST example](../integrations/home-assistant/rest-example.yaml) makes **one
request every ten seconds for the full list**, saves it as sensor attributes,
and derives an example status/fault entity for SPD ID 1. Duplicate the template
entries for other IDs; match by `spd_id`, not array position. This needs no MQTT
broker, but each SPD entity is configured manually. Home Assistant's
[REST integration](https://www.home-assistant.io/integrations/rest/) shares one
response between sensors, and its
[Template integration](https://www.home-assistant.io/integrations/template/)
can derive entities with explicit availability.

Put the receiver on your LAN, reserve its IP address, and set this in Home
Assistant's `secrets.yaml`:

```yaml
wireless_spd_api_url: "http://192.168.1.30/api/v1/get"
```

Merge the example into `configuration.yaml`, check the configuration and restart
Home Assistant. Exclude the aggregate snapshot sensor from Recorder if you do
not need its full telemetry attributes stored every poll. Use the individual
fault entities for notifications, and handle **unavailable** separately from
**off** so a disconnected receiver cannot imply that all SPDs are healthy.
