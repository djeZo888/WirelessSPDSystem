# Configure and flash

## Tools and dependencies

Install [Arduino CLI](https://arduino.github.io/arduino-cli/installation/) and
Python 3.9+, plus Git. Clone this repository and run commands from its root.
The setup tool installs pinned dependencies into ignored `.arduino/`; it leaves
your usual Arduino installation separate. On Apple Silicon, the DxCore AVR tools
may need Rosetta 2.

| Target | Core | Libraries |
| --- | --- | --- |
| `acdc`, `battery` | DxCore 1.6.2 | RadioLib 7.8.1; pinned usha256 |
| `server` | ESP32 2.0.14 | RadioLib 7.8.1; GFX Library for Arduino 1.4.6 |

Use `python3 tools/firmware.py setup acdc` (or `battery` / `server`).
The two beacons share dependencies, so one beacon setup is sufficient.
usha256 is fetched locally at an exact commit; see its
[licensing provenance](../firmware/beacons/third-party/usha256.md).

## Configure a beacon

Copy `config.example.h` to `config.h` inside the selected sketch folder:

```sh
cp firmware/beacons/acdc_avr128da32/config.example.h firmware/beacons/acdc_avr128da32/config.h
# For battery hardware use firmware/beacons/battery_avr128db32 instead.
```

Edit the local file: set a unique `WSPD_BEACON_ID` (10–127 in production),
`WSPD_SHARED_SECRET`, radio channel, transmit power and spreading factor.
Generate an independent nonzero random 32-bit key per beacon and register it
in the receiver. For example, `python3 -c 'import secrets; print(hex(secrets.randbelow(0xFFFFFFFF)+1))'`.
The example key is zero and compilation rejects it.

**Programming connection:** use Microchip PICkit 4 in AVR/UPDI mode. Keep the AC
input disconnected; program the board from an isolated regulated 3.3 V supply
that can power the radio. Leave PICkit target power disabled (its supply is
limited to 50 mA). Connect by signal, checking the actual PCB header orientation:

| PICkit 4 pin | Board signal |
| ---: | --- |
| 1 | RESET / PF6 |
| 2 | VCC / target voltage sense |
| 3 | GND |
| 4 | Dedicated UPDI |

Both supplied schematics label the PCB header **1 RESET, 2 VCC, 3 GND, 4 UPDI**;
consult [hardware pin maps](../hardware/README.md) and verify the physical orientation.
PICkit pins 5–8 are unused. No serial
port is required. Fit the intended LoRa antenna before running the firmware.

```sh
python3 tools/firmware.py build acdc
python3 tools/firmware.py flash acdc
# Substitute battery for the battery-powered board.
```

The script selects no-bootloader AVR128DA32/DB32, 24 MHz internal clock, TCB2
millis and RESET enabled; DB builds enable MVIO. Exact options are in each
`build-options.json`. Upload writes flash with AVRDUDE verification and the
DxCore upload recipe's fuse settings. Inspect existing BOD/watchdog/fuse state
on the first target; these compile settings do not qualify battery clock/voltage
behavior. Avoid casual **Burn Bootloader**, which can erase the chip.

Both beacon builds are firmware 1 with persistent nonces. The CLI sets and
verifies EESAVE before flash erase and continuous active BOD at 1.9 V before
upload (battery BOD is sampled at 32 Hz in sleep). Manual IDE/programmer uploads
must establish these fuses before erasing. Keep EEPROM intact on future updates.
For an ID previously received with RAM-only firmware, set `WSPD_NONCE_START`
above its last received nonce when installing on blank EEPROM. See the
[journal, migration and power-loss checks](nonce-storage.md).

## Configure and flash the server

Use the **LILYGO T Connect Pro V1.0 with 868 MHz SX1262**, not the 433 MHz model.

```sh
python3 tools/firmware.py setup server
cp firmware/servers/tconnectpro_868/config.example.h firmware/servers/tconnectpro_868/config.h
```

Edit `config.h`: set `CONFIGURED=true`, the AP password, optional Wi-Fi credentials,
and `SPD_CONFIGS` entries with matching beacon IDs/keys and readable names.
Up to 127 wireless beacons plus one local contact can be configured. The LCD
advances through eight-row pages every five seconds; eight or fewer stay on one
page. Match radio settings. Set a reset API key (the web RESET button asks for it);
an empty key explicitly disables reset protection. Leave `SPD_LOCAL_ID=-1`
unless using the optional isolated local dry contact.

Connect the ESP32 USB port with a data cable, find its port with
`arduino-cli board list`, then:

```sh
python3 tools/firmware.py build server
python3 tools/firmware.py flash server --port /dev/cu.usbmodemYOUR_PORT
```

Use the actual port (`COMn` on Windows). If upload cannot enter the bootloader,
hold BOOT, press/release RESET, then release BOOT and retry. Settings are ESP32S3
Dev Module, 16 MB flash, 3 MB APP / 9.9 MB FATFS partition, QIO 80 MHz, OPI PSRAM,
USB CDC enabled, 921600 upload; the script supplies the exact board options.

Open the IP shown on screen/Serial. Without a Wi-Fi connection, join the configured
fallback AP and open its displayed IP. Check the display and dashboard identify
each beacon correctly, then test OK/FAIL, temperature, battery voltage and alarms.
Power-cycle a persistent beacon and check that its next nonce is higher without
using receiver RESET. RESET remains available for explicit migration/recovery
and clears all wireless live readings/loss history.

See the [server guide](../firmware/servers/tconnectpro_868/README.md) for the full
configuration reference, LCD paging, alarms, web API, RAM storage limits and
Arduino IDE settings.

## Arduino IDE alternative

Install the same pinned cores/libraries in Arduino IDE and add
`https://drazzy.com/package_drazzy.com_index.json` to Boards Manager URLs for DxCore.
Put the fetched usha256 folder in your IDE sketchbook's `libraries` directory.
Open the named `.ino`, add the adjacent local `config.h`, select the matching
options above and **Upload Using Programmer** for beacons (PICkit 4), or ordinary
USB upload for the server. The CLI's isolated packages are not automatically
visible to an IDE using a different sketchbook/data directory.

Local configuration, build images and build manifests contain secrets and are
ignored by Git. Flash checks reject changed source/configuration or modified
images; synthetic validation builds are never selected for upload.

Hardware/tool references: [PICkit 4 guide](https://ww1.microchip.com/downloads/aemDocuments/documents/DEV/ProductDocuments/UserGuides/MPLAB-PICkit-4-In-Circuit-Debugger-Users-Guide-DS50002751.pdf),
[DxCore programmer definitions](https://github.com/SpenceKonde/DxCore/blob/1.6.2/megaavr/programmers.txt),
[LILYGO board setup](https://github.com/Xinyuan-LilyGO/T-Connect-Pro).
