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
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <math.h>
#include <string.h>
#include "nonce_eeprom.h"

// -----------------------------------------------------------------------------
// DEHN SPD FM wireless beacon - AVR128DB32 + Wio-SX1262 + LiFePO4 cell
// Hardware type 2 / firmware version 1 / low-power build with button wake.
// Pin map updated for manufactured PCB screenshot: BUTTON1=PD6, SPD_STATUS=PD4, LEDs=PF4/PF3/PF2.
// v4: BUTTON1 wake uses attachInterrupt(), not a manually defined PORTD_PORT_vect ISR.
// v5: SPD status front end is sampled once per minute instead of once per second.
// -----------------------------------------------------------------------------

// ---------------------- NETWORK CONFIG ----------------------
const uint32_t SECRET = WSPD_SHARED_SECRET;
static_assert(WSPD_SHARED_SECRET > 0 && WSPD_SHARED_SECRET <= 0xFFFFFFFFULL,
              "Set a private, nonzero 32-bit WSPD_SHARED_SECRET in config.h.");
static_assert(WSPD_BEACON_ID >= 1 && WSPD_BEACON_ID <= 127,
              "WSPD_BEACON_ID must be 1..127; use 10..127 for production.");
static_assert(WSPD_LORA_TX_DBM >= -9 && WSPD_LORA_TX_DBM <= 22,
              "WSPD_LORA_TX_DBM must be -9..22; obey local RF limits.");
static_assert(WSPD_LORA_SF >= 5 && WSPD_LORA_SF <= 12,
              "WSPD_LORA_SF must be 5..12 and match the receiver.");
const uint8_t  SPD_ID = WSPD_BEACON_ID;   // 1..127 (0 reserved)

// Payload identity byte:
//   bits 7..4 = hardware type: 1 = previous AVR128DA32 hardware, 2 = AVR128DB32 hardware
//   bits 3..0 = firmware version
// Type 0 is reserved. Production firmware version starts at 1.
constexpr uint8_t BEACON_HW_TYPE          = 2;
constexpr uint8_t BEACON_FIRMWARE_VERSION = 1;
static_assert(BEACON_HW_TYPE > 0 && BEACON_HW_TYPE <= 0x0F, "BEACON_HW_TYPE must be 1..15");
static_assert(BEACON_FIRMWARE_VERSION <= 0x0F, "BEACON_FIRMWARE_VERSION must be 0..15");

// On-air frame layout:
//   data[0]      = hardware/firmware byte: (type << 4) | firmware
//   data[1]      = old id/status byte: (SPD_ID << 1) | SPD status
//   data[2]      = signed int8 temperature in deg C, preserving old semantics
//   data[3..10]  = uint64 nonce, little-endian
//   data[11]     = cell voltage byte; 0=2.20V, 1=2.21V, ..., 150=3.70V, 255=error
//   data[12..19] = first 8 bytes of SHA-256(data[0..11] || SECRET_LE32)
constexpr size_t BEACON_DATA_LEN   = 12;
constexpr size_t BEACON_HASH_LEN   = 8;
constexpr size_t BEACON_PACKET_LEN = BEACON_DATA_LEN + BEACON_HASH_LEN;

constexpr uint8_t CELL_VOLTAGE_ERROR = 255;
constexpr uint16_t CELL_VOLTAGE_OFFSET_MV = 2200;
constexpr uint16_t CELL_VOLTAGE_STEP_MV   = 10;
constexpr uint8_t  CELL_VOLTAGE_MAX_CODE  = 150;  // 3.70 V; 151..254 intentionally unused

// First regular transmission happens after this startup delay.
const uint32_t START_DELAY_SEC = 5;

// Regular beacon interval is chosen randomly inside this range.
// Set these to e.g. 600..1800 for 10..30 minute randomized beaconing.
const uint32_t TX_INTERVAL_SEC_MIN = 500;
const uint32_t TX_INTERVAL_SEC_MAX = 600;

// SPD status is sampled periodically while the MCU otherwise sleeps.
// 60 s avoids powering the SPD status divider every second.
// The RTC PIT still provides the 1 Hz low-power scheduler tick; actual SPD front-end
// power and GPIO/ADC work are only done when this interval expires.
const uint16_t SPD_SAMPLE_INTERVAL_SEC = 60;

// LED policy requested for low-power operation.
const uint16_t LED_BLINK_INTERVAL_SEC = 60;
const uint16_t LED_BLINK_MS           = 50;
const uint16_t TX_LED_BLINK_MS        = 100;

// BUTTON1 is used as an immediate manual transmit request. PD6 is one of
// the AVR Dx fully asynchronous Px6 interrupt pins, so it can wake from
// Power-down on a falling edge. The callback is registered through
// attachInterrupt() rather than by defining ISR(PORTD_PORT_vect), because
// DxCore/RadioLib also use the shared port interrupt vector machinery.
const uint16_t BUTTON_DEBOUNCE_MS = 100;
const uint16_t BUTTON_MIN_TX_GAP_SEC = 5;

// On transition OK -> FAIL, send one immediate FAIL beacon and then repeat the
// exact same payload this many additional times.
const uint8_t  SPDFAIL_RETX_COUNT = 2;      // 0 = disabled
const uint16_t SPDFAIL_RETX_DELAY_SEC = 5;  // seconds between repeats

