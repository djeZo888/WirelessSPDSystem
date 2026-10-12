// Copy config.example.h to config.h and set the private network credentials.
#if __has_include("config.h")
#include "config.h"
#else
#error "Missing config.h: copy config.example.h and configure this beacon."
#endif

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <usha256.h>
#include <math.h>
#include <string.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include "nonce_eeprom.h"
#include "sleep_schedule.h"

// -----------------------------------------------------------------------------
// DEHN SPD FM wireless beacon - previous AVR128DA32 hardware + Wio-SX1262
// Payload v2 compatibility build: adds hardware/firmware byte and voltage byte.
// -----------------------------------------------------------------------------

// ---------------------- NETWORK / DEVICE CONFIG ----------------------
#ifdef WSPD_LORA_CHANNEL_ID
#error "Replace WSPD_LORA_CHANNEL_ID with WSPD_LORA_FREQ_MHZ in config.h."
#endif
#ifndef WSPD_LORA_FREQ_MHZ
#error "Set WSPD_LORA_FREQ_MHZ in config.h; beacon and receiver must match."
#endif
#ifndef WSPD_PREPRODUCTION
#define WSPD_PREPRODUCTION 0
#endif
#ifndef WSPD_LED_MODE
#define WSPD_LED_MODE 1
#endif

// Validate original configuration values before narrowing to packet/radio types.
static_assert(__builtin_classify_type(WSPD_PREPRODUCTION) == 1,
              "WSPD_PREPRODUCTION must be integer 0 (production) or 1 (testing).");
static_assert(WSPD_PREPRODUCTION == 0 || WSPD_PREPRODUCTION == 1,
              "WSPD_PREPRODUCTION must be 0 (production) or 1 (testing).");
static_assert(__builtin_classify_type(WSPD_LED_MODE) == 1 &&
                  WSPD_LED_MODE >= 0 && WSPD_LED_MODE <= 2,
              "WSPD_LED_MODE must be 0 (steady), 1 (heartbeat), or 2 (off).");
static_assert(__builtin_classify_type(WSPD_SHARED_SECRET) == 1,
              "WSPD_SHARED_SECRET must be a 4-byte unsigned integer value.");
static_assert(WSPD_SHARED_SECRET > 0 && WSPD_SHARED_SECRET <= 0xFFFFFFFFULL,
              "WSPD_SHARED_SECRET must be 0x00000001..0xFFFFFFFF (4 bytes).");
static_assert(__builtin_classify_type(WSPD_BEACON_ID) == 1,
              "WSPD_BEACON_ID must be an integer.");
static_assert(WSPD_BEACON_ID >= 1 && WSPD_BEACON_ID <= 127,
              "WSPD_BEACON_ID must be 1..127; use 10..127 for production.");
static_assert(__builtin_classify_type(WSPD_LORA_TX_DBM) == 1,
              "WSPD_LORA_TX_DBM must be an integer.");
static_assert(WSPD_LORA_TX_DBM >= -9 && WSPD_LORA_TX_DBM <= 22,
              "WSPD_LORA_TX_DBM must be -9..22; obey local RF limits.");
static_assert(__builtin_classify_type(WSPD_LORA_SF) == 1,
              "WSPD_LORA_SF must be an integer.");
static_assert(WSPD_LORA_SF >= 5 && WSPD_LORA_SF <= 12,
              "WSPD_LORA_SF must be 5..12 and match the receiver.");
static_assert(WSPD_LORA_FREQ_MHZ >= 863.0625f && WSPD_LORA_FREQ_MHZ <= 869.9375f,
              "WSPD_LORA_FREQ_MHZ must be finite and within 863.0625..869.9375 MHz.");
const uint32_t SECRET = WSPD_SHARED_SECRET;
static_assert(sizeof(SECRET) == 4, "The packet authentication key must occupy 4 bytes.");
const uint8_t SPD_ID = WSPD_BEACON_ID;

// Payload identity byte:
//   bits 7..4 = hardware type: 1 = previous AVR128DA32 hardware, 2 = AVR128DB32 hardware
//   bits 3..0 = firmware version
// Type 0 is reserved. Production firmware version starts at 1.
constexpr uint8_t BEACON_HW_TYPE          = 1;
constexpr uint8_t BEACON_FIRMWARE_VERSION = 1;
static_assert(BEACON_HW_TYPE > 0 && BEACON_HW_TYPE <= 0x0F, "BEACON_HW_TYPE must be 1..15");
static_assert(BEACON_FIRMWARE_VERSION <= 0x0F, "BEACON_FIRMWARE_VERSION must be 0..15");

// Old AVR128DA32 hardware cannot measure cell voltage. For the new payload,
// 0 means 2.20 V, 1 means 2.21 V, ..., and 255 means unavailable/error.
constexpr uint8_t CELL_VOLTAGE_UNAVAILABLE = 255;

// On-air frame layout:
//   data[0]      = hardware/firmware byte: (type << 4) | firmware
//   data[1]      = old id/status byte: (SPD_ID << 1) | SPD status
//   data[2]      = signed int8 temperature in deg C, preserving old semantics
//   data[3..10]  = uint64 nonce, little-endian
//   data[11]     = cell voltage byte; 255 on this old hardware
//   data[12..19] = first 8 bytes of SHA-256(data[0..11] || SECRET_LE32)
constexpr size_t BEACON_DATA_LEN   = 12;
constexpr size_t BEACON_HASH_LEN   = 8;
constexpr size_t BEACON_PACKET_LEN = BEACON_DATA_LEN + BEACON_HASH_LEN;

// First regular transmission happens after this startup delay.
const float START_DELAY_S = 5.0f;

