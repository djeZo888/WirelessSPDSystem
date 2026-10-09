# Radio and beacon configuration

Choose the shared settings once per home, then copy them to its receiver and
beacons. Each beacon's `config.example.h` has a **per-server** section followed
by a **per-SPD** section. Copy it to private `config.h`; never commit deployment
keys or firmware images.

## Shared settings for one home

| Beacon setting | Receiver setting | Accepted value |
| --- | --- | --- |
| `WSPD_SHARED_SECRET` | `SPD_CONFIGS[i].secret` | Nonzero unsigned 32-bit integer: `0x00000001UL` through `0xFFFFFFFFUL`. Write eight hexadecimal digits, including leading zeros. Authentication always uses exactly four key bytes, little-endian. |
| `WSPD_LORA_FREQ_MHZ` | `LORA_FREQ_MHZ` | Finite frequency in MHz, **863.0625–869.9375**, inclusive. Default **865.3**. |
| `WSPD_LORA_SF` | `LORA_SF` | Integer **5–12**, inclusive. Default **10**. |

Use the same four-byte secret for the home's group, repeating it in each
registered receiver entry. Use a different randomly generated secret for a
different home. The receiver still accepts existing installations with different
keys per beacon; the packet format has not changed.

The receiver's SX1262 listens on one frequency and one spreading factor at a
time. Both must match every beacon in its group. It does not scan the old
frequency table or receive all spreading factors simultaneously. Changing either
shared radio setting requires updating the receiver and its beacons. Transmit
power may differ between beacons and need not match the receiver's initialization
power setting.

## Settings for one SPD

| Beacon setting | Accepted value |
| --- | --- |
| `WSPD_BEACON_ID` | Integer **1–127**, unique within the receiver. **1–9** are reserved for preproduction; use **10–127** for production. ID **0** belongs only to the receiver's optional local contact. |
| `WSPD_LORA_TX_DBM` | Integer **−9 to +22 dBm**, default **10**. This is the SX1262 software limit; choose power permitted for the frequency, antenna and installation. |
| `WSPD_NONCE_START` | Integer **1–18,446,744,073,709,551,614** (`UINT64_MAX - 1`), with `ULL` suffix. Default **1**. Used only for completely erased EEPROM; an existing journal overrides it. See [nonce migration](nonce-storage.md). |

The receiver registers each SPD with `{ ID, "friendly name", secret }`.
Its `config.example.h` also documents ranges and units for network, display,
touch, stale detection, loss history, alarm and local-input settings.

## Fixed modem settings and upgrades

These are firmware constants in all three sketches, absent from config files:

| Parameter | Fixed value |
| --- | --- |
| Bandwidth | **125 kHz** |
| Coding rate | **4/5** (`5` in RadioLib) |
| Sync word | **0x12** |
| Preamble | **8 symbols** |
| LoRa payload CRC | **Enabled**, two bytes |

The old `WSPD_LORA_CHANNEL_ID` and its eight-entry frequency table are removed.
To retain an installation that used channel 1, replace that macro with
`#define WSPD_LORA_FREQ_MHZ 865.3f`. Former IDs 0–7 corresponded respectively to
865.1, 865.3, 865.5, 865.7, 865.9, 866.1, 866.3 and 866.5 MHz. Then rebuild.
For an existing receiver `config.h`, remove the definitions of `LORA_BW_KHZ`,
`LORA_CR`, `LORA_SYNCWORD`, `LORA_PREAMBLE` and `LORA_CRC_ENABLED`; the sketch
owns them. Copy the updated configurable declarations from the example while
preserving credentials and SPD registrations.

## Frequency limits

The project targets the European **863–870 MHz** SRD band. Its carrier bounds
leave half the nominal 125 kHz bandwidth inside the band's outer edges; there
is no required channel grid. The radio synthesizer selects the nearest supported
frequency, so arbitrary decimal precision does not imply an exact RF carrier.

These bounds are a project configuration check. SRD sub-bands have different
radiated-power and airtime limits. Include antenna gain, routine reports, button
requests and repeated fault frames when checking permitted operation. Firmware
does not enforce regulatory airtime budgets. Keep the occupied bandwidth within
the applicable sub-band too, and verify the fitted module, antenna and RF
performance when changing frequency. Existing **865.3 MHz / SF10 / 10 dBm**
defaults are retained.

References: [CEPT ERC Recommendation 70-03 and national implementation](https://docdb.cept.org/document/845),
[Semtech SX1262](https://www.semtech.com/products/wireless-rf/lora-connect/sx1262),
and [RadioLib SX1262 power configuration](https://github.com/jgromes/RadioLib/blob/7.8.1/src/modules/SX126x/SX1262.cpp).
