// ============================================================
// BLUETOOTH AUDIO MODULE — PUBLIC API
// ============================================================
//
// The ESP32-CAM operates as an A2DP SOURCE.
// It sends PCM audio to a Bluetooth Classic speaker/TV.
//
// Device name (visible on TV/phone scan): "ESP-Mini-Player"
//
// AUTO-CONNECT BEHAVIOUR:
//   1. On boot, starts scanning automatically.
//   2. Preferred devices tried in priority order:
//        [1] boAt Stone 1400  [2] Sony WH-CH510  [3] Samsung TV
//   3. If none found → re-scan every 30 seconds.
//   4. On disconnect → wait 10s → reconnect or re-scan.
//
// SERIAL COMMANDS:
//   bt scan             - force immediate scan
//   bt stop             - stop current scan
//   bt list             - print discovered devices
//   bt connect N        - connect to discovered device index N
//   bt connect <name>   - connect to device by name substring
//   bt disconnect       - disconnect cleanly
//   bt status           - show full status
//   bt test             - play 440 Hz test tone
//   bt notone           - stop test tone
//   bt prefer list      - show preferred device list
//   bt prefer add <str> - add a name to preferred list (runtime)
//   bt prefer clear     - clear runtime additions
//
// ============================================================

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>


// ============================================================
// STATE ENUM
// ============================================================

enum class BluetoothState {
    OFF,            // Stack not initialized
    IDLE,           // Initialized, not scanning or connected
    SCANNING,       // GAP inquiry running
    CONNECTING,     // A2DP connection attempt in progress
    CONNECTED,      // A2DP connected (audio NOT yet streaming)
    STREAMING,      // A2DP connected + audio flowing
    DISCONNECTING,  // Clean disconnect in progress
    RECONNECTING,   // Waiting to reconnect after unexpected disconnect
};


// ============================================================
// LIFECYCLE
// ============================================================

// Initialize the BT stack and start auto-connect.
// Call once from setup() — auto-scan begins immediately.
void bluetoothAudioBegin();

// Shut down cleanly (disconnects if connected).
void bluetoothAudioEnd();


// ============================================================
// DISCOVERY
// ============================================================

// Manually trigger a scan (also happens automatically).
void bluetoothAudioStartScan();

// Stop an active scan.
void bluetoothAudioStopScan();

// Number of devices found in the last scan.
size_t bluetoothAudioDeviceCount();

// Device info by index (from last scan).
const char* bluetoothAudioDeviceName(size_t index);
const char* bluetoothAudioDeviceAddress(size_t index);
int8_t      bluetoothAudioDeviceRSSI(size_t index);

// Print all discovered devices to Serial.
void bluetoothAudioPrintDevices();


// ============================================================
// CONNECTION
// ============================================================

// Connect to a discovered device by index.
bool bluetoothAudioConnect(size_t index);

// Connect to a device by name substring (case-sensitive).
bool bluetoothAudioConnectByName(const char* name);

// Disconnect the current connection.
void bluetoothAudioDisconnect();

// Connection state queries.
bool           bluetoothAudioConnected();
bool           bluetoothAudioStreaming();
BluetoothState bluetoothAudioGetState();
const char*    bluetoothAudioGetStateName();

// Name of currently connected device (empty if not connected).
const char* bluetoothAudioConnectedDeviceName();


// ============================================================
// PREFERRED DEVICE LIST  (RUNTIME OVERRIDE)
// ============================================================

// Add a name to the runtime preferred list (survives until reboot).
// The name is matched as a substring of discovered device names.
bool bluetoothAudioPreferAdd(const char* name);

// Print the full preferred list (hardcoded + runtime additions).
void bluetoothAudioPreferPrint();

// Clear all runtime-added preferred names (keeps hardcoded defaults).
void bluetoothAudioPreferClear();


// ============================================================
// AUDIO PLAYBACK
// ============================================================

// Provide a pre-loaded PCM buffer in PSRAM to play.
// Data must be 8-bit unsigned mono at the specified sample rate.
void bluetoothAudioSetPCMBuffer(const uint8_t* pcm_data, size_t size_bytes, uint32_t sample_rate);

// Set playback position in milliseconds.
// Called continuously by the video player to keep audio perfectly synced!
void bluetoothAudioSetTime(uint32_t time_ms);

// Audio pause & resume controls
void bluetoothAudioPause();
void bluetoothAudioResume();
bool bluetoothAudioIsPaused();


// ============================================================
// MAIN LOOP HOOK
// ============================================================

// Call from Arduino loop() every iteration.
// Drives auto-connect, reconnect, scan retry, watchdog.
void bluetoothAudioUpdate();


// ============================================================
// SERIAL COMMAND HANDLER
// ============================================================

// Pass any line from Serial that starts with "bt".
// Returns true if command was recognized.
bool bluetoothAudioHandleCommand(const char* line);
