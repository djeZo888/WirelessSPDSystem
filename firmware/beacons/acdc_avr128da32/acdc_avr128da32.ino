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

// -----------------------------------------------------------------------------
// DEHN SPD FM wireless beacon - previous AVR128DA32 hardware + Wio-SX1262
// Payload v2 compatibility build: adds hardware/firmware byte and voltage byte.
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
const float TX_INTERVAL_SEC_MIN = 300.0f;
const float TX_INTERVAL_SEC_MAX = 600.0f;

// On transition OK -> FAIL, send one immediate FAIL beacon and then repeat the
// exact same payload this many additional times.
const uint8_t SPDFAIL_RETX_COUNT = 2;      // 0 = disabled
const float   SPDFAIL_RETX_DELAY = 3.0f;   // seconds between repeats

// LoRa RF settings
// Channel map:
//   0=865.1  1=865.3  2=865.5  3=865.7
//   4=865.9  5=866.1  6=866.3  7=866.5
const uint8_t LORA_CHANNEL_ID   = WSPD_LORA_CHANNEL_ID;
const int8_t  LORA_TX_DBM       = WSPD_LORA_TX_DBM;   // min -9, max 22
const uint8_t LORA_SF           = WSPD_LORA_SF;
const float   LORA_BW_KHZ       = 125.0f;
const uint8_t LORA_CR           = 5;    // coding rate 4/5 in RadioLib naming
const uint8_t LORA_SYNCWORD     = 0x12; // private network sync word
const uint16_t LORA_PREAMBLE    = 8;

// Wio-SX1262 uses an active TCXO powered from DIO3.
// The module datasheet allows 1.7 .. 3.3 V and says DIO3 should stay about
// 200 mV below VCC. 2.4 V is a conservative value at 3.3 V supply.
const float SX1262_TCXO_VOLTAGE = 2.4f;

// Seeed documents this module as DC-DC powered, not LDO-only.
const bool  SX1262_USE_LDO = false;

// RadioLib defaults to a fairly low current limit on SX126x.
const float SX1262_CURRENT_LIMIT_MA = 140.0f;

constexpr float LORA_CHANNEL_FREQS_MHZ[] = {
  865.1f, 865.3f, 865.5f, 865.7f,
  865.9f, 866.1f, 866.3f, 866.5f
};
constexpr size_t LORA_CHANNEL_COUNT = sizeof(LORA_CHANNEL_FREQS_MHZ) / sizeof(LORA_CHANNEL_FREQS_MHZ[0]);
static_assert(LORA_CHANNEL_ID < LORA_CHANNEL_COUNT, "Invalid LORA_CHANNEL_ID");

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
static void sendBeaconEvent(uint8_t spd, int8_t temp, bool burstFailRepeats);
static uint8_t readSpdStatusBit();
static uint8_t readCellVoltageByte();
static void configureThermoAdc();
static bool readThermoRaw10(uint16_t& rawOut, int16_t& adcErrOut);
static bool readAdc0Ain0Single10(uint16_t& rawOut);
static uint16_t normalizeAdcReadingTo10bit(uint16_t value);
static int8_t convertRaw10ToTempC(uint16_t raw);
static uint8_t readTempC_int8(int8_t& tempOut, uint16_t& rawOut, int16_t& adcErrOut);
static int8_t chooseTempToTransmit(uint8_t status, int8_t latestTemp);

static uint64_t nextNonce = 0;
static uint32_t nextTxMs = 0;
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

static uint32_t secondsToMsRounded(float seconds) {
  if (seconds <= 0.0f) return 0;
  float ms = seconds * 1000.0f;
  if (ms >= 4294967000.0f) return 0xFFFFFFFFUL;
  return (uint32_t)lroundf(ms);
}

static float randomFloat01() {
  // 24 useful random bits are enough for interval dithering.
  return (float)(xorshift32() & 0x00FFFFFFUL) / 16777215.0f;
}

static uint32_t pickNextIntervalMs() {
  float lo = TX_INTERVAL_SEC_MIN;
  float hi = TX_INTERVAL_SEC_MAX;
  if (hi < lo) {
    float tmp = hi;
    hi = lo;
    lo = tmp;
  }
  float seconds = (hi > lo) ? (lo + (hi - lo) * randomFloat01()) : lo;
  return secondsToMsRounded(seconds);
}

