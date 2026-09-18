// ============================================================
// BLUETOOTH AUDIO CONFIGURATION
// ============================================================
//
// This file is the single place to tune all Bluetooth
// behaviour for the ESP32-CAM A2DP Source player.
//
// ============================================================

#pragma once


// ============================================================
// ESP32 DEVICE IDENTITY
// ============================================================
//
// Name visible on other devices' Bluetooth scan screens.
// (e.g. Samsung TV will show "ESP-Mini-Player")
//
#define BT_LOCAL_NAME   "ESP-Mini-Player"


// ============================================================
// PREFERRED DEVICE LIST  (PRIORITY ORDER)
// ============================================================
//
// The system will connect to the first device from this list
// that is found during a scan.
//
// Priority 1 (index 0) = highest priority.
// Add / remove entries freely.
//
// Case-insensitive partial matching is NOT used.
// Names must match EXACTLY (substring match: name contains entry).
//
#define BT_PREFERRED_COUNT  3

// Exact Bluetooth device names to try in order.
// Substring match: if the discovered device name CONTAINS this
// string, it matches. E.g. "Stone 1400" matches "boAt Stone 1400".
#define BT_PREFERRED_1      "boAt Stone 1400"     // boAt Stone 1400
#define BT_PREFERRED_2      "WH-CH510"       // Sony WH-CH510
#define BT_PREFERRED_3      "Samsung"        // Samsung TV fallback


// ============================================================
// AUTO-CONNECT / SCAN TIMING
// ============================================================

// How long one GAP inquiry scan runs.
// 8 units x 1.28s = ~10.24 seconds.
#define BT_INQUIRY_DURATION_UNITS   8

// Maximum devices collected per scan.
#define BT_MAX_DISCOVERED_DEVICES   24

// After a scan completes with NO preferred device found,
// wait this many ms before starting the next scan.
#define BT_RESCAN_INTERVAL_MS       30000

// After an unexpected disconnect, wait this many ms
// before trying to reconnect / re-scan.
#define BT_RECONNECT_DELAY_MS       10000

// How many direct reconnect attempts to make before
// falling back to a fresh scan.
#define BT_RECONNECT_RETRIES        3


// ============================================================
// AUDIO / PCM FORMAT
// ============================================================
//
// ESP32-A2DP encodes PCM → SBC internally.
// These constants describe the PCM format your callback must provide.
//
// DO NOT CHANGE — this is what the BT SBC stack expects.
//
#define BT_PCM_SAMPLE_RATE      44100
#define BT_PCM_CHANNELS         2
#define BT_PCM_BITS             16
#define BT_PCM_BYTES_PER_FRAME  4   // 2x int16_t (Frame.channel1, Frame.channel2)


// ============================================================
// TEST TONE
// ============================================================

#define BT_TEST_TONE_HZ         440.0f   // A4 (concert A)
#define BT_TEST_TONE_AMPLITUDE  20000    // 0..32767  (~61% full scale)


// ============================================================
// RUNTIME PREFERRED LIST (Serial override)
// ============================================================

// Max extra entries the user can add at runtime via Serial.
#define BT_RUNTIME_PREFERRED_MAX    8


// ============================================================
// LOGGING PREFIX
// ============================================================

#define BT_TAG  "[BT] "