// LoRa RF settings
// Channel map:
//   0=865.1  1=865.3  2=865.5  3=865.7
//   4=865.9  5=866.1  6=866.3  7=866.5
const uint8_t LORA_CHANNEL_ID   = WSPD_LORA_CHANNEL_ID;
const int8_t  LORA_TX_DBM       = WSPD_LORA_TX_DBM;   // SX1262 allowed range depends on module/RF path
const uint8_t LORA_SF           = WSPD_LORA_SF;
const float   LORA_BW_KHZ       = 125.0f;
const uint8_t LORA_CR           = 5;    // coding rate 4/5 in RadioLib naming
const uint8_t LORA_SYNCWORD     = 0x12; // private network sync word
const uint16_t LORA_PREAMBLE    = 8;

// Wio-SX1262 uses an active TCXO powered from DIO3.
const float SX1262_TCXO_VOLTAGE = 1.8f;

// Seeed documents this module as DC-DC powered, not LDO-only.
const bool  SX1262_USE_LDO = false;

// RadioLib default current limit can be lower than desired for higher TX powers.
const float SX1262_CURRENT_LIMIT_MA = 140.0f;

// Non-blocking transmit safety timeout. This is intentionally much longer than
// expected LoRa time-on-air for the small 20-byte packet even at slow SF values.
const uint32_t LORA_TX_TIMEOUT_MS = 10000UL;

constexpr float LORA_CHANNEL_FREQS_MHZ[] = {
  865.1f, 865.3f, 865.5f, 865.7f,
  865.9f, 866.1f, 866.3f, 866.5f
};
constexpr size_t LORA_CHANNEL_COUNT = sizeof(LORA_CHANNEL_FREQS_MHZ) / sizeof(LORA_CHANNEL_FREQS_MHZ[0]);
static_assert(LORA_CHANNEL_ID < LORA_CHANNEL_COUNT, "Invalid LORA_CHANNEL_ID");

// ---------------------- PIN MAPPING ----------------------
// Schematic DEHNSPDFM-wireless-003 / AVR128DB32-E/PT:
//   SPI0 default hardware pins are used:
//     MOSI = PA4, MISO = PA5, SCK = PA6, NSS/SS = PA7
//   SX1262 control pins:
//     RF_SW = PC0, NRST = PC1, BUSY = PC2, DIO1 = PC3
//
// No custom PORTMUX or software SPI is used. SPI.begin() uses the DxCore
// default SPI0 route for this pin group.
const uint8_t LORA_MOSI  = PIN_PA4;
const uint8_t LORA_MISO  = PIN_PA5;
const uint8_t LORA_SCK   = PIN_PA6;
const uint8_t LORA_NSS   = PIN_PA7;
const uint8_t LORA_RFSW  = PIN_PC0;
const uint8_t LORA_RST   = PIN_PC1;
const uint8_t LORA_BUSY  = PIN_PC2;
const uint8_t LORA_DIO1  = PIN_PC3;

// New low-power sensor front ends.
const uint8_t PIN_THERMO     = PIN_PD1;  // TERMO / ADC AIN1
const uint8_t PIN_NTC_PWR    = PIN_PD2;  // powers thermistor divider only during reading
const uint8_t PIN_SPD_PWR    = PIN_PD3;  // powers SPD status divider only during reading
const uint8_t PIN_SPD_STATUS = PIN_PD4;

// Momentary button from manufactured PCB schematic. SW1 connects BUTTON1/PD6 to GND.
// Internal pull-up is enabled; pressed = LOW.
const uint8_t PIN_BUTTON1 = PIN_PD6;

// LEDs are active-low.
const uint8_t LED_PWR = PIN_PF4;
const uint8_t LED_TX  = PIN_PF3;
const uint8_t LED_SPD = PIN_PF2;

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
// New PCB: NTC_PWR_GPIO -- R4 10k -- NTC node -- external NTC -- GND.
// TERMO/PD1 samples the NTC node through R5 1k with C9 100 nF to GND.
constexpr uint8_t  ADC_BITS                  = 12;
constexpr uint16_t ADC_MAX                   = (1U << ADC_BITS) - 1U;
constexpr uint8_t  TEMP_ADC_SAMPLES          = 8;
constexpr uint8_t  VOLTAGE_ADC_SAMPLES       = 16;
constexpr uint8_t  ADC_SAMPLE_DURATION       = 31;
constexpr uint16_t TEMP_RAW_SHORT_THRESHOLD  = 20;              // near GND => short/fault
constexpr uint16_t TEMP_RAW_OPEN_THRESHOLD   = ADC_MAX - 20U;   // near VDD => open/fault
constexpr uint16_t NTC_SETTLE_MS             = 5;
constexpr uint16_t SPD_STATUS_SETTLE_MS      = 5;
constexpr uint16_t VREF_SETTLE_US            = 1000;

