#pragma once

// Copy this file to config.h; the local file is ignored by Git.
// Never publish config.h or firmware containing this site's shared secret.

// ---------------------- PER SERVER / HOME ----------------------
// All beacons reporting to one receiver must use these same three values.

// Exactly 4 bytes (uint32_t), range 0x00000001..0xFFFFFFFF, inclusive.
// Write eight hexadecimal digits followed by UL, for example 0x01234567UL.
// Choose a private random value. The all-zero placeholder fails compilation.
#define WSPD_SHARED_SECRET 0x00000000UL

// Carrier frequency in MHz: 863.0625..869.9375, inclusive, finite number.
// This keeps the fixed 125 kHz channel inside the 863..870 MHz EU SRD band.
// Choose a locally permitted sub-band, TX power and duty cycle; the numeric
// bounds alone do not establish legal operation. The antenna/module must also
// support the selected frequency. Match the receiver exactly.
#define WSPD_LORA_FREQ_MHZ 865.3f

// Integer spreading factor: min 5, max 12, inclusive. Match the receiver.
// SF10 is the interoperable example default for the T Connect Pro receiver.
#define WSPD_LORA_SF 10

// ---------------------- PER SPD / BEACON ----------------------
// Integer ID: min 1, max 127, inclusive; 0 is reserved.
// Use 10..127 for production; 1..9 are reserved for preproduction.
// This ID must be unique among the beacons registered with this receiver.
#define WSPD_BEACON_ID 10

// Integer conducted TX power in dBm: min -9, max 22, inclusive (SX1262 range).
// Antenna gain and the selected sub-band can require a lower legal limit.
#define WSPD_LORA_TX_DBM 10

// Integer initial nonce: min 1, max 18446744073709551614 (UINT64_MAX-1).
// Use the ULL suffix. Used only when all 512 EEPROM bytes are erased.
// On a previously deployed ID, set this above the receiver's last nonce before
// the first persistent build. Existing journals override this setting;
// preserve EEPROM on every reflash. Blank EEPROM defaults to nonce 1.
#define WSPD_NONCE_START 1ULL

// Bandwidth, coding rate, sync word, preamble and radio CRC are fixed in the
// sketch. Hardware type and firmware version are also fixed by this build.
