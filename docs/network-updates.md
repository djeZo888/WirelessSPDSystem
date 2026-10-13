# Network firmware updates

The T Connect Pro receiver supports browser updates from **v0.2**. First install
v0.2 over USB using the [flashing guide](flashing.md). Keep **16 MB flash** and
**16M Flash (3MB APP / 9.9MB FATFS)**: this layout already contains two 3 MB
application slots. An update writes the inactive slot.

## Enable and build

Copy the new settings from `config.example.h` into private `config.h`:

```cpp
constexpr auto OTA_ENABLED = true;
static const char *OTA_USERNAME = "admin";
static const char *OTA_PASSWORD = "YOUR_PRIVATE_UPDATE_PASSWORD"; // 12–64 characters
```

Choose a private printable ASCII password independent of the AP password. The
username is 1–32 nonspace ASCII characters without a colon. Updates default to disabled; monitoring and API reads
need no update login. Use the receiver on a trusted LAN/AP. HTTP Basic credentials
and binaries are not TLS encrypted; do not expose the update endpoint to the Internet.

Build for the same board/partition settings with your complete private configuration.
Use the application's **`tconnectpro_868.ino.bin`** file: not the bootloader,
partition table, merged/full-flash image or an image for another board. The image
contains your Wi-Fi, beacon and update credentials, so keep it private.

## Upload

Open the receiver dashboard → **Firmware**, select the application image and
enter the update username/password. The browser computes SHA-256, shows progress
and waits for the receiver to reconnect. The new image's compiled configuration
replaces the previous one; preserve your beacon IDs/keys and network credentials.
Keep `OTA_ENABLED` and the update login configured in the new image if you want
subsequent network updates. Changing station Wi-Fi or the AP name may require
reconnecting to the new network.

The authenticated `POST /api/v1/firmware` accepts multipart fields **`size`** and
**`sha256`**, both before **`firmware`**, plus **`X-WSPD-Update: 1`**. For scripts:

```sh
IMAGE=/private/path/tconnectpro_868.ino.bin
SIZE=$(wc -c < "$IMAGE" | tr -d ' ')
SHA256=$(shasum -a 256 "$IMAGE" | cut -d ' ' -f 1)
curl --user admin --header 'X-WSPD-Update: 1' \
  --form "size=$SIZE" --form "sha256=$SHA256" \
  --form "firmware=@$IMAGE;type=application/octet-stream" \
  http://RECEIVER_IP/api/v1/firmware
```

`curl` prompts for the password. Success is **202** with `status: "ok"`; the
receiver restarts after sending its response. Invalid credentials, wrong image,
checksum/length errors or flash failures return an error and do not activate the
new image. SHA-256 detects transfer/file mismatch; it is not a firmware signature.

## During updates and recovery

Monitoring pauses and the relay stays OFF during uploading/restart. Keep stable
power connected and perform updates when this brief interruption is acceptable.
Rejected/cancelled updates restore reception and alarm handling. Restart clears
the receiver's RAM readings/loss history and mute state; persistent beacon counters
continue.

Interrupted/rejected uploads preserve the current selected application. This
does **not** guarantee recovery from firmware that uploads correctly but fails
during boot: Arduino confirms images before sketch health checks. Keep USB access
available and use the normal [USB upload procedure](flashing.md) for recovery.

Implementation references: [ESP32 partition layout](https://github.com/espressif/arduino-esp32/blob/3.3.12/tools/partitions/app3M_fat9M_16MB.csv),
[Arduino Update implementation](https://github.com/espressif/arduino-esp32/blob/3.3.12/libraries/Update/src/Updater.cpp).