// Fault behavior on the air:
//   TEMP_FAULT_TX_SENTINEL   -> send impossible temperatures for easy log diagnosis
//   TEMP_FAULT_TX_LAST_VALID -> keep last good temperature instead
// The server still receives the same signed int8 temperature field as before.
enum TempFaultTxMode : uint8_t {
  TEMP_FAULT_TX_SENTINEL = 0,
  TEMP_FAULT_TX_LAST_VALID = 1,
};
const TempFaultTxMode TEMP_FAULT_TX_MODE = TEMP_FAULT_TX_SENTINEL;

// Sentinel temperatures, intentionally impossible here.
const int8_t TEMP_SENTINEL_ADC_ERROR = -128;
const int8_t TEMP_SENTINEL_OPEN      = -127;
const int8_t TEMP_SENTINEL_SHORT     = -126;

enum : uint8_t {
  TEMP_READ_OK = 0,
  TEMP_READ_ADC_ERROR,
  TEMP_READ_OPEN_CIRCUIT,
  TEMP_READ_SHORT_CIRCUIT,
};

// ---------------------- REGISTER FALLBACKS ----------------------
// DxCore/device headers normally provide these names.  Numeric fallbacks match
// the AVR128DB data sheet values and keep the sketch explicit.
#ifndef VREF_REFSEL_gm
  #define VREF_REFSEL_gm 0x07
#endif
#ifndef VREF_REFSEL_1V024_gc
  #define VREF_REFSEL_1V024_gc 0x00
#endif
#ifndef VREF_REFSEL_VDD_gc
  #define VREF_REFSEL_VDD_gc 0x05
#endif
#ifndef ADC_MUXPOS_AIN1_gc
  #define ADC_MUXPOS_AIN1_gc 0x01
#endif
#ifndef ADC_MUXPOS_VDDDIV10_gc
  #define ADC_MUXPOS_VDDDIV10_gc 0x44
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
#ifndef ADC_RESSEL_12BIT_gc
  #define ADC_RESSEL_12BIT_gc 0x00
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
#ifndef PORT_ISC_gm
  #define PORT_ISC_gm 0x07
#endif
#ifndef PORT_ISC_INTDISABLE_gc
  #define PORT_ISC_INTDISABLE_gc 0x00
#endif
#ifndef PORT_ISC_LEVEL_gc
  #define PORT_ISC_LEVEL_gc 0x05
#endif
#ifndef PORT_ISC_FALLING_gc
  #define PORT_ISC_FALLING_gc 0x03
#endif
#ifndef PORT_PULLUPEN_bm
  #define PORT_PULLUPEN_bm 0x08
#endif
#ifndef PIN6_bm
  #define PIN6_bm (1 << 6)
#endif

#ifndef RTC_CLKSEL_OSC32K_gc
  #define RTC_CLKSEL_OSC32K_gc 0x00
#endif
#ifndef RTC_PERIOD_CYC32768_gc
  #define RTC_PERIOD_CYC32768_gc 0x70
#endif
#ifndef RTC_PITEN_bm
  #define RTC_PITEN_bm 0x01
#endif
#ifndef RTC_PI_bm
  #define RTC_PI_bm 0x01
#endif

// ---------------------- SIMPLE PRNG ----------------------
static uint32_t prng_state = 0x12345678UL;

static uint32_t xorshift32() {
  uint32_t x = prng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  prng_state = x;
  return x;
}

static uint32_t randomRangeSec(uint32_t lo, uint32_t hi) {
  if (hi < lo) {
    uint32_t tmp = hi;
    hi = lo;
    lo = tmp;
  }
  if (hi == lo) return lo;

  const uint32_t span = hi - lo + 1UL;
  return lo + (xorshift32() % span);
}

static uint32_t pickNextIntervalSec() {
  return randomRangeSec(TX_INTERVAL_SEC_MIN, TX_INTERVAL_SEC_MAX);
}

// ---------------------- LOW-POWER TIMEBASE ----------------------
static volatile uint32_t rtcSeconds = 0;
static volatile bool rtcTick = false;
static volatile bool buttonWakeRequested = false;

ISR(RTC_PIT_vect) {
  RTC.PITINTFLAGS = RTC_PI_bm;  // clear by writing 1
  rtcSeconds++;
  rtcTick = true;
}

static void buttonWakeIsr() {
  // Keep this as short as possible. The main loop performs debounce,
  // packet-rate limiting, measurements and radio work.
  buttonWakeRequested = true;
}

static uint32_t nowSeconds() {
  uint32_t s;
  noInterrupts();
  s = rtcSeconds;
  interrupts();
  return s;
}

static void setupRtcPit1Hz() {
#if defined(RTC)
  while (RTC.STATUS > 0) { }
  RTC.CLKSEL = RTC_CLKSEL_OSC32K_gc;   // 32.768 kHz internal OSC32K

  while (RTC.PITSTATUS > 0) { }
  RTC.PITINTCTRL = RTC_PI_bm;
  RTC.PITINTFLAGS = RTC_PI_bm;
  RTC.PITCTRLA = RTC_PERIOD_CYC32768_gc | RTC_PITEN_bm;  // 1 Hz from 32.768 kHz
#endif
}

static void enterPowerDownSleep() {
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);

  noInterrupts();
  sleep_enable();
#if defined(BODS) && defined(BODSE)
  sleep_bod_disable();