// Regular beacon interval is chosen randomly inside this range.
#if WSPD_PREPRODUCTION == 1
// Short interval for preproduction/testing only.
constexpr float TX_INTERVAL_SEC_MIN = 20.0f;
constexpr float TX_INTERVAL_SEC_MAX = 40.0f;
#else
// Production interval; also selected by older config files without the flag.
constexpr float TX_INTERVAL_SEC_MIN = 300.0f;
constexpr float TX_INTERVAL_SEC_MAX = 600.0f;
#endif

// On transition OK -> FAIL, send one immediate FAIL beacon and then repeat the
// exact same payload this many additional times.
const uint8_t SPDFAIL_RETX_COUNT = 2;      // 0 = disabled
const float   SPDFAIL_RETX_DELAY = 3.0f;   // seconds between repeats

static_assert(TX_INTERVAL_SEC_MIN > 0.0f && TX_INTERVAL_SEC_MIN <= TX_INTERVAL_SEC_MAX &&
                  TX_INTERVAL_SEC_MAX * AcDcSleep::TICKS_PER_SECOND <=
                      AcDcSleep::MAX_DELAY_TICKS,
              "Report intervals must fit the RTC signed half range (below 1024 seconds).");

// LoRa RF settings: one carrier frequency per receiver/home.
const float   LORA_FREQ_MHZ = WSPD_LORA_FREQ_MHZ;
const int8_t  LORA_TX_DBM   = WSPD_LORA_TX_DBM;
const uint8_t LORA_SF       = WSPD_LORA_SF;
// Fixed interoperable modulation settings; not deployment configuration.
constexpr float   LORA_BW_KHZ      = 125.0f;
constexpr uint8_t  LORA_CR          = 5;    // coding rate 4/5 in RadioLib naming
constexpr uint8_t  LORA_SYNCWORD    = 0x12; // private network sync word
constexpr uint16_t LORA_PREAMBLE    = 8;
constexpr bool    LORA_CRC_ENABLED = true;

// Wio-SX1262 uses an active TCXO powered from DIO3.
// The module datasheet allows 1.7 .. 3.3 V and says DIO3 should stay about
// 200 mV below VCC. 2.4 V is a conservative value at 3.3 V supply.
const float SX1262_TCXO_VOLTAGE = 2.4f;

// Seeed documents this module as DC-DC powered, not LDO-only.
const bool  SX1262_USE_LDO = false;

// RadioLib defaults to a fairly low current limit on SX126x.
const float SX1262_CURRENT_LIMIT_MA = 140.0f;

// ---------------------- PIN MAPPING ----------------------
// AVR128DA32 SPI0 default pins on this PCB:
//   MOSI = PA4, MISO = PA5, SCK = PA6, NSS = PA7
// SX1262 control pins:
//   RF_SW = PC0, NRST = PC1, BUSY = PC2, DIO1 = PC3
const uint8_t LORA_NSS   = PIN_PA7;
const uint8_t LORA_RST   = PIN_PC1;
const uint8_t LORA_BUSY  = PIN_PC2;
const uint8_t LORA_DIO1  = PIN_PC3;
const uint8_t LORA_RFSW  = PIN_PC0;
const uint8_t LORA_MOSI  = PIN_PA4;
const uint8_t LORA_MISO  = PIN_PA5;
const uint8_t LORA_SCK   = PIN_PA6;

// Inputs
const uint8_t PIN_SPD_STATUS = PIN_PD7;
const uint8_t PIN_THERMO     = PIN_PD0;

// LEDs (active low)
const uint8_t LED_PWR = PIN_PA1;
const uint8_t LED_TX  = PIN_PF5;
const uint8_t LED_SPD = PIN_PF4;

#ifndef RADIOLIB_NC
  #define RADIOLIB_NC ((uint32_t)-1)
#endif

static inline void ledOn(uint8_t pin)  { digitalWrite(pin, LOW);  }
static inline void ledOff(uint8_t pin) { digitalWrite(pin, HIGH); }

// ---------------------- THERMISTOR ----------------------
// Littelfuse AC103E2F style 10k NTC, Beta ~= 3435 K
const float R_PULLUP = 10000.0f;
const float R0       = 10000.0f;
const float T0       = 298.15f;   // 25 C in Kelvin
const float BETA     = 3435.0f;

// ADC / thermistor fault handling
// The PCB has: VCC -- R4 10k -- CN6 pin 4 -- AC103E2F -- CN6 pin 3/GND.
// TERMO/PD0 is read after R5 1k, with C9 100 nF to GND.  R5 has essentially
// no DC effect because the ADC input is high impedance.
//
// This build intentionally bypasses DxCore analogRead() for temperature.  The
// recent DxCore 1.6.x VREF bug can otherwise make normal thermistor voltages
// saturate, and direct register reads make the ADC scale unambiguous.
const uint8_t  TEMP_ADC_BITS              = 10;
const uint16_t TEMP_ADC_MAX               = (1U << TEMP_ADC_BITS) - 1U;
const uint8_t  TEMP_ADC_SAMPLES           = 8;
const uint8_t  TEMP_ADC_SAMPLE_DURATION   = 31;   // longer acquisition for robustness
const uint16_t TEMP_RAW_SHORT_THRESHOLD   = 5;    // near GND => short/fault
const uint16_t TEMP_RAW_OPEN_THRESHOLD    = TEMP_ADC_MAX - 5U; // near VDD => open/fault

// Fault behavior on the air:
//   TEMP_FAULT_TX_SENTINEL  -> send impossible temperatures for easy log diagnosis
//   TEMP_FAULT_TX_LAST_VALID -> keep last good temperature instead
enum TempFaultTxMode : uint8_t {
  TEMP_FAULT_TX_SENTINEL = 0,
  TEMP_FAULT_TX_LAST_VALID = 1,
};
const TempFaultTxMode TEMP_FAULT_TX_MODE = TEMP_FAULT_TX_SENTINEL;

