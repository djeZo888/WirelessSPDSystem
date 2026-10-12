# Beacon power management

The AC-DC AVR128DA32 firmware sleeps between scheduled reports. Earlier hardware-1
firmware continuously polled the contact and ADC, and RadioLib's blocking
transmit returned the radio to standby. Sleeping requires rebuilding and flashing
this revision; the previously flashed preproduction unit does not acquire it
automatically. Hardware type and firmware identity remain **1 / 1** during the
current preproduction phase. The 20-byte receiver protocol and EEPROM journal
are unchanged.

## Idle and wake behavior

- AVR128DA32 enters **STANDBY**. Its high-frequency CPU clock stops. The internal
  1024 Hz RTC source divided by 32 supplies a wrapping **32 Hz** counter; only
  its next compare deadline wakes the CPU. There is no periodic millisecond or
  one-second CPU interrupt during the long wait.
- A one-shot RTC compare schedules randomized **300–600 second** production
  reports, **20–40 seconds** in preproduction, the existing 5-second first report,
  3-second FAIL repeats, LED turn-off and bounded radio recovery. Quantization
  is 31.25 ms; accuracy depends on the internal RTC oscillator.
- STANDBY is selected because RTC CNT runs in that mode. POWER_DOWN would
  require a different timer design using PIT. A deeper mode name alone does
  not establish lower total board consumption.
- **PD7 CHANGE** interrupts wake on contact opening or closing. Opening an OK
  contact latches an immediate FAIL event. The two additional FAIL transmissions
  use the exact cached packet and nonce, sleeping between repeats. A normal
  regular report reads the current contact status.
- The SX1262 receives `sleep(true)` after initialization and every transmission.
  Warm sleep retains its configuration and turns its TCXO off. The command
  completes before `SPI.end()`. At the next transmission, SPI is enabled and
  standby is requested before TX. Sleep/initialization/TX failures reset the
  radio and retry after one second while the MCU otherwise sleeps.
- ADC0 is disabled after each temperature measurement, including every error
  exit. Temperature is sampled for a new packet, not repeatedly during idle or
  cached FAIL retransmissions. SPI and the high-frequency TCB2 millis timer do
  not run in STANDBY. Confirmed unused input buffers are disabled; RESET, UPDI
  and all connected signal pins remain available.
- Runtime BOD configuration uses **32 Hz sampled BOD during sleep**, preserving
  continuous active BOD and the existing 1.9 V threshold for EEPROM writes.
  No programming-profile fuse change is required. The nonce is still committed
  and verified before each new on-air packet, and EEPROM retention remains enabled.

The RTC arm sequence checks pending contacts and elapsed deadlines after register
synchronization. Interrupt enabling immediately precedes SLEEP, preventing a
contact edge from being serviced between the final check and sleeping.

## LEDs and hardware current limits

`WSPD_LED_MODE` is an AC-DC per-SPD configuration integer **0–2**:

| Value | Behavior |
| --- | --- |
| **0** | Earlier steady green power / red FAIL indications; yellow throughout TX. These LEDs continue consuming power during MCU sleep. |
| **1**, default | Green OK or red FAIL pulses at startup, contact changes and new reports, lasting 1–2 RTC ticks (about 31–63 ms). Yellow starts with TX and is capped at 50 ms. All normal LEDs are off during the long idle wait. |
| **2** | Normal LEDs disabled, including radio initialization indication. |

An EEPROM integrity/commit fault still blinks all LEDs and halts transmission in
every mode. Fast LED pulses do not change whether a FAIL is reported.

Several loads in the existing schematic cannot be switched off by firmware:

| Load at nominal 3.3 V | Consequence |
| --- | --- |
| SPD input's external 10 kΩ pull-up, contact closed | Approximately **330 µA**. |
| 10 kΩ pull-up plus connected 10 kΩ NTC at 25°C | Approximately **165 µA**, varying with temperature. An unplugged NTC does not create this divider load. |
| 680 Ω LED resistors | Steady LEDs draw milliamps, depending on LED forward voltage; mode 1/2 avoids this idle load. |
| IRM-02 converter | Datasheet specifies no-load input power **less than 75 mW**. This is an upper specification, not a measured fixed floor or a 3.3 V output current. |

The Wio-SX1262 module datasheet lists **1.62 µA** sleep current. This module figure
is more useful than the bare SX1262 silicon's typical sleep current, but it still
does not include the MCU, input divider, pull-up, converter or other PCB loads.
Actual total consumption requires measurement. Reducing the fixed contact and
NTC loads further would require hardware changes.

## Validation and board acceptance

`python3 tests/check_acdc_sleep.py` compiles the actual scheduler and extracted
sketch helpers against native I/O shims with AddressSanitizer/UBSan. It covers
RTC wrap and overdue work, contact wake/arming races, cached FAIL repeats, all
ADC cleanup exits, radio sleep/SPI ordering, timeout recovery, LED modes, and
clock/BOD/input-buffer setup. CI also compiles the actual AVR sketch and retains
packet, persistent nonce, configuration and EEPROM-fuse checks.

These host checks do not measure electrical sleep current or prove asynchronous
hardware wake timing. After flashing with regulated 3.3 V power and EEPROM
retention, measure the long-idle 3.3 V load, verify regular wake/TX and receiver
acceptance, open/close the contact while asleep, check FAIL repeats, and verify
nonce progression after a power cycle. The earlier successful flash/LED tests
apply to the earlier preproduction build; this sleep revision has not yet been
flashed or physically qualified.

References: [AVR128DA datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU08/ProductDocuments/DataSheets/AVR128DA28-32-48-64-Data-Sheet-DS40002183.pdf),
[DxCore pin interrupts](https://github.com/SpenceKonde/DxCore/blob/1.6.2/megaavr/extras/Ref_PinInterrupts.md),
[Wio-SX1262 module datasheet](https://files.seeedstudio.com/products/SenseCAP/Wio_SX1262/Wio-SX1262_Module_Datasheet.pdf),
[RadioLib SX126x commands](https://github.com/jgromes/RadioLib/blob/7.8.1/src/modules/SX126x/SX126x_commands.cpp),
[IRM-02 datasheet](https://www.meanwell.com/Upload/PDF/IRM-02/IRM-02-SPEC.pdf).