#endif
  interrupts();

  sleep_cpu();
  sleep_disable();
}

static void sleepUntilSecond(uint32_t targetSecond) {
  while ((int32_t)(nowSeconds() - targetSecond) < 0) {
    enterPowerDownSleep();
  }
}

// ---------------------- RADIO ----------------------
// Module(cs, irq, rst, gpio/busy). RadioLib uses Arduino SPI by default, so
// this uses the AVR128DB32 hardware SPI0 pins PA4/PA5/PA6 plus PA7 as NSS.
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// ---------------------- STATE ----------------------
static AvrNonceStorage nonceStorage;
static PersistentNonceJournal<AvrNonceStorage> nonceJournal(nonceStorage);
static uint32_t nextTxSec = 0;
static uint32_t nextStatusSampleSec = 0;
static uint32_t nextLedBlinkSec = 0;
static uint8_t lastSpdStatus = 1;
static bool    haveLastGoodTemp = false;
static int8_t  lastGoodTempC = 25;
static bool    radioInitialized = false;
static bool    haveLastButtonTxSec = false;
static uint32_t lastButtonTxSec = 0;

// ---------------------- FORWARD DECLARATIONS ----------------------
static bool radioOk(int16_t state);
static bool loraInit();
static void radioPinsIdle();
static bool loraInitFail();
static void radioSleepRetain();
static bool transmitPacket(const uint8_t* pkt, size_t len);
static void sendBeaconEvent(uint8_t spd, int8_t temp, uint8_t cellVoltageByte, bool burstFailRepeats);
static uint8_t readSpdStatusBit();
static uint8_t readCellVoltageByte();
static bool readCellVoltageMv(uint16_t& mvOut);
static void configureLowPowerPins();
static void configureAnalogPins();
static void configureUnusedPinsForLowPower();
static bool adcReadAverage12(uint8_t mux, uint8_t vrefSel, uint8_t samples, uint16_t& rawOut);
static int8_t convertRawToTempC(uint16_t raw);
static uint8_t readTempC_int8(int8_t& tempOut, uint16_t& rawOut);
static int8_t chooseTempToTransmit(uint8_t status, int8_t latestTemp);
static void armButtonWakeInterrupt();
static void detachButtonWakeInterrupt();
static bool consumeButtonWakeRequest();
static bool buttonTransmitAllowed(uint32_t now);
static void waitForButtonReleaseAndRearm();
static void handleButtonWakeRequest();
static void blinkStatusLeds();
static void buildAndSendCurrentBeacon(uint8_t spd, bool burstFailRepeats);
static void haltNonceStorageFault() __attribute__((noreturn));

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
  pinMode(LORA_MISO, INPUT);
}

static bool loraInitFail() {
  radioInitialized = false;
  SPI.end();
  radioPinsIdle();
  return false;
}

static uint64_t allocateNonce() {
  uint64_t nonce;
  if (!nonceJournal.allocate(nonce)) haltNonceStorageFault();
  return nonce;
}