// Sentinel temperatures (all are intentionally impossible here)
const int8_t TEMP_SENTINEL_ADC_ERROR = -128;
const int8_t TEMP_SENTINEL_OPEN      = -127;
const int8_t TEMP_SENTINEL_SHORT     = -126;

enum : uint8_t {
  TEMP_READ_OK = 0,
  TEMP_READ_ADC_ERROR,
  TEMP_READ_OPEN_CIRCUIT,
  TEMP_READ_SHORT_CIRCUIT,
};

// ---------------------- SIMPLE PRNG ----------------------
static uint32_t prng_state = 0x12345678UL;

// ---------------------- RADIO ----------------------
// Module(cs, irq, rst, gpio/busy)
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// ---------------------- FORWARD DECLARATIONS ----------------------
static bool radioOk(int16_t state);
static bool loraInit();
static bool transmitPacket(const uint8_t* pkt, size_t len);
static void sendBeaconEvent(uint8_t spd, bool burstFailRepeats);
static void radioSleepRetain();
static void radioPinsIdle();
static uint16_t nowTicks();
static void signalStatus(uint8_t spd);
static void finishGreenPulse(uint16_t now);
static void startGreenHeartbeat(uint16_t now);
static void serviceGreenHeartbeat(uint16_t now);
static void sleepUntilWork();
static uint8_t readSpdStatusBit();
static uint8_t readCellVoltageByte();
static void configureThermoAdc();
static bool readThermoRaw10(uint16_t& rawOut, int16_t& adcErrOut);
static bool readAdc0Ain0Single10(uint16_t& rawOut);
static uint16_t normalizeAdcReadingTo10bit(uint16_t value);
static int8_t convertRaw10ToTempC(uint16_t raw);
static uint8_t readTempC_int8(int8_t& tempOut, uint16_t& rawOut, int16_t& adcErrOut);
static int8_t chooseTempToTransmit(uint8_t status, int8_t latestTemp);
static void haltNonceStorageFault() __attribute__((noreturn));

static AvrNonceStorage nonceStorage;
static PersistentNonceJournal<AvrNonceStorage> nonceJournal(nonceStorage);
static AcDcSleep::Schedule txSchedule;
static uint8_t failRepeatPacket[BEACON_PACKET_LEN];
static volatile bool spdWakeRequested = false;
static volatile bool spdFailRequested = false;
static bool pendingFailEvent = false;
static bool radioInitialized = false;
static uint16_t nextRadioRetryTick = 0;
static volatile bool greenPulseActive = false;
static volatile uint16_t greenPulseEndTick = 0;
static volatile uint16_t nextGreenHeartbeatTick = 0;
constexpr uint16_t GREEN_HEARTBEAT_TICKS = 30U * AcDcSleep::TICKS_PER_SECOND;
constexpr uint16_t GREEN_ON_TICKS = AcDcSleep::TICKS_PER_SECOND / 2U;
static uint8_t lastSpdStatus = 1;
static bool    haveLastGoodTemp = false;
static int8_t  lastGoodTempC = 25;

static uint32_t xorshift32() {
  uint32_t x = prng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  prng_state = x;
  return x;
}

static uint16_t secondsToTicksRounded(float seconds) {
  if (seconds <= 0.0f) return 0;
  const float ticks = seconds * AcDcSleep::TICKS_PER_SECOND;
  if (ticks >= AcDcSleep::MAX_DELAY_TICKS) return AcDcSleep::MAX_DELAY_TICKS;
  return (uint16_t)lroundf(ticks);
}

static float randomFloat01() {
  // 24 useful random bits are enough for interval dithering.
  return (float)(xorshift32() & 0x00FFFFFFUL) / 16777215.0f;
}

static uint16_t pickNextIntervalTicks() {
  float lo = TX_INTERVAL_SEC_MIN;
  float hi = TX_INTERVAL_SEC_MAX;
  if (hi < lo) {
    float tmp = hi;
    hi = lo;
    lo = tmp;
  }
  float seconds = (hi > lo) ? (lo + (hi - lo) * randomFloat01()) : lo;
  return secondsToTicksRounded(seconds);
}

// ---------------------- LOW-POWER TIMEBASE ----------------------
// Unlike millis()/TCB2, this counter runs with the CPU/high-frequency clock off.
// Only the next work deadline generates an interrupt; there is no periodic CPU
// tick while waiting. RTC CNT needs STANDBY (POWER_DOWN supports only the PIT).
ISR(RTC_CNT_vect) {
  RTC.INTFLAGS = RTC_CMP_bm | RTC_OVF_bm;
  RTC.INTCTRL = 0;  // one-shot; the main loop arms the next deadline
  // The green heartbeat is independent of reports/radio initialization. Its
  // start/end must run even while foreground work is writing EEPROM or in TX.
  // A normal-work deadline is evaluated separately in the main loop.
  serviceGreenHeartbeat(RTC.CNT);
#if WSPD_LED_MODE == 1
  if (!greenPulseActive) {
    // Preserve heartbeat cadence even if foreground work has not returned to
    // the main loop by its next start. The main loop may replace this compare
    // with an earlier report/retry deadline when it is ready to sleep.
    while (RTC.STATUS & RTC_CMPBUSY_bm) { }
    RTC.CMP = nextGreenHeartbeatTick;
    while (RTC.STATUS & RTC_CMPBUSY_bm) { }
    RTC.INTCTRL = RTC_CMP_bm;
  }
#endif
}

