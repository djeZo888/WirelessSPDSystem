# Persistent beacon nonce

Both AVR128DA32 and AVR128DB32 contain 512 bytes of internal EEPROM. Hardware 1
and hardware 2 firmware version 1 now use that EEPROM for the nonce journal;
no secret, ID or measurement is stored there. The existing 20-byte packet format
and receiver nonce comparison remain unchanged. The receiver does not need new
firmware to accept these reports.

## Transmission and restart

The journal records the **next value to use**, not the last packet sent. Before
building a new report with nonce N, the beacon saves N+1, waits for the EEPROM
operation to complete and verifies the stored bytes. Only then can it transmit N.
After restarting, it resumes at the largest valid committed value. Repeated
copies of one FAIL packet keep the same nonce and do not cause another save.

A normal new report increments the nonce by exactly one. Idle boots do not write
EEPROM or advance the counter. If power fails after a commit but before the RF
transmission, the allocated nonce remains consumed: a gap can appear as one
missed report on the receiver. This is necessary to avoid ever reusing a value
that might have been received. There are no block reservations that create large
nonce gaps on every reboot. Radio failures also consume their attempted nonce.

## EEPROM layout and failures

All 512 bytes are reserved: 32 rotating records of 16 bytes. Each holds a commit
marker, format identifier, the little-endian 64-bit next nonce and a CRC-32.
Before reusing the oldest slot, its marker is invalidated and verified. All body
bytes are written and checked, then the commit marker is written and checked.
The newest committed slot is never the slot being overwritten.

Full byte writes refresh each cell on every rotation, including stable format
and high-counter bytes. This addresses the AVR Dx array-refresh requirement as
well as spreading wear. EEPROM has finite endurance; this is not a lifetime
qualification. Writes add roughly 170 ms to a new report using the datasheet's
byte erase/write timing. Repeated copies do not add EEPROM work.

A partially written slot is ignored when another valid committed record exists.
A corrupt committed record, unexpected marker, nonblank journal with no valid
record, failed readback or exhausted 64-bit range stops transmission. All three
LEDs blink together at 250 ms on / 250 ms off until serviced. The beacon never
silently resets its nonce on an uncertain storage state. An interruption of the
very first journal write can leave no valid record and deliberately requires
service; no report has been emitted yet. Recover using a known safe initial
value after inspecting the journal, rather than automatically erasing it.

## First installation, upgrades and reflashing

A completely erased EEPROM starts at `WSPD_NONCE_START`, default **1**. This
setting is ignored once a valid journal exists. Existing private `config.h`
files without the new macro also default to 1. A first persistent build cannot
recover the old RAM-only counter from a previously deployed beacon.

For an existing receiver entry, read its latest nonce and set `WSPD_NONCE_START`
strictly higher before first installation on blank EEPROM. For example, after
nonce 9000, use `9001ULL`. Alternatively, explicitly reset that receiver's nonce
history once during migration; its RESET also clears other beacons' live state.
Subsequent ordinary power cycles need neither action.

Preserve EEPROM on every firmware update. The provided CLI flashing workflow
reads the target's EEPROM-retention fuse, enables and verifies EESAVE **before**
the upload can perform its automatic chip erase. It also programs and verifies
the BOD fuse separately because ordinary DxCore uploads do not set BODCFG.
Neither preparation step issues a chip erase. MCU signature mismatches abort.
The profile uses continuous brownout detection while active at 1.9 V; AC-DC uses
continuous detection in sleep, while battery uses sampled 32 Hz detection in
sleep to limit standby current. Confirm the supply and actual fuse readback on
the board. These fuse changes are part of preparing persistent storage.

For manual IDE/PICkit uploads, set and verify EESAVE and active BOD before any
erase. Selecting a compile menu is insufficient to change BOD. Avoid casual
Burn Bootloader, EEPROM erase, or unlocking a locked chip: retention cannot be
relied on for those operations. Back up an existing journal before servicing a
used beacon. If it is erased or the MCU is replaced, provide a new safe start
above the receiver's remembered nonce; do not deploy the default 1 blindly.

## Validation

Host tests exercise the exact journal header shared by both sketches, with cuts
after every write at virgin provisioning and ring boundaries, dropped/corrupt
writes, corrupt committed records, ordinary reboots and exhaustion. Packet tests
exercise both actual encoders and the receiver tag verifier. See
[validation](validation.md). No physical board was flashed for this change.

Before accepting a production unit, receive a report, power-cycle it and verify
that its next accepted nonce is greater without resetting the receiver. Repeat
with power removed near a report/EEPROM update; inspect fuse readback and the
all-LED storage-fault indication. Journaling protects interrupted writes; active
BOD and valid supply conditions protect CPU execution. Physical brownout, RF,
battery standby current and long-term endurance remain board acceptance checks.

References: [AVR DA datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU08/ProductDocuments/DataSheets/AVR128DA28-32-48-64-Data-Sheet-DS40002183.pdf),
[AVR DB datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU08/ProductDocuments/DataSheets/AVR128DB28-32-48-64-DataSheet-DS40002247.pdf),
and [DxCore EEPROM library](https://github.com/SpenceKonde/DxCore/tree/1.6.2/megaavr/libraries/EEPROM).