// Never emit a replay after a storage failure; all LEDs blink until serviced.
static void haltNonceStorageFault() {
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
  const uint8_t id_status = ((SPD_ID & 0x7F) << 1) | (spd & 0x01);

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

static bool loraInit() {
  pinMode(LORA_RFSW, OUTPUT);
  digitalWrite(LORA_RFSW, LOW);

  SPI.begin();

  int16_t state = radio.begin(
    LORA_CHANNEL_FREQS_MHZ[LORA_CHANNEL_ID],
    LORA_BW_KHZ,
    LORA_SF,
    LORA_CR,
    LORA_SYNCWORD,
    LORA_TX_DBM,
    LORA_PREAMBLE,
    SX1262_TCXO_VOLTAGE,
    SX1262_USE_LDO
  );
  if (!radioOk(state)) return loraInitFail();

  state = radio.explicitHeader();
  if (!radioOk(state)) return loraInitFail();

  state = radio.setCRC(2);
  if (!radioOk(state)) return loraInitFail();

  state = radio.setCurrentLimit(SX1262_CURRENT_LIMIT_MA);
  if (!radioOk(state)) return loraInitFail();

  // Internal TX RF switch control on SX1262 DIO2.
  state = radio.setDio2AsRfSwitch(true);
  if (!radioOk(state)) return loraInitFail();

  // External Wio RF_SW pin: HIGH only in RX, LOW otherwise.
  // This beacon only transmits, so it normally remains LOW.
  radio.setRfSwitchPins(LORA_RFSW, RADIOLIB_NC);

  radioInitialized = true;
  return true;
}

static void radioSleepRetain() {
  if (radioInitialized) {
    // This SPI transaction must be done before SPI.end().
    (void)radio.sleep(true);   // retain LoRa configuration for quick wake-up
  }

  // Disable the hardware SPI peripheral while asleep.  The pins are then held
  // at defined idle levels to avoid floating inputs and unnecessary leakage.
  SPI.end();
  radioPinsIdle();
}

static bool transmitPacket(const uint8_t* pkt, size_t len) {
  if (!radioInitialized) {
    if (!loraInit()) {
      return false;
    }
  }

  // radioSleepRetain() disables SPI between transmissions; re-enable it before
  // waking the SX1262 or accessing any registers.
  SPI.begin();

  int16_t state = radio.standby();
  if (!radioOk(state)) {
    radioInitialized = false;
    if (!loraInit()) {
      radioSleepRetain();
      return false;
    }
  }

  // DIO1 should be low after the previous finishTransmit(); if it is still high,
  // finish/clear once before starting a new non-blocking TX.
  if (digitalRead(LORA_DIO1) == HIGH) {
    (void)radio.finishTransmit();
  }

  state = radio.startTransmit(pkt, len);
  if (!radioOk(state)) {
    radioSleepRetain();
    return false;
  }

  const uint32_t startMs = millis();
  bool txLedOn = true;
  ledOn(LED_TX);

  while (digitalRead(LORA_DIO1) == LOW) {
    const uint32_t elapsed = millis() - startMs;
    if (txLedOn && elapsed >= TX_LED_BLINK_MS) {
      ledOff(LED_TX);
      txLedOn = false;
    }
    if (elapsed >= LORA_TX_TIMEOUT_MS) {
      ledOff(LED_TX);
      radioInitialized = false;
      (void)loraInit();
      radioSleepRetain();
      return false;
    }
    delay(1);
  }

  ledOff(LED_TX);
  state = radio.finishTransmit();
  radioSleepRetain();
  return radioOk(state);
}

static void sendBeaconEvent(uint8_t spd, int8_t temp, uint8_t cellVoltageByte, bool burstFailRepeats) {
  uint8_t pkt[BEACON_PACKET_LEN];
  const uint64_t nonce = allocateNonce();
  buildBeaconPacket(spd, temp, nonce, cellVoltageByte, pkt);

  (void)transmitPacket(pkt, sizeof(pkt));

  if (burstFailRepeats && SPDFAIL_RETX_COUNT > 0) {
    for (uint8_t i = 0; i < SPDFAIL_RETX_COUNT; i++) {
      if (SPDFAIL_RETX_DELAY_SEC > 0) {
        sleepUntilSecond(nowSeconds() + SPDFAIL_RETX_DELAY_SEC);
      }
      (void)transmitPacket(pkt, sizeof(pkt));
    }
  }
}

// ---------------------- INPUTS / ADC ----------------------
static uint8_t readSpdStatusBit() {
  // Same logical semantics as old hardware:
  // contact closed -> input LOW  -> SPD status bit = 1 (OK)
  // contact open   -> input HIGH -> SPD status bit = 0 (FAIL)
  digitalWrite(PIN_SPD_PWR, HIGH);
  delay(SPD_STATUS_SETTLE_MS);
  const uint8_t pinState = digitalRead(PIN_SPD_STATUS);
  digitalWrite(PIN_SPD_PWR, LOW);

  return (pinState == LOW) ? 1 : 0;
}

static void configureAnalogPins() {
#if defined(PORTD) && defined(PORT_ISC_INPUT_DISABLE_gc) && defined(PORT_ISC_gm) && defined(PORT_PULLUPEN_bm)
  // PD1/TERMO is analog only; disable digital input buffer and internal pull-up.
  PORTD.PIN1CTRL = (PORTD.PIN1CTRL & (uint8_t)(~(PORT_ISC_gm | PORT_PULLUPEN_bm))) | PORT_ISC_INPUT_DISABLE_gc;
#endif
}

static bool adcReadSingle12(uint8_t mux, uint16_t& rawOut) {
#if defined(ADC0)
  ADC0.MUXPOS = mux;
  ADC0.INTFLAGS = ADC_RESRDY_bm;
  ADC0.COMMAND = ADC_STCONV_bm;

  uint32_t guard = ADC_DIRECT_TIMEOUT_COUNT;
  while ((ADC0.INTFLAGS & ADC_RESRDY_bm) == 0) {
    if (--guard == 0) {
      return false;
    }
  }

  rawOut = (uint16_t)ADC0.RES;
  if (rawOut > ADC_MAX) rawOut = ADC_MAX;
  return true;
#else
  (void)mux;
  (void)rawOut;
  return false;
#endif
}

static bool adcReadAverage12(uint8_t mux, uint8_t vrefSel, uint8_t samples, uint16_t& rawOut) {
#if defined(ADC0) && defined(VREF)
  if (samples == 0) samples = 1;

  ADC0.CTRLA = 0x00;  // disable while changing mode/reference
  VREF.ADC0REF = (VREF.ADC0REF & (uint8_t)(~VREF_REFSEL_gm)) | (vrefSel & VREF_REFSEL_gm);
  delayMicroseconds(VREF_SETTLE_US);

  ADC0.CTRLB = ADC_SAMPNUM_NONE_gc;
  ADC0.CTRLC = ADC_PRESC_DIV32_gc;
  ADC0.CTRLD = 0x00;
  ADC0.CTRLE = 0x00;
  ADC0.SAMPCTRL = ADC_SAMPLE_DURATION;
  ADC0.MUXPOS = mux;
  ADC0.INTFLAGS = ADC_RESRDY_bm;
  ADC0.CTRLA = ADC_ENABLE_bm | ADC_RESSEL_12BIT_gc;  // single-ended, right-adjusted, 12-bit

  uint16_t discard = 0;
  if (!adcReadSingle12(mux, discard)) {
    ADC0.CTRLA = 0x00;
    return false;
  }

  uint32_t sum = 0;
  for (uint8_t i = 0; i < samples; i++) {
    uint16_t v = 0;
    if (!adcReadSingle12(mux, v)) {
      ADC0.CTRLA = 0x00;
      return false;
    }
    sum += v;
  }

  ADC0.CTRLA = 0x00;  // ADC disabled between measurements to minimize sleep current
  rawOut = (uint16_t)((sum + (samples / 2U)) / samples);
  return true;
#else
  (void)mux;
  (void)vrefSel;
  (void)samples;
  (void)rawOut;
  return false;
#endif
}

static int8_t convertRawToTempC(uint16_t raw) {
  float rawf = (float)raw;

  if (rawf < 1.0f) rawf = 1.0f;
  if (rawf > (float)(ADC_MAX - 1U)) rawf = (float)(ADC_MAX - 1U);

  // Divider math: NTC to GND, 10k pull-up to NTC_PWR_GPIO/VCC.
  const float ratio = rawf / (float)ADC_MAX;
  const float r_ntc = R_PULLUP * (ratio / (1.0f - ratio));

  const float invT  = (1.0f / T0) + (1.0f / BETA) * logf(r_ntc / R0);
  const float tempK = 1.0f / invT;
  const float tempC = tempK - 273.15f;

  int tempRounded = (int)lroundf(tempC);
  if (tempRounded < -128) tempRounded = -128;
  if (tempRounded >  127) tempRounded =  127;
  return (int8_t)tempRounded;
}

static uint8_t readTempC_int8(int8_t& tempOut, uint16_t& rawOut) {
  digitalWrite(PIN_NTC_PWR, HIGH);
  delay(NTC_SETTLE_MS);

  const bool ok = adcReadAverage12(ADC_MUXPOS_AIN1_gc, VREF_REFSEL_VDD_gc, TEMP_ADC_SAMPLES, rawOut);

  digitalWrite(PIN_NTC_PWR, LOW);

  if (!ok) {
    return TEMP_READ_ADC_ERROR;
  }
  if (rawOut <= TEMP_RAW_SHORT_THRESHOLD) {
    return TEMP_READ_SHORT_CIRCUIT;
  }
  if (rawOut >= TEMP_RAW_OPEN_THRESHOLD) {
    return TEMP_READ_OPEN_CIRCUIT;
  }

  tempOut = convertRawToTempC(rawOut);
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
    case TEMP_READ_ADC_ERROR:     return TEMP_SENTINEL_ADC_ERROR;
    case TEMP_READ_OPEN_CIRCUIT:  return TEMP_SENTINEL_OPEN;
    case TEMP_READ_SHORT_CIRCUIT: return TEMP_SENTINEL_SHORT;
    default:                      return TEMP_SENTINEL_ADC_ERROR;
  }
}