static void spdWakeIsr() {
  spdWakeRequested = true;
  // Latch an opening contact even if it closes again before the main loop runs.
  // PD7 supports CHANGE wake in STANDBY despite not being a Px2/Px6 pin.
  if (VPORTD.IN & PIN7_bm) {
    spdFailRequested = true;
#if WSPD_LED_MODE == 1
    ledOn(LED_SPD);
#endif
  } else {
#if WSPD_LED_MODE == 1
    ledOff(LED_SPD);
#endif
  }
}

static uint16_t nowTicks() {
  const uint8_t savedSreg = SREG;
  cli();
  const uint16_t now = RTC.CNT;
  SREG = savedSreg;
  return now;
}

static void setupSleepClock() {
  // The station selects TCB2 for millis. Keep it available for awake RadioLib
  // timeouts, but explicitly prevent it from requesting CLK_PER during STANDBY.
  TCB2.CTRLA &= (uint8_t)~TCB_RUNSTDBY_bm;
  while (RTC.STATUS) { }
  RTC.CTRLA = 0;
  while (RTC.STATUS) { }
  RTC.PITINTCTRL = 0;
  while (RTC.PITSTATUS) { }
  RTC.PITCTRLA = 0;
  RTC.INTCTRL = 0;
  RTC.INTFLAGS = RTC_CMP_bm | RTC_OVF_bm;
  RTC.CLKSEL = RTC_CLKSEL_OSC1K_gc;  // OSC32K / 32 = 1024 Hz
  RTC.CNT = 0;
  while (RTC.STATUS) { }
  RTC.PER = 0xFFFF;
  while (RTC.STATUS) { }
  RTC.CTRLA = RTC_RUNSTDBY_bm | RTC_PRESCALER_DIV32_gc | RTC_RTCEN_bm;
  while (RTC.STATUS) { }

  // Keep continuous active BOD for EEPROM writes. Only the sleep behavior is
  // changed: sampled at 32 Hz instead of continuous while the CPU is stopped.
  _PROTECTED_WRITE(BOD.CTRLA,
      (BOD.CTRLA & (uint8_t)~BOD_SLEEP_gm) |
      BOD_SLEEP_SAMPLED_gc | BOD_SAMPFREQ_bm);
}

static void disableUnusedInputBuffers() {
  // Confirmed NC pins in the AC/DC schematic. Disable their floating digital
  // input buffers without driving a PCB pad; RESET/UPDI and every used pin stay
  // untouched. The thermistor input is disabled separately by its ADC setup.
  PORTA.PIN0CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTD.PIN1CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTD.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTD.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTD.PIN4CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTD.PIN5CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTD.PIN6CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTF.PIN0CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTF.PIN1CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTF.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTF.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;
}

static void signalStatus(uint8_t spd) {
#if WSPD_LED_MODE == 0
  if (radioInitialized) ledOn(LED_PWR);
  else ledOff(LED_PWR);
  if (spd == 0) ledOn(LED_SPD);
  else ledOff(LED_SPD);
#elif WSPD_LED_MODE == 1
  if (spd == 0) ledOn(LED_SPD);
  else ledOff(LED_SPD);
#else
  (void)spd;
#endif
}

static void startGreenHeartbeat(uint16_t now) {
#if WSPD_LED_MODE == 1
  const uint8_t savedSreg = SREG;
  cli();
  ledOn(LED_PWR);
  greenPulseEndTick = (uint16_t)(now + GREEN_ON_TICKS);
  greenPulseActive = true;
  // Anchor subsequent starts to the original 30-second cadence. Skip missed
  // starts rather than replaying several pulses or drifting with radio work.
  do {
    nextGreenHeartbeatTick = (uint16_t)(nextGreenHeartbeatTick + GREEN_HEARTBEAT_TICKS);
  } while (AcDcSleep::reached(now, nextGreenHeartbeatTick));

  // Arm the 0.5-second turn-off now. This also works when called from the RTC
  // ISR during a relatively slow foreground EEPROM commit or transmission.
  RTC.INTCTRL = 0;
  RTC.INTFLAGS = RTC_CMP_bm | RTC_OVF_bm;
  while (RTC.STATUS & RTC_CMPBUSY_bm) { }
  RTC.CMP = greenPulseEndTick;
  while (RTC.STATUS & RTC_CMPBUSY_bm) { }
  if (AcDcSleep::reached(RTC.CNT, greenPulseEndTick)) {
    finishGreenPulse(RTC.CNT);
  } else {
    RTC.INTCTRL = RTC_CMP_bm;
  }
  SREG = savedSreg;
#else
  (void)now;
#endif
}

static void finishGreenPulse(uint16_t now) {
  const uint8_t savedSreg = SREG;
  cli();
  if (greenPulseActive && AcDcSleep::reached(now, greenPulseEndTick)) {
    ledOff(LED_PWR);
    greenPulseActive = false;
  }
  SREG = savedSreg;
}

static void serviceGreenHeartbeat(uint16_t now) {
#if WSPD_LED_MODE == 1
  const uint8_t savedSreg = SREG;
  cli();
  finishGreenPulse(now);
  if (AcDcSleep::reached(now, nextGreenHeartbeatTick)) startGreenHeartbeat(now);
  SREG = savedSreg;
#else
  (void)now;
#endif
}

