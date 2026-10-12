#!/usr/bin/env python3
"""Exercise actual AC/DC scheduling and sketch helpers with offline I/O shims.

This does not open USB. Native simulations check control flow; they cannot
measure current or qualify AVR asynchronous registers and wake-up hardware.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "firmware/beacons/acdc_avr128da32/acdc_avr128da32.ino"


def function(source, name):
    """Copy a definition, including preprocessor branches, without rewriting it."""
    match = re.search(r"(?m)^(?:static\s+)?(?:inline\s+)?[^;\n]+\b" +
                      re.escape(name) + r"\s*\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError("Missing actual sketch function: " + name)
    # Blank comments and strings solely for brace counting; return original text.
    clean = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"',
                   lambda found: " " * len(found.group()), source)
    depth = 1
    pos = match.end()
    while depth:
        if clean[pos] == "{":
            depth += 1
        elif clean[pos] == "}":
            depth -= 1
        pos += 1
    return source[match.start():pos]


PRELUDE = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "sleep_schedule.h"
static unsigned checks = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) throw std::runtime_error( \
  std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #expr); } while (0)
static std::vector<std::string> events;
static std::array<int, 40> pinLevel, pinModes;
enum { LOW=0, HIGH=1, INPUT=2, OUTPUT=3, INPUT_PULLUP=4,
       LED_PWR=1, LED_TX=2, LED_SPD=3, LORA_NSS=4, LORA_RST=5,
       LORA_SCK=6, LORA_MOSI=7, LORA_MISO=8, LORA_RFSW=9,
       LORA_DIO1=10, PIN_SPD_STATUS=11, SLEEP_MODE_STANDBY=12,
       RTC_CMP_bm=1, RTC_OVF_bm=2, RTC_CMPBUSY_bm=1 };
static uint8_t SREG = 0x80;
static bool spdWakeRequested=false, spdFailRequested=false,
            rtcWakeRequested=false, pendingFailEvent=false,
            radioInitialized=false, statusPulseActive=false;
static uint16_t nextRadioRetryTick=0, statusPulseEndTick=0;
static uint8_t lastSpdStatus=1;
static bool haveLastGoodTemp=false;
static int8_t lastGoodTempC=25;
static uint32_t prng_state=0x12345678;
static unsigned fakeMillis=0, dioReads=0, dioDoneAfter=0;
static bool neverDio=false;
static void digitalWrite(int pin,int value) {
  pinLevel.at(pin)=value;
  events.push_back("write:"+std::to_string(pin)+":"+std::to_string(value));
}
static int digitalRead(int pin) {
  if (pin==LORA_DIO1) return !neverDio && dioReads++ >= dioDoneAfter ? HIGH:LOW;
  return pinLevel.at(pin);
}
static void pinMode(int pin,int mode) {
  pinModes.at(pin)=mode;
  events.push_back("mode:"+std::to_string(pin)+":"+std::to_string(mode));
}
static void ledOn(uint8_t pin) { digitalWrite(pin,LOW); }
static void ledOff(uint8_t pin) { digitalWrite(pin,HIGH); }
static uint32_t millis() { return fakeMillis++; }
static void delay(unsigned ms) { fakeMillis+=ms; events.push_back("delay:"+std::to_string(ms)); }
static void cli() { SREG=0; events.push_back("cli"); }
static void sei() { SREG=0x80; events.push_back("sei"); }
static bool injectedContact=false, injectedDeadline=false;
struct CompareRegister {
  uint16_t value=0;
  CompareRegister& operator=(uint16_t);
  operator uint16_t() const { return value; }
};
struct FakeRtc {
  uint16_t CNT=0;
  uint8_t STATUS=0, INTCTRL=0, INTFLAGS=0;
  uint8_t CTRLA=0, PITINTCTRL=0, PITSTATUS=0, PITCTRLA=0, CLKSEL=0;
  uint16_t PER=0;
  CompareRegister CMP;
} RTC;
enum { TCB_RUNSTDBY_bm=0x40, RTC_CLKSEL_OSC1K_gc=1,
       RTC_RUNSTDBY_bm=0x80, RTC_PRESCALER_DIV32_gc=0x28, RTC_RTCEN_bm=1,
       BOD_SLEEP_gm=3, BOD_SLEEP_SAMPLED_gc=2, BOD_SAMPFREQ_bm=0x10,
       PORT_ISC_INPUT_DISABLE_gc=4 };
struct ControlRegister { uint8_t CTRLA=0; } TCB2, BOD;
#define _PROTECTED_WRITE(reg,value) do { reg=(value); } while (0)
struct PortRegisters {
  uint8_t PIN0CTRL=0, PIN1CTRL=0, PIN2CTRL=0, PIN3CTRL=0,
          PIN4CTRL=0, PIN5CTRL=0, PIN6CTRL=0, PIN7CTRL=0;
} PORTA, PORTD, PORTF;
struct VirtualPortRegisters { uint8_t IN=0; } VPORTD;
constexpr uint8_t PIN7_bm=0x80;
CompareRegister& CompareRegister::operator=(uint16_t next) {
  value=next;
  events.push_back("cmp-write");
  if (injectedContact) spdWakeRequested=true;
  if (injectedDeadline) RTC.CNT=next;
  return *this;
}
static bool enabledSleep=false;
static unsigned sleeps=0;
static void set_sleep_mode(int mode) { CHECK(mode==SLEEP_MODE_STANDBY); events.push_back("sleep-mode"); }
static void sleep_enable() { enabledSleep=true; events.push_back("sleep-enable"); }
static void sleep_disable() { enabledSleep=false; events.push_back("sleep-disable"); }
static void sleep_cpu() {
  CHECK(enabledSleep);
  CHECK(events.back()=="sei");
  CHECK(RTC.INTCTRL==RTC_CMP_bm);
  ++sleeps;
  events.push_back("sleep-cpu");
}
struct FakeSpi {
  bool active=false;
  void begin() { active=true; events.push_back("spi-begin"); }
  void end() { active=false; events.push_back("spi-end"); }
} SPI;
enum { RADIOLIB_ERR_NONE=0, RADIOLIB_ERR_TX_TIMEOUT=-5 };
struct FakeRadio {
  int16_t standbyResult=0, startResult=0, finishResult=0, sleepResult=0;
  unsigned airUs=100000;
  bool sleeping=true;
  std::vector<std::vector<uint8_t>> frames;
  int16_t standby() {
    CHECK(SPI.active); CHECK(pinModes[LORA_MISO]==INPUT);
    events.push_back("radio-standby"); sleeping=false; return standbyResult;
  }
  int16_t startTransmit(const uint8_t* pkt,size_t len) {
    CHECK(SPI.active); CHECK(!sleeping); events.push_back("radio-tx");
    frames.emplace_back(pkt,pkt+len); return startResult;
  }
  unsigned getTimeOnAir(size_t) { return airUs; }
  int16_t finishTransmit() { CHECK(SPI.active); events.push_back("radio-finish"); return finishResult; }
  int16_t sleep(bool retain) {
    CHECK(retain); CHECK(SPI.active); events.push_back("radio-sleep-retain");
    sleeping=true; return sleepResult;
  }
} radio;
static AcDcSleep::Schedule txSchedule;
static uint8_t failRepeatPacket[20];
static uint64_t nonceNext=1;
static unsigned nonceCalls=0, adcConfigurations=0, adcConversions=0, initCalls=0;
static int conversionFailure=-1;
static bool initializationSucceeds=true;
struct FakeAdc { uint8_t CTRLA=0; } fakeAdc;
#define ADC0 fakeAdc
static void configureThermoAdc() { ++adcConfigurations; ADC0.CTRLA=1; }
static bool readAdc0Ain0Single10(uint16_t& raw) {
  CHECK(ADC0.CTRLA!=0);
  const int conversion=static_cast<int>(adcConversions++);
  if (conversion==conversionFailure) return false;
  raw=512; return true;
}
static uint64_t allocateNonce() { ++nonceCalls; return nonceNext++; }
static bool loraInit() {
  ++initCalls; events.push_back("radio-init"); SPI.begin();
  radioInitialized=initializationSucceeds; radio.sleeping=false;
  return radioInitialized;
}
// The packet encoder is copied from the sketch. Hashing is deterministic here;
// the separate packet-compatibility suite validates the real authentication.
struct Sha256Context { std::vector<uint8_t> data; };
static void sha256_init(Sha256Context* ctx) { ctx->data.clear(); }
static void sha256_update(Sha256Context* ctx,const uint8_t* data,size_t count) {
  ctx->data.insert(ctx->data.end(),data,data+count);
}
static void sha256_final(Sha256Context* ctx,uint8_t* out) {
  for (unsigned i=0;i<32;++i) out[i]=ctx->data[i%ctx->data.size()] ^ static_cast<uint8_t>(i*7);
}
static bool radioOk(int16_t);
static uint16_t nowTicks();
static void radioSleepRetain();
static void signalStatus(uint8_t);
static void finishStatusPulse(uint16_t);
static uint8_t readSpdStatusBit();
static uint8_t readCellVoltageByte();
static uint8_t readTempC_int8(int8_t&,uint16_t&,int16_t&);
static int8_t chooseTempToTransmit(uint8_t,int8_t);
static bool transmitPacket(const uint8_t*,size_t);
static void sendBeaconEvent(uint8_t,bool);
static void sleepUntilWork();
'''

TESTS = r'''
static size_t eventPosition(const std::string& value) {
  auto found=std::find(events.begin(),events.end(),value);
  CHECK(found!=events.end()); return static_cast<size_t>(found-events.begin());
}
static unsigned eventCount(const std::string& value) {
  return static_cast<unsigned>(std::count(events.begin(),events.end(),value));
}
static uint64_t frameNonce(const std::vector<uint8_t>& frame) {
  uint64_t nonce=0;
  CHECK(frame.size()==BEACON_PACKET_LEN);
  for (unsigned i=0;i<8;++i) nonce |= uint64_t(frame.at(3+i)) << (8*i);
  return nonce;
}
static void resetIo(uint16_t now=0) {
  events.clear(); pinLevel.fill(HIGH); pinModes.fill(INPUT);
  RTC.CNT=now; RTC.STATUS=RTC.INTCTRL=RTC.INTFLAGS=0;
  VPORTD.IN=0;
  injectedContact=injectedDeadline=false;
  spdWakeRequested=spdFailRequested=rtcWakeRequested=pendingFailEvent=false;
  radioInitialized=true; statusPulseActive=false; lastSpdStatus=1;
  nextRadioRetryTick=statusPulseEndTick=0;
  fakeMillis=dioReads=dioDoneAfter=0; neverDio=false;
  enabledSleep=false; sleeps=0; SPI.active=false; radio=FakeRadio();
  nonceNext=1; nonceCalls=adcConfigurations=adcConversions=initCalls=0;
  conversionFailure=-1; initializationSucceeds=true; ADC0.CTRLA=0;
  pinLevel[PIN_SPD_STATUS]=LOW;
  txSchedule.begin(now,160);
}
static void testScheduler() {
  CHECK(secondsToTicksRounded(START_DELAY_S)==160);
  CHECK(secondsToTicksRounded(3)==96);
  CHECK(secondsToTicksRounded(20)==640);
  CHECK(secondsToTicksRounded(40)==1280);
  CHECK(secondsToTicksRounded(300)==9600);
  CHECK(secondsToTicksRounded(600)==19200);
  CHECK(secondsToTicksRounded(-1)==0);
  CHECK(secondsToTicksRounded(0)==0);
  CHECK(secondsToTicksRounded(5000)==AcDcSleep::MAX_DELAY_TICKS);
  CHECK(secondsToTicksRounded(1.015625f)==33);
  for (unsigned i=0;i<10000;++i) {
    const uint16_t interval=pickNextIntervalTicks();
    CHECK(interval>=secondsToTicksRounded(TX_INTERVAL_SEC_MIN));
    CHECK(interval<=secondsToTicksRounded(TX_INTERVAL_SEC_MAX));
  }
  for (unsigned start=0;start<65536;start+=97) {
    for (uint16_t delayTicks: {uint16_t(1),uint16_t(160),uint16_t(640),
                              uint16_t(1280),uint16_t(9600),uint16_t(19200)}) {
      AcDcSleep::Schedule schedule;
      schedule.begin(static_cast<uint16_t>(start),delayTicks);
      CHECK(!schedule.regularDue(static_cast<uint16_t>(start)));
      CHECK(!schedule.regularDue(static_cast<uint16_t>(start+delayTicks-1)));
      CHECK(schedule.regularDue(static_cast<uint16_t>(start+delayTicks)));
      CHECK(schedule.nextDeadline(static_cast<uint16_t>(start))==
            static_cast<uint16_t>(start+delayTicks));
    }
  }
  AcDcSleep::Schedule schedule;
  schedule.begin(65530,160);
  CHECK(schedule.regularDeadline()==154);
  CHECK(!schedule.regularDue(153)); CHECK(schedule.regularDue(154));
  schedule.regularSent(154,19200);
  schedule.beginFailRetries(160,2,96);
  CHECK(schedule.nextDeadline(160)==256);
  CHECK(!schedule.retryDue(255)); CHECK(schedule.retryDue(256));
  schedule.retrySent(256,96);
  CHECK(schedule.remainingRetries()==1 && schedule.retryDeadline()==352);
  schedule.retrySent(352,96);
  CHECK(schedule.remainingRetries()==0 && !schedule.retryDue(1000));
  schedule.retrySent(1000,96); CHECK(schedule.remainingRetries()==0);
  schedule.regularSent(1000,100);
  schedule.beginFailRetries(1050,2,96);
  CHECK(schedule.nextDeadline(1050)==1100);
  schedule.beginFailRetries(1080,2,96); // A new FAIL replaces an older burst.
  CHECK(schedule.remainingRetries()==2 && schedule.retryDeadline()==1176);
  CHECK(schedule.nextDeadline(1100)==1100); // Already due returns now.
  schedule.observe(1100);
  CHECK(schedule.regularDue(44000)); // Latched beyond the signed half range.
  schedule.observe(1176); CHECK(schedule.retryDue(44000));
  schedule.regularSent(44000,19200); schedule.retrySent(44000,96);
  CHECK(!schedule.regularDue(44000) && !schedule.retryDue(44000));
  CHECK(AcDcSleep::earlier(65530,154,10)==10);
  CHECK(AcDcSleep::earlier(100,90,200)==90);
  CHECK(AcDcSleep::earlier(100,200,90)==90);
}
static void testRadio() {
  resetIo(500);
  std::array<uint8_t,20> frame{};
  CHECK(transmitPacket(frame.data(),frame.size()));
  CHECK(eventPosition("spi-begin")<eventPosition("radio-standby"));
  CHECK(eventPosition("radio-standby")<eventPosition("radio-tx"));
  CHECK(eventPosition("radio-tx")<eventPosition("radio-finish"));
  CHECK(eventPosition("radio-finish")<eventPosition("radio-sleep-retain"));
  CHECK(eventPosition("radio-sleep-retain")<eventPosition("spi-end"));
  CHECK(radio.sleeping && !SPI.active && radioInitialized);
  CHECK(pinLevel[LORA_NSS]==HIGH && pinLevel[LORA_SCK]==LOW);
  CHECK(pinLevel[LORA_MOSI]==LOW && pinLevel[LORA_RFSW]==LOW);
  CHECK(pinModes[LORA_MISO]==INPUT_PULLUP && pinLevel[LED_TX]==HIGH);
  CHECK(adcConfigurations==0 && nonceCalls==0);
  for (unsigned failure=0;failure<5;++failure) {
    resetIo(65530);
    if (failure==0) radio.standbyResult=-1;
    if (failure==1) radio.startResult=-1;
    if (failure==2) radio.finishResult=-1;
    if (failure==3) radio.sleepResult=-1;
    if (failure==4) { neverDio=true; radio.airUs=1000; }
    CHECK(!transmitPacket(frame.data(),frame.size()));
    CHECK(!radioInitialized && !SPI.active);
    CHECK(pinLevel[LORA_RST]==LOW);
    CHECK(nextRadioRetryTick==26); // 1 second, crossing the 16-bit wrap.
    CHECK(fakeMillis<1000); // A stuck DIO1 is bounded.
    CHECK(adcConfigurations==0 && ADC0.CTRLA==0);
  }
  resetIo(); SPI.begin(); radioInitialized=false; radioSleepRetain();
  CHECK(eventCount("radio-sleep-retain")==0 && !SPI.active);
  CHECK(pinLevel[LORA_RST]==LOW && nextRadioRetryTick==32);
}
static void testAdcCleanup() {
  resetIo(); uint16_t raw=0; int16_t error=999;
  CHECK(readThermoRaw10(raw,error));
  CHECK(raw==512 && error==0 && adcConversions==9 && ADC0.CTRLA==0);
  CHECK(adcConfigurations==1);
  // Fail the discarded conversion and every retained conversion in turn.
  for (int failure=0;failure<9;++failure) {
    resetIo(); conversionFailure=failure;
    CHECK(!readThermoRaw10(raw,error));
    CHECK(error==-30000 && ADC0.CTRLA==0);
    CHECK(adcConversions==static_cast<unsigned>(failure+1));
  }
}
static void testLoopAndFailBurst() {
  resetIo(0);
  loop(); CHECK(radio.frames.empty() && adcConfigurations==0 && nonceCalls==0);
  RTC.CNT=159; loop(); CHECK(radio.frames.empty() && adcConfigurations==0);
  RTC.CNT=160; loop();
  CHECK(radio.frames.size()==1 && frameNonce(radio.frames[0])==1);
  CHECK(adcConfigurations==1 && ADC0.CTRLA==0 && nonceCalls==1);
  const uint16_t regular=txSchedule.regularDeadline();
  CHECK(static_cast<uint16_t>(regular-160)>=secondsToTicksRounded(TX_INTERVAL_SEC_MIN));
  CHECK(static_cast<uint16_t>(regular-160)<=secondsToTicksRounded(TX_INTERVAL_SEC_MAX));
  // Opening the contact wakes and transmits immediately, before the next report.
  RTC.CNT=200; pinLevel[PIN_SPD_STATUS]=HIGH; VPORTD.IN=PIN7_bm;
  CHECK(RTC.INTCTRL==RTC_CMP_bm); // The long timer was still armed.
  spdWakeIsr(); CHECK(spdWakeRequested && spdFailRequested); loop();
  CHECK(radio.frames.size()==2 && frameNonce(radio.frames[1])==2);
  CHECK((radio.frames[1][1]&1)==0 && txSchedule.remainingRetries()==2);
  const auto failFrame=radio.frames[1];
  CHECK(adcConfigurations==2 && nonceCalls==2);
  RTC.CNT=295; loop(); CHECK(radio.frames.size()==2);
  // Closing a contact updates status but the repeat keeps the original FAIL.
  RTC.CNT=296; pinLevel[PIN_SPD_STATUS]=LOW; VPORTD.IN=0;
  spdWakeIsr(); CHECK(spdWakeRequested && !spdFailRequested); loop();
  CHECK(radio.frames.size()==3 && radio.frames[2]==failFrame);
  CHECK(txSchedule.remainingRetries()==1);
  CHECK(adcConfigurations==2 && nonceCalls==2 && ADC0.CTRLA==0);
  RTC.CNT=392; loop();
  CHECK(radio.frames.size()==4 && radio.frames[3]==failFrame);
  CHECK(txSchedule.remainingRetries()==0 && adcConfigurations==2 && nonceCalls==2);
  RTC.CNT=488; loop(); CHECK(radio.frames.size()==4);
  // A complete open/close pulse captured by the ISR is still a FAIL event.
  RTC.CNT=500; VPORTD.IN=PIN7_bm; spdWakeIsr();
  VPORTD.IN=0; spdWakeIsr(); loop();
  CHECK(radio.frames.size()==5 && (radio.frames.back()[1]&1)==0);
  CHECK(frameNonce(radio.frames.back())==3 && txSchedule.remainingRetries()==2);
  // A later opening supersedes the cached burst with a new nonce/frame.
  RTC.CNT=510; pinLevel[PIN_SPD_STATUS]=HIGH;
  VPORTD.IN=PIN7_bm; spdWakeIsr(); loop();
  CHECK(frameNonce(radio.frames.back())==4 && txSchedule.remainingRetries()==2);
  CHECK(txSchedule.retryDeadline()==606);
  const auto newFailFrame=radio.frames.back();
  RTC.CNT=606; loop(); CHECK(radio.frames.back()==newFailFrame);
  CHECK(nonceCalls==4 && adcConfigurations==4);
}
static void testInitializationStall() {
  resetIo(); radioInitialized=false; initializationSucceeds=false;
  nextRadioRetryTick=32;
  RTC.CNT=31; loop(); CHECK(initCalls==0 && adcConfigurations==0);
  RTC.CNT=32; loop(); CHECK(initCalls==1 && !radioInitialized);
  CHECK(nextRadioRetryTick==64 && ADC0.CTRLA==0);
  // Clock may wrap repeatedly while the radio remains unavailable. Observing
  // once per second latches the regular deadline; no ADC/nonce work is done.
  for (unsigned second=2;second<=1400;++second) {
    RTC.CNT=static_cast<uint16_t>(second*32); loop();
  }
  CHECK(txSchedule.regularDue(RTC.CNT));
  CHECK(radio.frames.empty() && nonceCalls==0 && adcConfigurations==0);
  initializationSucceeds=true; RTC.CNT=static_cast<uint16_t>(1401*32); loop();
  CHECK(radioInitialized && radio.frames.size()==1);
  CHECK(frameNonce(radio.frames.back())==1 && nonceCalls==1 && ADC0.CTRLA==0);
}
static void testSleepAndLedModes() {
  resetIo(0); sleepUntilWork();
  CHECK(sleeps==1 && !enabledSleep && RTC.CMP==160);
  CHECK(RTC.INTCTRL==RTC_CMP_bm); // No periodic overflow interrupt.
  resetIo(0); txSchedule.beginFailRetries(0,2,96); sleepUntilWork(); CHECK(RTC.CMP==96);
  resetIo(0); radioInitialized=false; nextRadioRetryTick=32;
  sleepUntilWork(); CHECK(RTC.CMP==32);
  resetIo(0); statusPulseActive=true; statusPulseEndTick=2;
  sleepUntilWork(); CHECK(RTC.CMP==2);
  resetIo(0); RTC.CNT=160; sleepUntilWork(); CHECK(sleeps==0);
  resetIo(0); spdWakeRequested=true; sleepUntilWork(); CHECK(sleeps==0);
  resetIo(0); pendingFailEvent=true; sleepUntilWork(); CHECK(sleeps==0);
  // Simulate a contact or deadline arriving while asynchronous CMP synchronizes.
  resetIo(0); injectedContact=true; sleepUntilWork(); CHECK(sleeps==0);
  resetIo(0); injectedDeadline=true; sleepUntilWork(); CHECK(sleeps==0);
  resetIo(0); signalStatus(1);
#if WSPD_LED_MODE==0
  CHECK(pinLevel[LED_PWR]==LOW && pinLevel[LED_SPD]==HIGH && !statusPulseActive);
  signalStatus(0); CHECK(pinLevel[LED_SPD]==LOW);
#elif WSPD_LED_MODE==1
  CHECK(pinLevel[LED_PWR]==LOW && pinLevel[LED_SPD]==HIGH && statusPulseActive);
  CHECK(RTC.CMP==2 && RTC.INTCTRL==RTC_CMP_bm);
  finishStatusPulse(1); CHECK(statusPulseActive);
  // Execute the actual RTC ISR while foreground work could be committing EEPROM.
  RTC.CNT=2; rtcCounterIsr();
  CHECK(!statusPulseActive && pinLevel[LED_PWR]==HIGH && RTC.INTCTRL==0);
  RTC.CNT=65535; signalStatus(0); CHECK(statusPulseEndTick==1);
  finishStatusPulse(0); CHECK(statusPulseActive);
  finishStatusPulse(1); CHECK(!statusPulseActive && pinLevel[LED_SPD]==HIGH);
#else
  CHECK(pinLevel[LED_PWR]==HIGH && pinLevel[LED_SPD]==HIGH && !statusPulseActive);
  signalStatus(0); CHECK(pinLevel[LED_SPD]==HIGH);
#endif
  resetIo(0); std::array<uint8_t,20> frame{};
  dioDoneAfter=100; radio.airUs=100000;
  CHECK(transmitPacket(frame.data(),frame.size()));
#if WSPD_LED_MODE==0
  CHECK(eventCount("delay:15")==1);
#elif WSPD_LED_MODE==1
  CHECK(eventCount("delay:15")==0);
  CHECK(eventPosition("write:2:1")<eventPosition("radio-finish"));
#else
  CHECK(eventCount("write:2:0")==0 && eventCount("delay:15")==0);
#endif
}
static void testClockAndInputGating() {
  resetIo(); TCB2.CTRLA=0xff; BOD.CTRLA=0xa5;
  setupSleepClock();
  CHECK(TCB2.CTRLA==static_cast<uint8_t>(0xff & ~TCB_RUNSTDBY_bm));
  CHECK((BOD.CTRLA & ~BOD_SLEEP_gm & ~BOD_SAMPFREQ_bm)==
        (0xa5 & ~BOD_SLEEP_gm & ~BOD_SAMPFREQ_bm));
  CHECK((BOD.CTRLA & BOD_SLEEP_gm)==BOD_SLEEP_SAMPLED_gc);
  CHECK((BOD.CTRLA & BOD_SAMPFREQ_bm)!=0);
  CHECK(RTC.PITINTCTRL==0 && RTC.PITCTRLA==0 && RTC.INTCTRL==0);
  CHECK(RTC.CLKSEL==RTC_CLKSEL_OSC1K_gc && RTC.PER==0xffff && RTC.CNT==0);
  CHECK(RTC.CTRLA==(RTC_RUNSTDBY_bm|RTC_PRESCALER_DIV32_gc|RTC_RTCEN_bm));
  PORTA=PortRegisters(); PORTD=PortRegisters(); PORTF=PortRegisters();
  disableUnusedInputBuffers();
  for (uint8_t value : {PORTA.PIN0CTRL,PORTA.PIN2CTRL,PORTA.PIN3CTRL,
                       PORTD.PIN1CTRL,PORTD.PIN2CTRL,PORTD.PIN3CTRL,
                       PORTD.PIN4CTRL,PORTD.PIN5CTRL,PORTD.PIN6CTRL,
                       PORTF.PIN0CTRL,PORTF.PIN1CTRL,PORTF.PIN2CTRL,PORTF.PIN3CTRL}) {
    CHECK(value==PORT_ISC_INPUT_DISABLE_gc);
  }
  for (uint8_t used : {PORTA.PIN1CTRL,PORTA.PIN4CTRL,PORTA.PIN5CTRL,
                      PORTA.PIN6CTRL,PORTA.PIN7CTRL,PORTD.PIN0CTRL,
                      PORTD.PIN7CTRL,PORTF.PIN4CTRL,PORTF.PIN5CTRL,PORTF.PIN6CTRL}) {
    CHECK(used==0);
  }
}
int main() {
  try {
    testScheduler(); testRadio(); testAdcCleanup(); testLoopAndFailBurst();
    testInitializationStall(); testSleepAndLedModes(); testClockAndInputGating();
    std::cout << "PASS: " << checks << " native checks; LED=" << WSPD_LED_MODE
              << ", preproduction=" << WSPD_PREPRODUCTION << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
'''


def inspect_hardware_paths(source):
    """Verify hardware choices; this is source inspection, not hardware emulation."""
    clock = function(source, "setupSleepClock")
    for statement in ("RTC.PITINTCTRL = 0", "RTC.PITCTRLA = 0",
                      "RTC.CLKSEL = RTC_CLKSEL_OSC1K_gc", "RTC.PER = 0xFFFF",
                      "RTC_RUNSTDBY_bm | RTC_PRESCALER_DIV32_gc | RTC_RTCEN_bm",
                      "BOD_SLEEP_SAMPLED_gc | BOD_SAMPFREQ_bm",
                      "TCB2.CTRLA &= (uint8_t)~TCB_RUNSTDBY_bm"):
        if statement not in clock:
            raise AssertionError("Missing RTC/low-power hardware choice: " + statement)
    sleep = function(source, "sleepUntilWork")
    if not re.search(r"sei\(\);\s*sleep_cpu\(\);", sleep):
        raise AssertionError("SEI must immediately precede the sleep instruction.")
    if "SLEEP_MODE_STANDBY" not in sleep:
        raise AssertionError("RTC CNT deadline needs STANDBY.")
    if sleep.index("RTC.INTFLAGS =") > sleep.index("RTC.CMP ="):
        raise AssertionError("Clearing the compare flag after arming loses an IRQ.")
    if sleep.count("RTC.INTFLAGS =") != 1:
        raise AssertionError("Do not clear a newly armed RTC deadline flag.")
    if "VPORTD.IN & PIN7_bm" not in function(source, "spdWakeIsr"):
        raise AssertionError("Contact ISR no longer latches an opening on PD7.")
    if "spdFailRequested = true" not in function(source, "spdWakeIsr"):
        raise AssertionError("Contact IRQ no longer records short FAIL edges.")
    setup = function(source, "setup")
    if "attachInterrupt(digitalPinToInterrupt(PIN_SPD_STATUS), spdWakeIsr, CHANGE)" not in setup:
        raise AssertionError("Contact transitions must wake the MCU.")
    adc = function(source, "configureThermoAdc")
    if "PORT_ISC_INPUT_DISABLE_gc" not in adc:
        raise AssertionError("Analog input digital buffer is not disabled.")


def harness(source):
    settings = source[source.index("// First regular transmission"):
                      source.index("// LoRa RF settings")]
    thermo = source[source.index("// ---------------------- THERMISTOR"):
                    source.index("// ---------------------- SIMPLE PRNG")]
    # Native shims use symbolic pin names; these packet constants are copied.
    packet = "\n".join(re.findall(r"(?m)^constexpr (?:uint8_t|size_t)\s+(?:BEACON_\w+|CELL_VOLTAGE_UNAVAILABLE)\s*=.*;", source))
    names = ["xorshift32", "secondsToTicksRounded", "randomFloat01", "pickNextIntervalTicks",
             "nowTicks", "setupSleepClock", "disableUnusedInputBuffers",
             "signalStatus", "finishStatusPulse", "rtcCounterIsr", "spdWakeIsr", "sleepUntilWork",
             "packUint64LE", "buildBeaconPacket", "radioOk", "radioPinsIdle",
             "radioSleepRetain", "transmitPacket", "sendBeaconEvent",
             "readSpdStatusBit", "readCellVoltageByte", "readThermoRaw10",
             "convertRaw10ToTempC", "readTempC_int8", "chooseTempToTransmit", "loop"]
    # Only change the ISR declaration for a native compiler; its body is actual.
    native_source = source.replace("ISR(RTC_CNT_vect)", "static void rtcCounterIsr()")
    return (PRELUDE + settings + thermo + packet +
            "\nconst uint32_t SECRET=0x12345678UL; const uint8_t SPD_ID=10;\n" +
            "\n\n".join(function(native_source, name) for name in names) + TESTS)


def main():
    source = SKETCH.read_text()
    inspect_hardware_paths(source)
    compiler = shutil.which("c++")
    if not compiler:
        raise SystemExit("A host C++ compiler is required.")
    with tempfile.TemporaryDirectory(prefix="wspd-acdc-sleep-") as temp:
        cpp = Path(temp) / "test_acdc_sleep.cpp"
        cpp.write_text(harness(source))
        for led_mode in (0, 1, 2):
            for preproduction in (0, 1):
                binary = Path(temp) / f"test_acdc_sleep_{led_mode}_{preproduction}"
                subprocess.run([compiler, "-std=c++11", "-O1", "-Wall", "-Wextra",
                                "-Wno-unused-function", "-Wno-unused-const-variable",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                                f"-DWSPD_LED_MODE={led_mode}",
                                f"-DWSPD_PREPRODUCTION={preproduction}",
                                "-I", str(SKETCH.parent), str(cpp), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
    print("PASS: actual AC/DC scheduler and sketch helper control flow, all six LED/timing modes.")
    print("Source-inspected: one-shot RTC STANDBY, disabled PIT, contact CHANGE IRQ, atomic sleep entry.")
    print("Offline shims cannot qualify physical wake races, SX1262 sleep current, oscillator drift or brownouts.")


if __name__ == "__main__":
    main()