static bool readCellVoltageMv(uint16_t& mvOut) {
  uint16_t raw = 0;
  if (!adcReadAverage12(ADC_MUXPOS_VDDDIV10_gc, VREF_REFSEL_1V024_gc, VOLTAGE_ADC_SAMPLES, raw)) {
    return false;
  }

  if (raw == 0 || raw >= ADC_MAX) {
    return false;
  }

  // ADC input is VDD/10 and the ADC reference is internal 1.024V.
  // VDD[mV] = raw / 4095 * 1024mV * 10.
  const uint32_t mv = ((uint32_t)raw * 1024UL * 10UL + (ADC_MAX / 2U)) / ADC_MAX;
  if (mv > 65535UL) {
    return false;
  }

  mvOut = (uint16_t)mv;
  return true;
}

static uint8_t readCellVoltageByte() {
  uint16_t mv = 0;
  if (!readCellVoltageMv(mv)) {
    return CELL_VOLTAGE_ERROR;
  }

  int32_t code = ((int32_t)mv - (int32_t)CELL_VOLTAGE_OFFSET_MV + (CELL_VOLTAGE_STEP_MV / 2)) / CELL_VOLTAGE_STEP_MV;
  if (code < 0) code = 0;
  if (code > CELL_VOLTAGE_MAX_CODE) code = CELL_VOLTAGE_MAX_CODE;
  return (uint8_t)code;
}

// ---------------------- BUTTON WAKE ----------------------
static void armButtonWakeInterrupt() {
  const uint8_t sreg = SREG;
  cli();
  buttonWakeRequested = false;
  SREG = sreg;

  // Make sure the button input is high before arming the falling-edge wake.
  // SW1 shorts BUTTON1 to GND, so pressed = LOW.
  pinMode(PIN_BUTTON1, INPUT_PULLUP);
#if defined(PORTD) && defined(PIN6_bm)
  PORTD.INTFLAGS = PIN6_bm;  // clear any stale PD6 flag before attaching
#endif
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON1), buttonWakeIsr, FALLING);
}