static void sleepUntilWork() {
  // Heartbeat deadlines are written in the ISR. Snapshot their 16-bit values
  // atomically together with the counter before selecting the earliest work.
  cli();
  const uint16_t now = RTC.CNT;
  uint16_t deadline = radioInitialized ? txSchedule.nextDeadline(now)
                                      : nextRadioRetryTick;
#if WSPD_LED_MODE == 1
  deadline = AcDcSleep::earlier(now, deadline, nextGreenHeartbeatTick);
  if (greenPulseActive) {
    deadline = AcDcSleep::earlier(now, deadline, greenPulseEndTick);
  }
#endif

  set_sleep_mode(SLEEP_MODE_STANDBY);
  cli();
  if (spdWakeRequested || (pendingFailEvent && radioInitialized) ||
      AcDcSleep::reached(RTC.CNT, deadline)) {
    sei();
    return;
  }

  RTC.INTCTRL = 0;
  RTC.INTFLAGS = RTC_CMP_bm | RTC_OVF_bm;
  while (RTC.STATUS & RTC_CMPBUSY_bm) { }
  RTC.CMP = deadline;
  while (RTC.STATUS & RTC_CMPBUSY_bm) { }

  // A deadline/contact may arrive during asynchronous register synchronization.
  // Never clear the newly armed flag after this final check.
  if (spdWakeRequested || AcDcSleep::reached(RTC.CNT, deadline)) {
    sei();
    return;
  }
  RTC.INTCTRL = RTC_CMP_bm;
  sleep_enable();
  // AVR guarantees the instruction following SEI runs before servicing an ISR.
  // SLEEP must immediately follow, so an edge cannot be consumed before sleep.
  sei();
  sleep_cpu();
  sleep_disable();
}

static uint64_t allocateNonce() {
  uint64_t nonce;
  if (!nonceJournal.allocate(nonce)) haltNonceStorageFault();
  return nonce;
}

// A storage fault must never turn into a replayed packet. All LEDs blink together
// until serviced; this differs from the TX-only radio initialization indication.
static void haltNonceStorageFault() {
  cli();
  greenPulseActive = false;
  RTC.INTCTRL = 0;
  sei();
  while (true) {
    ledOn(LED_PWR); ledOn(LED_TX); ledOn(LED_SPD);
    delay(250);
    ledOff(LED_PWR); ledOff(LED_TX); ledOff(LED_SPD);
    delay(250);
  }
}

static void packUint64LE(uint8_t* out, uint64_t value) {
  for (uint8_t i = 0; i < 8; i++) {
    out[i] = (uint8_t)(value >> (8 * i));
  }
}

static void buildBeaconPacket(uint8_t spd, int8_t temp, uint64_t nonce, uint8_t cellVoltageByte, uint8_t* pkt) {
  uint8_t id_status = ((SPD_ID & 0x7F) << 1) | (spd & 0x01);

  uint8_t payload[BEACON_DATA_LEN];
  payload[0] = (uint8_t)(((BEACON_HW_TYPE & 0x0F) << 4) | (BEACON_FIRMWARE_VERSION & 0x0F));
  payload[1] = id_status;
  payload[2] = (uint8_t)temp;
  packUint64LE(&payload[3], nonce);
  payload[11] = cellVoltageByte;

  uint8_t sec4[4] = {
    (uint8_t)(SECRET >> 0),
    (uint8_t)(SECRET >> 8),
    (uint8_t)(SECRET >> 16),
    (uint8_t)(SECRET >> 24),
  };

  uint8_t hash[32];
  Sha256Context ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, payload, sizeof(payload));
  sha256_update(&ctx, sec4, sizeof(sec4));
  sha256_final(&ctx, hash);

  memcpy(pkt, payload, sizeof(payload));
  memcpy(pkt + sizeof(payload), hash, BEACON_HASH_LEN);
}

static bool radioOk(int16_t state) {
  return (state == RADIOLIB_ERR_NONE);
}

static void radioPinsIdle() {
  digitalWrite(LORA_RFSW, LOW);
  digitalWrite(LORA_NSS, HIGH);
  digitalWrite(LORA_SCK, LOW);
  digitalWrite(LORA_MOSI, LOW);
  pinMode(LORA_RFSW, OUTPUT);
  pinMode(LORA_NSS, OUTPUT);
  pinMode(LORA_SCK, OUTPUT);
  pinMode(LORA_MOSI, OUTPUT);
  // The SX1262 releases MISO during sleep; prevent a floating AVR input.
  pinMode(LORA_MISO, INPUT_PULLUP);
}

static void radioSleepRetain() {
  if (radioInitialized) {
    // This command has to finish before disabling the SPI peripheral.
    const int16_t state = radio.sleep(true);
    if (!radioOk(state)) {
      radioInitialized = false;
      // A failed sleep transaction must not leave the radio active indefinitely.
      digitalWrite(LORA_RST, LOW);
    }
  } else {
    digitalWrite(LORA_RST, LOW);
  }
  SPI.end();
  radioPinsIdle();
  if (!radioInitialized) {
    nextRadioRetryTick = (uint16_t)(nowTicks() + AcDcSleep::TICKS_PER_SECOND);
  }
}

static bool transmitPacket(const uint8_t* pkt, size_t len) {
#if WSPD_LED_MODE == 0
  ledOn(LED_TX);
  delay(15);
#endif

  SPI.begin();
  pinMode(LORA_MISO, INPUT);  // restore the live bus after its sleep pull-up
  int16_t state = radio.standby();
  if (radioOk(state)) state = radio.startTransmit(pkt, len);
  if (radioOk(state)) {
#if WSPD_LED_MODE == 1
    ledOn(LED_TX);
#endif
    // Match RadioLib's bounded TX timeout. millis/TCB2 runs only while awake;
    // the long report/retry waits always use RTC CNT instead.
    const uint32_t timeoutMs = 5UL + (radio.getTimeOnAir(len) * 5UL) / 1000UL;
    const uint32_t startedMs = millis();
    while (digitalRead(LORA_DIO1) == LOW) {
      const uint32_t elapsedMs = millis() - startedMs;
#if WSPD_LED_MODE == 1
      if (elapsedMs >= 50UL) ledOff(LED_TX);
#endif
      serviceGreenHeartbeat(nowTicks());
      if (elapsedMs > timeoutMs) {
        state = RADIOLIB_ERR_TX_TIMEOUT;
        break;
      }
    }
    const int16_t finishState = radio.finishTransmit();
    if (radioOk(state)) state = finishState;
  }
  ledOff(LED_TX);
  if (!radioOk(state)) radioInitialized = false;
  radioSleepRetain();
  return radioOk(state) && radioInitialized;
}