static uint64_t allocateNonce() {
  return nextNonce++;
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

static bool transmitPacket(const uint8_t* pkt, size_t len) {
  ledOn(LED_TX);
  delay(15);
  int16_t state = radio.transmit(pkt, len);
  ledOff(LED_TX);

  if (!radioOk(state)) {
    while (!loraInit()) {
      ledOn(LED_TX);
      delay(50);
      ledOff(LED_TX);
      delay(450);
    }
    ledOn(LED_PWR);
    return false;
  }
  return true;
}

static void sendBeaconEvent(uint8_t spd, int8_t temp, bool burstFailRepeats) {
  uint8_t pkt[BEACON_PACKET_LEN];
  uint64_t nonce = allocateNonce();
  const uint8_t cellVoltageByte = readCellVoltageByte();
  buildBeaconPacket(spd, temp, nonce, cellVoltageByte, pkt);

  transmitPacket(pkt, sizeof(pkt));

  if (burstFailRepeats && SPDFAIL_RETX_COUNT > 0) {
    const uint32_t delayMs = secondsToMsRounded(SPDFAIL_RETX_DELAY);
    for (uint8_t i = 0; i < SPDFAIL_RETX_COUNT; i++) {
      if (delayMs > 0) delay(delayMs);
      transmitPacket(pkt, sizeof(pkt));
    }
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
  // suitable prescaler constant.  At F_CPU=24 MHz, DIV32 gives about 750 kHz.
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
    return false;
  }

  uint32_t sum = 0;
  for (uint8_t i = 0; i < TEMP_ADC_SAMPLES; i++) {
    uint16_t v = 0;
    if (!readAdc0Ain0Single10(v)) {
      adcErrOut = -30000;
      return false;
    }
    sum += v;
    delay(2);
  }

  adcErrOut = 0;
  rawOut = (uint16_t)((sum + (TEMP_ADC_SAMPLES / 2)) / TEMP_ADC_SAMPLES);
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
  // Wio-SX1262 external RF_SW pin is HIGH only in RX mode.
  // Hold LOW until RadioLib takes ownership.
  pinMode(LORA_RFSW, OUTPUT);
  digitalWrite(LORA_RFSW, LOW);

  // No custom PORTMUX is needed: the PCB uses the AVR128DA32 default SPI0 pins.
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
  if (!radioOk(state)) return false;

  state = radio.explicitHeader();
  if (!radioOk(state)) return false;

  state = radio.setCRC(2);
  if (!radioOk(state)) return false;

  state = radio.setCurrentLimit(SX1262_CURRENT_LIMIT_MA);
  if (!radioOk(state)) return false;

  // Internal TX RF switch control on DIO2.
  state = radio.setDio2AsRfSwitch(true);
  if (!radioOk(state)) return false;

  // External module RF_SW pin: HIGH only in RX, LOW otherwise.
  radio.setRfSwitchPins(LORA_RFSW, RADIOLIB_NC);

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

  // Inputs
  pinMode(PIN_SPD_STATUS, INPUT);   // external pullup/front-end assumed present
  pinMode(PIN_THERMO, INPUT);

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

  // Init radio (retry forever, like the old sketch)
  while (!loraInit()) {
    ledOn(LED_TX);
    delay(50);
    ledOff(LED_TX);
    delay(450);
  }

  ledOn(LED_PWR);
  lastSpdStatus = readSpdStatusBit();
  nextTxMs = millis() + secondsToMsRounded(START_DELAY_S);
}

void loop() {
  uint8_t spd = readSpdStatusBit();

  int8_t tempMeasured = 0;
  uint16_t rawTemp = 0;
  int16_t adcErr = 0;
  uint8_t tempStatus = readTempC_int8(tempMeasured, rawTemp, adcErr);
  int8_t tempToSend = chooseTempToTransmit(tempStatus, tempMeasured);

  // Preserved LED meaning from the old sketch:
  // LED ON when SPD_STATUS == 0 (open / FAIL)
  if (spd == 0) ledOn(LED_SPD);
  else          ledOff(LED_SPD);

  const uint32_t now = millis();
  const bool failTransition = (lastSpdStatus == 1) && (spd == 0);

  if (failTransition) {
    sendBeaconEvent(spd, tempToSend, true);
    nextTxMs = now + pickNextIntervalMs();
    lastSpdStatus = spd;
    return;
  }

  if ((int32_t)(now - nextTxMs) >= 0) {
    sendBeaconEvent(spd, tempToSend, false);
    nextTxMs = now + pickNextIntervalMs();
  }

  lastSpdStatus = spd;
}