static void detachButtonWakeInterrupt() {
  detachInterrupt(digitalPinToInterrupt(PIN_BUTTON1));
  pinMode(PIN_BUTTON1, INPUT_PULLUP);
#if defined(PORTD) && defined(PIN6_bm)
  PORTD.INTFLAGS = PIN6_bm;  // discard any bounce/stale flag while detached
#endif
}

static bool consumeButtonWakeRequest() {
  bool pending;

  const uint8_t sreg = SREG;
  cli();
  pending = buttonWakeRequested;
  buttonWakeRequested = false;
  SREG = sreg;

  return pending;
}

static bool buttonTransmitAllowed(uint32_t now) {
  if (!haveLastButtonTxSec) {
    return true;
  }

  return ((uint32_t)(now - lastButtonTxSec) >= (uint32_t)BUTTON_MIN_TX_GAP_SEC);
}

static void waitForButtonReleaseAndRearm() {
  // The button interrupt is intentionally detached while the button is held.
  // During a long hold, sleep on the 1 Hz PIT wake source and check again.
  for (;;) {
    while (digitalRead(PIN_BUTTON1) == LOW) {
      sleepUntilSecond(nowSeconds() + 1);
    }

    delay(BUTTON_DEBOUNCE_MS);
    if (digitalRead(PIN_BUTTON1) == HIGH) {
      break;
    }
  }

  armButtonWakeInterrupt();
}

static void handleButtonWakeRequest() {
  // Stop new button callbacks while this press is being debounced/handled.
  // The interrupt is re-armed only after the button is released.
  detachButtonWakeInterrupt();

  // Reject very short glitches before spending energy on ADC/radio work.
  delay(BUTTON_DEBOUNCE_MS);

  if (digitalRead(PIN_BUTTON1) == LOW) {
    const uint32_t nowBeforeTx = nowSeconds();

    if (buttonTransmitAllowed(nowBeforeTx)) {
      // Count a manual radio attempt immediately so bounce/repeated presses
      // cannot create more than one button-triggered packet inside 5 seconds.
      haveLastButtonTxSec = true;
      lastButtonTxSec = nowBeforeTx;

      const uint8_t spd = readSpdStatusBit();
      lastSpdStatus = spd;

      buildAndSendCurrentBeacon(spd, false);

      // Manual send counts as the latest report.  Do not send another scheduled
      // packet immediately afterwards; restart the regular random interval.
      const uint32_t nowAfterTx = nowSeconds();
      nextTxSec = nowAfterTx + pickNextIntervalSec();
      nextStatusSampleSec = nowAfterTx + SPD_SAMPLE_INTERVAL_SEC;
      nextLedBlinkSec = nowAfterTx + LED_BLINK_INTERVAL_SEC;
    }
  }

  waitForButtonReleaseAndRearm();
}

// ---------------------- APPLICATION HELPERS ----------------------
static void blinkStatusLeds() {
  // PWR heartbeat always blinks once per 10 seconds.
  ledOn(LED_PWR);
  if (lastSpdStatus == 0) {
    ledOn(LED_SPD);
  }

  delay(LED_BLINK_MS);

  ledOff(LED_PWR);
  ledOff(LED_SPD);
}

static void buildAndSendCurrentBeacon(uint8_t spd, bool burstFailRepeats) {
  int8_t tempMeasured = 0;
  uint16_t rawTemp = 0;
  const uint8_t tempStatus = readTempC_int8(tempMeasured, rawTemp);
  const int8_t tempToSend = chooseTempToTransmit(tempStatus, tempMeasured);
  const uint8_t cellVoltageByte = readCellVoltageByte();

  sendBeaconEvent(spd, tempToSend, cellVoltageByte, burstFailRepeats);
}

static void configureUnusedPinsForLowPower() {
  // Manufactured PCB screenshot shows these MCU pads as not connected. Drive
  // them LOW as outputs to avoid floating-input leakage. Do not touch UPDI or
  // PF6/RESET.
  const uint8_t unusedPins[] = {
    PIN_PA0, PIN_PA1, PIN_PA2, PIN_PA3,
    PIN_PD5, PIN_PD7,
    PIN_PF0, PIN_PF1, PIN_PF5
  };

  for (uint8_t i = 0; i < (sizeof(unusedPins) / sizeof(unusedPins[0])); i++) {
    digitalWrite(unusedPins[i], LOW);
    pinMode(unusedPins[i], OUTPUT);
  }
}