static void sendBeaconEvent(uint8_t spd, bool burstFailRepeats) {
  const uint16_t startedTick = nowTicks();
  int8_t tempMeasured = 0;
  uint16_t rawTemp = 0;
  int16_t adcErr = 0;
  const uint8_t tempStatus = readTempC_int8(tempMeasured, rawTemp, adcErr);
  const int8_t temp = chooseTempToTransmit(tempStatus, tempMeasured);
  uint8_t pkt[BEACON_PACKET_LEN];
  uint64_t nonce = allocateNonce();
  const uint8_t cellVoltageByte = readCellVoltageByte();
  buildBeaconPacket(spd, temp, nonce, cellVoltageByte, pkt);

  if (burstFailRepeats) memcpy(failRepeatPacket, pkt, sizeof(pkt));
  txSchedule.regularSent(startedTick, pickNextIntervalTicks());
  signalStatus(readSpdStatusBit());

  transmitPacket(pkt, sizeof(pkt));

  if (burstFailRepeats && SPDFAIL_RETX_COUNT > 0) {
    txSchedule.beginFailRetries(nowTicks(), SPDFAIL_RETX_COUNT,
                                secondsToTicksRounded(SPDFAIL_RETX_DELAY));
  }
}

// ---------------------- HELPERS ----------------------
static uint8_t readSpdStatusBit() {
  // Preserved from old design:
  // contact closed -> input LOW -> SPD_STATUS = 1
  // contact open   -> input HIGH -> SPD_STATUS = 0
  return (digitalRead(PIN_SPD_STATUS) == LOW) ? 1 : 0;
}

static uint8_t readCellVoltageByte() {
  // Previous AVR128DA32 hardware has no battery-voltage measurement path.
  // Keep the new server-side field populated with the agreed sentinel.
  return CELL_VOLTAGE_UNAVAILABLE;
}

// Fallback numeric values for AVR-DA headers.  DxCore's device headers normally
// define all of these; the fallback values are here only to keep the sketch
// source explicit and portable across small header naming differences.
#ifndef VREF_REFSEL_gm
  #define VREF_REFSEL_gm 0x07
#endif
#ifndef ADC_MUXPOS_AIN0_gc
  #define ADC_MUXPOS_AIN0_gc 0x00
#endif
#ifndef ADC_STCONV_bm
  #define ADC_STCONV_bm 0x01
#endif
#ifndef ADC_RESRDY_bm
  #define ADC_RESRDY_bm 0x01
#endif
#ifndef ADC_ENABLE_bm
  #define ADC_ENABLE_bm 0x01
#endif
#ifndef ADC_FREERUN_bm
  #define ADC_FREERUN_bm 0x02
#endif
#ifndef ADC_RESSEL_gm
  #define ADC_RESSEL_gm 0x0C
#endif
#ifndef ADC_RESSEL_10BIT_gc
  #define ADC_RESSEL_10BIT_gc 0x04
#endif
#ifndef ADC_LEFTADJ_bm
  #define ADC_LEFTADJ_bm 0x20
#endif
#ifndef ADC_CONVMODE_bm
  #define ADC_CONVMODE_bm 0x40
#endif
#ifndef ADC_SAMPNUM_NONE_gc
  #define ADC_SAMPNUM_NONE_gc 0x00
#endif
#ifndef ADC_PRESC_DIV32_gc
  #define ADC_PRESC_DIV32_gc 0x08
#endif
#ifndef ADC_DIRECT_TIMEOUT_COUNT
  #define ADC_DIRECT_TIMEOUT_COUNT 100000UL
#endif

static void configureThermoAdc() {
#if defined(PORTD) && defined(PORT_ISC_INPUT_DISABLE_gc) && defined(PORT_ISC_gm) && defined(PORT_PULLUPEN_bm)
  // PD0 is used only as an analog input. Disable the digital input buffer and
  // make sure the internal pull-up is off so it cannot disturb the divider.
  PORTD.PIN0CTRL = (PORTD.PIN0CTRL & (uint8_t)(~(PORT_ISC_gm | PORT_PULLUPEN_bm))) | PORT_ISC_INPUT_DISABLE_gc;
#endif

#if defined(VREF) && defined(ADC0)
  // AVR-DA VREF.ADC0REF REFSEL=0x05 means VDD. Use the literal
  // datasheet value instead of any Arduino-core analogReference()/VDD constant.
  VREF.ADC0REF = (VREF.ADC0REF & (uint8_t)(~VREF_REFSEL_gm)) | 0x05;

  // Configure the ADC hardware directly.  This avoids DxCore 1.6.x
  // analogReference()/analogRead() reference handling bugs.
  ADC0.CTRLA = 0x00;                       // disable while changing mode
  ADC0.MUXPOS = ADC_MUXPOS_AIN0_gc;        // PD0 / AIN0 / TERMO
  ADC0.CTRLB = ADC_SAMPNUM_NONE_gc;        // no hardware accumulation
  ADC0.CTRLD = 0x00;                       // no extra init/sample delay
  ADC0.CTRLE = 0x00;                       // window comparator off

  // Use a conservative ADC clock if the selected device header exposes a
  // suitable prescaler constant. At F_CPU=4 MHz, DIV32 gives about 125 kHz.
  ADC0.CTRLC = ADC_PRESC_DIV32_gc;

  ADC0.SAMPCTRL = TEMP_ADC_SAMPLE_DURATION;
  ADC0.INTFLAGS = ADC_RESRDY_bm;           // clear stale result-ready flag
  ADC0.CTRLA = ADC_ENABLE_bm | ADC_RESSEL_10BIT_gc;  // single-ended, right-adjusted, 10-bit
#else
  // Non-AVR-DA fallback. This path is not expected on the AVR128DA32 board.
  analogReference(VDD);
  (void)analogReadResolution(TEMP_ADC_BITS);
  (void)analogSampleDuration(TEMP_ADC_SAMPLE_DURATION);
#endif
}

