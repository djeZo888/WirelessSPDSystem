#pragma once

// Copy this file to config.h. That local file is ignored by Git.
// Set a private random uint32 shared with the receiver and this site's beacons.
// Zero deliberately fails compilation. Never publish config.h or built firmware.
#define WSPD_SHARED_SECRET 0x00000000UL

// Unique within the receiver's site. 1..9 are reserved for preproduction.
#define WSPD_BEACON_ID 10

// Used only when all 512 EEPROM bytes are erased. On a previously deployed ID,
// set this above the receiver's last nonce before the first persistent build.
// Existing journals override this setting; preserve EEPROM on every reflash.
#define WSPD_NONCE_START 1ULL

// All beacons and the receiver must share channel and spreading factor.
// Channel 1 = 865.3 MHz. Keep RF settings within your local legal limits.
#define WSPD_LORA_CHANNEL_ID 1
#define WSPD_LORA_TX_DBM 10
#define WSPD_LORA_SF 10
// SF10 is the interoperable example default for the T Connect Pro receiver.