static void configureLowPowerPins() {
  configureUnusedPinsForLowPower();

  // Active-low LEDs. Write HIGH before switching to output to avoid a visible glitch.
  digitalWrite(LED_PWR, HIGH);
  digitalWrite(LED_TX,  HIGH);
  digitalWrite(LED_SPD, HIGH);
  pinMode(LED_PWR, OUTPUT);
  pinMode(LED_TX,  OUTPUT);
  pinMode(LED_SPD, OUTPUT);

  // Sensor excitation pins default LOW, so no divider current flows while asleep.
  digitalWrite(PIN_NTC_PWR, LOW);
  digitalWrite(PIN_SPD_PWR, LOW);
  pinMode(PIN_NTC_PWR, OUTPUT);
  pinMode(PIN_SPD_PWR, OUTPUT);

  pinMode(PIN_THERMO, INPUT);
  pinMode(PIN_SPD_STATUS, INPUT);
  pinMode(PIN_BUTTON1, INPUT_PULLUP);

  // Radio control pins and hardware SPI idle levels. RadioLib/SPI will
  // reconfigure the SPI pins as needed when SPI.begin() is called.
  digitalWrite(LORA_NSS, HIGH);
  digitalWrite(LORA_SCK, LOW);
  digitalWrite(LORA_MOSI, LOW);
  digitalWrite(LORA_RFSW, LOW);
  pinMode(LORA_NSS, OUTPUT);
  pinMode(LORA_SCK, OUTPUT);
  pinMode(LORA_MOSI, OUTPUT);
  pinMode(LORA_MISO, INPUT);
  pinMode(LORA_RFSW, OUTPUT);
  pinMode(LORA_BUSY, INPUT);
  pinMode(LORA_DIO1, INPUT);
  pinMode(LORA_RST, OUTPUT);
  digitalWrite(LORA_RST, HIGH);

  configureAnalogPins();
}

// ---------------------- SETUP / LOOP ----------------------
void setup() {
  configureLowPowerPins();
  if (!nonceJournal.begin(WSPD_NONCE_START)) haltNonceStorageFault();
  setupRtcPit1Hz();
  armButtonWakeInterrupt();
  sei();

  // Seed PRNG from ADC readings and timer jitter.
  uint16_t seedA = 0;
  uint16_t seedB = 0;
  (void)adcReadAverage12(ADC_MUXPOS_VDDDIV10_gc, VREF_REFSEL_1V024_gc, 4, seedA);
  digitalWrite(PIN_NTC_PWR, HIGH);
  delay(NTC_SETTLE_MS);
  (void)adcReadAverage12(ADC_MUXPOS_AIN1_gc, VREF_REFSEL_VDD_gc, 4, seedB);
  digitalWrite(PIN_NTC_PWR, LOW);
  prng_state = ((uint32_t)seedA << 20) ^ ((uint32_t)seedB << 4) ^ (uint32_t)micros() ^ 0x6D2B79F5UL;
  if (prng_state == 0) prng_state = 0x6D2B79F5UL;

  // Prime lastGoodTempC if possible.
  int8_t tempBoot = 0;
  uint16_t rawBoot = 0;
  const uint8_t bootTempStatus = readTempC_int8(tempBoot, rawBoot);
  if (bootTempStatus == TEMP_READ_OK) {
    lastGoodTempC = tempBoot;
    haveLastGoodTemp = true;
  }

  // Initialize radio once, then immediately put it into sleep-retain mode.
  // If it does not initialize, retry once per RTC tick while otherwise sleeping.
  while (!loraInit()) {
    ledOn(LED_TX);
    delay(LED_BLINK_MS);
    ledOff(LED_TX);
    sleepUntilSecond(nowSeconds() + 1);
  }
  radioSleepRetain();

  lastSpdStatus = readSpdStatusBit();

  const uint32_t now = nowSeconds();
  nextTxSec = now + START_DELAY_SEC;
  nextStatusSampleSec = now + SPD_SAMPLE_INTERVAL_SEC;
  nextLedBlinkSec = now + LED_BLINK_INTERVAL_SEC;
}

void loop() {
  if (consumeButtonWakeRequest()) {
    handleButtonWakeRequest();
    return;
  }

  const uint32_t now = nowSeconds();
  bool didWork = false;
  bool haveFreshSpdSample = false;
  uint8_t freshSpdSample = lastSpdStatus;

  if ((int32_t)(now - nextStatusSampleSec) >= 0) {
    const uint8_t spd = readSpdStatusBit();
    const bool failTransition = (lastSpdStatus == 1) && (spd == 0);

    lastSpdStatus = spd;
    freshSpdSample = spd;
    haveFreshSpdSample = true;
    nextStatusSampleSec = now + SPD_SAMPLE_INTERVAL_SEC;
    didWork = true;

    if (failTransition) {
      buildAndSendCurrentBeacon(spd, true);
      nextTxSec = nowSeconds() + pickNextIntervalSec();
    }
  }

  if ((int32_t)(now - nextTxSec) >= 0) {
    // If the periodic status sample was just performed in this same loop pass,
    // reuse it instead of powering the SPD status divider a second time.
    const uint8_t spd = haveFreshSpdSample ? freshSpdSample : readSpdStatusBit();
    lastSpdStatus = spd;

    buildAndSendCurrentBeacon(spd, false);
    const uint32_t nowAfterTx = nowSeconds();
    nextTxSec = nowAfterTx + pickNextIntervalSec();
    nextStatusSampleSec = nowAfterTx + SPD_SAMPLE_INTERVAL_SEC;
    didWork = true;
  }

  if ((int32_t)(now - nextLedBlinkSec) >= 0) {
    blinkStatusLeds();
    nextLedBlinkSec = nowSeconds() + LED_BLINK_INTERVAL_SEC;
    didWork = true;
  }

  // If more than one event became due while active, loop again without sleeping.
  // Otherwise enter AVR power-down until the RTC PIT wakes us.
  if (!didWork) {
    rtcTick = false;
    enterPowerDownSleep();
  }
}