static uint16_t normalizeAdcReadingTo10bit(uint16_t value) {
  // The direct ADC path is configured for 10-bit results.  If a future header or
  // core path leaves a 12-bit value in ADC0.RES, scale it instead of converting
  // a valid room-temperature voltage into a fake high-rail result.
  if (value <= TEMP_ADC_MAX) {
    return value;
  }

  if (value <= 4095U) {
    return (uint16_t)((value + 2U) >> 2);  // rounded 12-bit -> 10-bit
  }

  return TEMP_ADC_MAX;
}

static bool readAdc0Ain0Single10(uint16_t& rawOut) {
#if defined(ADC0)
  ADC0.MUXPOS = ADC_MUXPOS_AIN0_gc;
  ADC0.INTFLAGS = ADC_RESRDY_bm;
  ADC0.COMMAND = ADC_STCONV_bm;

  uint32_t guard = ADC_DIRECT_TIMEOUT_COUNT;
  while ((ADC0.INTFLAGS & ADC_RESRDY_bm) == 0) {
    if (--guard == 0) {
      return false;
    }
  }

  rawOut = normalizeAdcReadingTo10bit((uint16_t)ADC0.RES);
  return true;
#else
  int16_t v = analogRead(PIN_THERMO);
  if (v < 0) return false;
  rawOut = normalizeAdcReadingTo10bit((uint16_t)v);
  return true;
#endif
}

static bool readThermoRaw10(uint16_t& rawOut, int16_t& adcErrOut) {
  configureThermoAdc();

  // Discard the first conversion after configuring the ADC/reference/mux.
  // This avoids stale sample-and-hold charge after any previous ADC use.
  uint16_t discard = 0;
  if (!readAdc0Ain0Single10(discard)) {
    adcErrOut = -30000;
#if defined(ADC0)
    ADC0.CTRLA = 0;
#endif
    return false;
  }

  uint32_t sum = 0;
  for (uint8_t i = 0; i < TEMP_ADC_SAMPLES; i++) {
    uint16_t v = 0;
    if (!readAdc0Ain0Single10(v)) {
      adcErrOut = -30000;
#if defined(ADC0)
      ADC0.CTRLA = 0;
#endif
      return false;
    }
    sum += v;
    delay(2);
  }

  adcErrOut = 0;
  rawOut = (uint16_t)((sum + (TEMP_ADC_SAMPLES / 2)) / TEMP_ADC_SAMPLES);
#if defined(ADC0)
  ADC0.CTRLA = 0;  // no ADC/reference activity between measurements
#endif
  return true;
}

static int8_t convertRaw10ToTempC(uint16_t raw) {
  float rawf = (float)raw;

  // Guard rails to avoid divide-by-zero / log(0)
  if (rawf < 1.0f) rawf = 1.0f;
  if (rawf > (float)(TEMP_ADC_MAX - 1U)) rawf = (float)(TEMP_ADC_MAX - 1U);

  // Divider math (NTC to GND, pullup to VCC)
  float ratio = rawf / (float)TEMP_ADC_MAX;
  float r_ntc = R_PULLUP * (ratio / (1.0f - ratio));

  // Beta equation
  float invT  = (1.0f / T0) + (1.0f / BETA) * logf(r_ntc / R0);
  float tempK = 1.0f / invT;
  float tempC = tempK - 273.15f;

  int tempRounded = (int)lroundf(tempC);
  if (tempRounded < -128) tempRounded = -128;
  if (tempRounded >  127) tempRounded =  127;
  return (int8_t)tempRounded;
}

static uint8_t readTempC_int8(int8_t& tempOut, uint16_t& rawOut, int16_t& adcErrOut) {
  if (!readThermoRaw10(rawOut, adcErrOut)) {
    return TEMP_READ_ADC_ERROR;
  }

  // These are not realistic temperatures for this divider; treat as hardware faults.
  if (rawOut <= TEMP_RAW_SHORT_THRESHOLD) {
    return TEMP_READ_SHORT_CIRCUIT;
  }
  if (rawOut >= TEMP_RAW_OPEN_THRESHOLD) {
    return TEMP_READ_OPEN_CIRCUIT;
  }

  tempOut = convertRaw10ToTempC(rawOut);
  return TEMP_READ_OK;
}

static int8_t chooseTempToTransmit(uint8_t status, int8_t latestTemp) {
  if (status == TEMP_READ_OK) {
    lastGoodTempC = latestTemp;
    haveLastGoodTemp = true;
    return latestTemp;
  }

  if (TEMP_FAULT_TX_MODE == TEMP_FAULT_TX_LAST_VALID && haveLastGoodTemp) {
    return lastGoodTempC;
  }

  switch (status) {
    case TEMP_READ_ADC_ERROR:    return TEMP_SENTINEL_ADC_ERROR;
    case TEMP_READ_OPEN_CIRCUIT: return TEMP_SENTINEL_OPEN;
    case TEMP_READ_SHORT_CIRCUIT:return TEMP_SENTINEL_SHORT;
    default:                     return TEMP_SENTINEL_ADC_ERROR;
  }
}

static bool loraInit() {
  radioInitialized = false;
  // Wio-SX1262 external RF_SW pin is HIGH only in RX mode.
  // Hold LOW until RadioLib takes ownership.
  pinMode(LORA_RFSW, OUTPUT);
  digitalWrite(LORA_RFSW, LOW);

  // No custom PORTMUX is needed: the PCB uses the AVR128DA32 default SPI0 pins.
  SPI.begin();
  pinMode(LORA_MISO, INPUT);
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, HIGH);

  int16_t state = radio.begin(
    LORA_FREQ_MHZ,
    LORA_BW_KHZ,
    LORA_SF,
    LORA_CR,
    LORA_SYNCWORD,
    LORA_TX_DBM,
    LORA_PREAMBLE,
    SX1262_TCXO_VOLTAGE,
    SX1262_USE_LDO
  );
  if (!radioOk(state)) return false;

  state = radio.explicitHeader();
  if (!radioOk(state)) return false;

  state = radio.setCRC(LORA_CRC_ENABLED ? 2 : 0);
  if (!radioOk(state)) return false;

  state = radio.setCurrentLimit(SX1262_CURRENT_LIMIT_MA);
  if (!radioOk(state)) return false;

  // Internal TX RF switch control on DIO2.
  state = radio.setDio2AsRfSwitch(true);
  if (!radioOk(state)) return false;

  // External module RF_SW pin: HIGH only in RX, LOW otherwise.
  radio.setRfSwitchPins(LORA_RFSW, RADIOLIB_NC);

  radioInitialized = true;
  return true;
}

// ---------------------- SETUP / LOOP ----------------------
void setup() {
  // LEDs
  pinMode(LED_PWR, OUTPUT);
  pinMode(LED_TX,  OUTPUT);
  pinMode(LED_SPD, OUTPUT);
  ledOff(LED_PWR);
  ledOff(LED_TX);
  ledOff(LED_SPD);

  if (!nonceJournal.begin(WSPD_NONCE_START)) haltNonceStorageFault();

  // Inputs
  pinMode(PIN_SPD_STATUS, INPUT);   // external pullup/front-end assumed present
  pinMode(PIN_THERMO, INPUT);

  disableUnusedInputBuffers();
  setupSleepClock();
  lastSpdStatus = readSpdStatusBit();
  attachInterrupt(digitalPinToInterrupt(PIN_SPD_STATUS), spdWakeIsr, CHANGE);

  // Configure ADC before first use.
  configureThermoAdc();

  // Seed PRNG from ADC noise + timer.
  uint16_t rawSeed = 0;
  int16_t adcErr = 0;
  if (readThermoRaw10(rawSeed, adcErr)) {
    prng_state = ((uint32_t)rawSeed << 16) ^ (uint32_t)micros();
  } else {
    prng_state = (uint32_t)micros() ^ 0x6D2B79F5UL;
  }
  if (prng_state == 0) prng_state = 0x6D2B79F5UL;

  // Prime lastGoodTempC if possible.
  int8_t tempBoot = 0;
  uint16_t rawBoot = 0;
  uint8_t bootStatus = readTempC_int8(tempBoot, rawBoot, adcErr);
  if (bootStatus == TEMP_READ_OK) {
    lastGoodTempC = tempBoot;
    haveLastGoodTemp = true;
  }

  // One initialization attempt; errors retry once per second while otherwise
  // sleeping, rather than keeping the MCU/radio active in an infinite loop.
  (void)loraInit();
  radioSleepRetain();
  signalStatus(readSpdStatusBit());
#if WSPD_LED_MODE == 1
  nextGreenHeartbeatTick = nowTicks();
  serviceGreenHeartbeat(nextGreenHeartbeatTick);  // immediate startup heartbeat
#endif
  txSchedule.begin(nowTicks(), secondsToTicksRounded(START_DELAY_S));
}

void loop() {
  const uint8_t savedSreg = SREG;
  cli();
  const bool contactWake = spdWakeRequested;
  const bool capturedFail = spdFailRequested;
  spdWakeRequested = false;
  spdFailRequested = false;
  SREG = savedSreg;

  const uint8_t spd = readSpdStatusBit();
  const bool statusChanged = spd != lastSpdStatus;
  if (lastSpdStatus == 1 && (spd == 0 || capturedFail)) pendingFailEvent = true;
  lastSpdStatus = spd;
  if (contactWake || statusChanged) signalStatus(spd);

  uint16_t now = nowTicks();
  txSchedule.observe(now);
  serviceGreenHeartbeat(now);

  if (!radioInitialized && AcDcSleep::reached(now, nextRadioRetryTick)) {
    (void)loraInit();
    radioSleepRetain();
#if WSPD_LED_MODE == 0
    signalStatus(spd);
#endif
    if (!radioInitialized) {
#if WSPD_LED_MODE != 2
      ledOn(LED_TX);
      delay(50);
      ledOff(LED_TX);
#endif
    }
  }

  if (radioInitialized) {
    now = nowTicks();
    if (pendingFailEvent) {
      pendingFailEvent = false;
      sendBeaconEvent(0, true);
    } else if (txSchedule.retryDue(now)) {
      // The packet was cached before the first FAIL transmission. Repeats use
      // exactly its original nonce, temperature, status and authentication tag.
      transmitPacket(failRepeatPacket, sizeof(failRepeatPacket));
      txSchedule.retrySent(nowTicks(), secondsToTicksRounded(SPDFAIL_RETX_DELAY));
    } else if (txSchedule.regularDue(now)) {
      sendBeaconEvent(spd, false);
    }
  }

  serviceGreenHeartbeat(nowTicks());
#if WSPD_LED_MODE == 0
  signalStatus(readSpdStatusBit());
#endif
  sleepUntilWork();
}
