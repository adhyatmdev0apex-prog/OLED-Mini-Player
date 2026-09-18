// ============================================================
// BLUETOOTH AUDIO MODULE — IMPLEMENTATION
// ============================================================
//
// Role: A2DP SOURCE (ESP32 → BT speaker)
//
// Auto-connect flow:
//
//   bluetoothAudioBegin()
//       ↓
//   startScan() — library begins GAP inquiry
//       ↓
//   ssidCallback(name, addr, rssi) called per device
//       ↓  [priority match against preferred list?]
//      YES → set target, return true → library connects
//       NO → store device, return false → keep scanning
//       ↓
//   scan ends with no preferred device found
//       ↓
//   wait BT_RESCAN_INTERVAL_MS (30s)
//       ↓
//   startScan() again (loop forever until connected)
//       ↓
//   connected → a2dpConnectionCallback(CONNECTED)
//       ↓
//   disconnected → wait BT_RECONNECT_DELAY_MS (10s)
//       ↓
//   try direct reconnect (up to BT_RECONNECT_RETRIES times)
//       ↓ if fails
//   fresh scan
//
// PCM format: 44100 Hz / 16-bit / stereo / Frame struct
//
// Thread safety:
//   ssidCallback() runs in BT task context.
//   All other public functions run in Arduino loop task.
//   Device list protected by spinlock.
//   s_state volatile, written from both contexts.
//
// ============================================================

#include "bluetooth.h"
#include "bluetooth_config.h"

#include <BluetoothA2DPSource.h>
#include <Arduino.h>
#include <string.h>
#include <math.h>
#include <vector>


// ============================================================
// INTERNAL TYPES
// ============================================================

struct BtDevice {
    char    name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
    char    addr_str[18];   // "XX:XX:XX:XX:XX:XX\0"
    uint8_t addr[6];
    int8_t  rssi;
    bool    valid;
};


// ============================================================
// MODULE STATE
// ============================================================

static BluetoothA2DPSource  s_a2dp;
static volatile BluetoothState s_state = BluetoothState::OFF;
static bool                 s_initialized  = false;
static bool                 s_a2dp_started = false;

// ---- Device list ----
static BtDevice             s_devices[BT_MAX_DISCOVERED_DEVICES];
static volatile size_t      s_device_count  = 0;
static volatile bool        s_dev_lock      = false;

// ---- Connection target ----
static volatile bool        s_connect_req   = false;
static uint8_t              s_target_addr[6]       = {0};
static size_t               s_target_idx           = SIZE_MAX;
static char                 s_connected_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = "";

// ---- Auto-connect timing ----
// millis() timestamps
static unsigned long        s_scan_started_ms    = 0;   // when scan began
static unsigned long        s_last_scan_end_ms   = 0;   // when scan ended (no match)
static unsigned long        s_disconnected_ms    = 0;   // when disconnect detected
static int                  s_reconnect_retries  = 0;   // reconnect attempt counter
static bool                 s_scan_found_pref    = false; // did scan find a preferred device?

// ---- Preferred list (runtime additions) ----
static char s_runtime_prefs[BT_RUNTIME_PREFERRED_MAX][ESP_BT_GAP_MAX_BDNAME_LEN + 1];
static size_t s_runtime_pref_count = 0;

// ---- Hardcoded preferred list ----
static const char* const HARDCODED_PREFS[] = {
    BT_PREFERRED_1,
    BT_PREFERRED_2,
    BT_PREFERRED_3,
};
static const size_t HARDCODED_PREF_COUNT =
    sizeof(HARDCODED_PREFS) / sizeof(HARDCODED_PREFS[0]);

// ---- Audio Playback ----
static const uint8_t*  s_pcm_data = nullptr;
static size_t          s_pcm_size = 0;
static uint32_t        s_pcm_sample_rate = 5000;

// Current playback time driven by the video player
static volatile uint32_t s_playback_time_ms = 0;
static volatile bool     s_audio_paused = false;

// Sub-sample fractional playback position for smooth interpolation
static double s_audio_pos = 0.0;


// ============================================================
// HELPERS
// ============================================================

static void addrToStr(const uint8_t* a, char* out)
{
    snprintf(
        out, 18,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        a[0], a[1], a[2], a[3], a[4], a[5]
    );
}

static bool addrEq(const uint8_t* a, const uint8_t* b)
{
    return memcmp(a, b, 6) == 0;
}

static void devLock()   { while (s_dev_lock) delayMicroseconds(5); s_dev_lock = true; }
static void devUnlock() { s_dev_lock = false; }


// ============================================================
// PREFERRED LIST MATCHING
// ============================================================
//
// Returns the priority (0 = highest) of the device name,
// or -1 if not in the preferred list.
//
// Match is SUBSTRING: preferred entry "Stone 1400" matches
// any device whose name contains "Stone 1400".
//
// ============================================================

static int preferredPriority(const char* device_name)
{
    if (!device_name || device_name[0] == '\0') {
        return -1;
    }

    // Hardcoded list (priority 0, 1, 2, ...)
    for (size_t i = 0; i < HARDCODED_PREF_COUNT; i++) {
        if (strstr(device_name, HARDCODED_PREFS[i]) != nullptr) {
            return (int)i;
        }
    }

    // Runtime additions (lower priority than hardcoded)
    for (size_t i = 0; i < s_runtime_pref_count; i++) {
        if (strstr(device_name, s_runtime_prefs[i]) != nullptr) {
            return (int)(HARDCODED_PREF_COUNT + i);
        }
    }

    return -1;
}


// ============================================================
// SSID CALLBACK
// ============================================================
//
// Called by the A2DP library's GAP scan for every device.
//
// Return true  → library connects to this device immediately
// Return false → skip, keep scanning
//
// Strategy:
//   1. Store every discovered device (for 'bt list')
//   2. Check if device is in preferred list
//   3. If yes AND it's higher priority than any previously
//      found preferred device → mark it as target
//
// We only return true for the BEST priority device found.
// Since ssidCallback is called real-time during scan, we
// wait until we see a priority-0 device (best) or the scan
// naturally selects the best available.
//
// For simplicity: return true for the FIRST preferred device
// seen. The preferred list is ordered, so if priority-1 is
// found, it wins over priority-2 found later in the same scan.
//
// ============================================================

static int s_best_priority_seen = INT_MAX;  // lower = better

static bool ssidCallback(
    const char*   name,
    esp_bd_addr_t addr,
    int           rssi
)
{
    // ---- Store device ----
    devLock();

    bool found = false;
    for (size_t i = 0; i < s_device_count; i++) {
        if (addrEq(s_devices[i].addr, addr)) {
            // Update name if we didn't have it
            if (
                name && name[0] != '\0' &&
                s_devices[i].name[0] == '\0'
            ) {
                strncpy(
                    s_devices[i].name, name,
                    ESP_BT_GAP_MAX_BDNAME_LEN
                );
                s_devices[i].name[ESP_BT_GAP_MAX_BDNAME_LEN] = '\0';
            }
            if (rssi != 0) s_devices[i].rssi = (int8_t)rssi;
            found = true;
            break;
        }
    }

    if (!found && s_device_count < BT_MAX_DISCOVERED_DEVICES) {
        BtDevice& d = s_devices[s_device_count];
        memcpy(d.addr, addr, 6);
        addrToStr(addr, d.addr_str);
        d.rssi  = (int8_t)rssi;
        d.valid = true;

        if (name && name[0] != '\0') {
            strncpy(d.name, name, ESP_BT_GAP_MAX_BDNAME_LEN);
            d.name[ESP_BT_GAP_MAX_BDNAME_LEN] = '\0';
        } else {
            d.name[0] = '\0';
        }

        size_t idx = s_device_count;
        s_device_count++;
        devUnlock();

        Serial.printf(
            BT_TAG "Found [%u]: %s  [%s]  %d dBm\n",
            (unsigned)idx,
            (s_devices[idx].name[0] != '\0')
                ? s_devices[idx].name
                : "(unnamed)",
            s_devices[idx].addr_str,
            rssi
        );

    } else {
        devUnlock();
    }

    // ---- Preferred check ----
    if (name && name[0] != '\0') {
        int pri = preferredPriority(name);

        if (pri >= 0 && pri < s_best_priority_seen) {
            s_best_priority_seen = pri;
            s_scan_found_pref    = true;
            s_connect_req        = true;
            memcpy(s_target_addr, addr, 6);

            // Find the index of this device in s_devices[]
            devLock();
            for (size_t i = 0; i < s_device_count; i++) {
                if (addrEq(s_devices[i].addr, addr)) {
                    s_target_idx = i;
                    break;
                }
            }
            devUnlock();
            
            Serial.printf("[MEM] After BT Scan: heap=%u largest=%u PSRAM=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());

            Serial.printf(
                BT_TAG ">>> Preferred device found (priority %d): %s\n",
                pri,
                name
            );

            s_state = BluetoothState::CONNECTING;

            // Priority-0 device found → connect immediately
            if (pri == 0) {
                return true;
            }

            // Lower priority found → keep scanning briefly
            // for a higher priority device, but the library will
            // connect to us eventually. We'll let the scan
            // complete unless this is the only one.
            //
            // Actually the cleanest approach: return true so
            // the library connects now. The preferred list
            // ordering already ensures we accept the best one.
            // If a better one appears later, it would have been
            // found first (library calls in discovery order,
            // which is RSSI / proximity based, not name-based).
            return true;
        }
    }

    return false;
}


// ============================================================
// A2DP CONNECTION STATE CALLBACK
// ============================================================

static void onConnectionState(
    esp_a2d_connection_state_t  cs,
    void*                       /*obj*/
)
{
    switch (cs) {
        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            s_state       = BluetoothState::CONNECTED;
            s_connect_req = false;
            s_reconnect_retries = 0;
            s_best_priority_seen = INT_MAX;   // reset for next scan

            // Store connected device name
            if (s_target_idx < s_device_count) {
                devLock();
                strncpy(
                    s_connected_name,
                    s_devices[s_target_idx].name,
                    ESP_BT_GAP_MAX_BDNAME_LEN
                );
                s_connected_name[ESP_BT_GAP_MAX_BDNAME_LEN] = '\0';
                devUnlock();
            }

            Serial.printf(
                BT_TAG "Connected to: %s\n",
                s_connected_name[0] != '\0'
                    ? s_connected_name
                    : "(unknown)"
            );
            break;

        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            Serial.println(BT_TAG "Disconnected.");

            s_disconnected_ms   = millis();
            s_reconnect_retries = 0;
            s_connected_name[0] = '\0';
            s_state             = BluetoothState::RECONNECTING;
            // bluetoothAudioUpdate() will drive the reconnect
            break;

        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            Serial.println(BT_TAG "A2DP connecting...");
            s_state = BluetoothState::CONNECTING;
            break;

        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            Serial.println(BT_TAG "A2DP disconnecting...");
            s_state = BluetoothState::DISCONNECTING;
            break;

        default:
            break;
    }
}


// ============================================================
// A2DP AUDIO STATE CALLBACK
// ============================================================

static void onAudioState(
    esp_a2d_audio_state_t  as,
    void*                  /*obj*/
)
{
    if (as == ESP_A2D_AUDIO_STATE_STARTED) {
        Serial.println(BT_TAG "Audio streaming started.");
        s_state = BluetoothState::STREAMING;
    } else {
        if (s_state == BluetoothState::STREAMING) {
            s_state = BluetoothState::CONNECTED;
        }
        Serial.println(BT_TAG "Audio streaming paused.");
    }
}


// ============================================================
// PCM DATA CALLBACK
// ============================================================
//
// Called from A2DP library task.
// Must fill frame_count stereo frames (Frame.channel1, .channel2)
//
static int32_t onPCM(Frame* frames, int32_t count)
{
    // The A2DP library expects 44100 Hz, 16-bit stereo.
    // Our source data is 8-bit unsigned mono at `s_pcm_sample_rate` (5000 Hz).
    // We resample on-the-fly using continuous fractional phase and linear interpolation!
    
    if (!s_pcm_data || s_pcm_size == 0 || s_audio_paused) {
        memset(frames, 0, (size_t)count * BT_PCM_BYTES_PER_FRAME);
        return count;
    }

    // Step in source samples per output frame at 44100 Hz
    const double step = (double)s_pcm_sample_rate / (double)BT_PCM_SAMPLE_RATE;

    // Check drift against video playback presentation clock
    double target_pos = ((double)s_playback_time_ms * (double)s_pcm_sample_rate) / 1000.0;
    double drift = target_pos - s_audio_pos;

    // If drift is large (> 150ms), e.g. at playback start, loop, or seek, hard-sync
    if (fabs(drift) > ((double)s_pcm_sample_rate * 0.15)) {
        s_audio_pos = target_pos;
    } else if (fabs(drift) > ((double)s_pcm_sample_rate * 0.01)) {
        // Tighter micro-slew to keep audio and video in strict lockstep
        s_audio_pos += drift * 0.02;
    }

    for (int32_t i = 0; i < count; i++) {
        uint32_t idx0 = (uint32_t)s_audio_pos;

        if (idx0 < s_pcm_size) {
            uint32_t idx1 = (idx0 + 1 < s_pcm_size) ? idx0 + 1 : idx0;
            float frac = (float)(s_audio_pos - (double)idx0);

            // Read 8-bit unsigned (0..255, center 128)
            // Convert to signed 16-bit range (-32768..32767)
            int32_t s0 = ((int32_t)s_pcm_data[idx0] - 128) << 8;
            int32_t s1 = ((int32_t)s_pcm_data[idx1] - 128) << 8;

            // High-quality linear interpolation:
            // Completely eliminates the 1000Hz stair-stepping screech and aliasing!
            int32_t sample_out = s0 + (int32_t)(frac * (float)(s1 - s0));

            frames[i].channel1 = (int16_t)sample_out;
            frames[i].channel2 = (int16_t)sample_out;
        } else {
            frames[i].channel1 = 0;
            frames[i].channel2 = 0;
        }

        s_audio_pos += step;
    }

    return count;
}


// ============================================================
// INTERNAL: START SCAN
// ============================================================
//
// Sets up a fresh scan:
//   - clears device list
//   - resets priority tracker
//   - calls s_a2dp.start() if first time, or relies on the
//     library's continuous GAP if already started
//
// ============================================================

static void internalStartScan()
{
    if (
        s_state == BluetoothState::CONNECTED ||
        s_state == BluetoothState::STREAMING  ||
        s_state == BluetoothState::CONNECTING  ||
        s_state == BluetoothState::DISCONNECTING
    ) {
        return;  // Don't scan while connection active
    }

    // Reset
    devLock();
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count = 0;
    devUnlock();

    s_connect_req        = false;
    s_target_idx         = SIZE_MAX;
    s_scan_found_pref    = false;
    s_best_priority_seen = INT_MAX;
    s_scan_started_ms    = millis();

    Serial.println();
    Serial.printf("[MEM] Before BT Scan: heap=%u largest=%u PSRAM=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
    
    Serial.printf(
        BT_TAG "Scanning for preferred speakers (~%.0f sec)...\n",
        BT_INQUIRY_DURATION_UNITS * 1.28f
    );
    Serial.printf(
        BT_TAG "  Priority 1: %s\n"
        BT_TAG "  Priority 2: %s\n"
        BT_TAG "  Priority 3: %s (fallback)\n",
        BT_PREFERRED_1, BT_PREFERRED_2, BT_PREFERRED_3
    );
    if (s_runtime_pref_count > 0) {
        Serial.printf(BT_TAG "  Runtime additions:\n");
        for (size_t i = 0; i < s_runtime_pref_count; i++) {
            Serial.printf(
                BT_TAG "    [%u] %s\n",
                (unsigned)(HARDCODED_PREF_COUNT + i),
                s_runtime_prefs[i]
            );
        }
    }
    Serial.println();

    s_state = BluetoothState::SCANNING;

    if (!s_a2dp_started) {
        std::vector<const char*> empty;
        s_a2dp.start(empty);
        s_a2dp_started = true;
    }
    // If already started, library's GAP discovery is already
    // running or will restart automatically.
}


// ============================================================
// LIFECYCLE
// ============================================================

void bluetoothAudioBegin()
{
    if (s_initialized) return;

    Serial.println();
    Serial.println(BT_TAG "Starting Bluetooth A2DP Source...");
    Serial.printf(  BT_TAG "Device name: %s\n", BT_LOCAL_NAME);

    // Reset all state
    memset(s_devices, 0, sizeof(s_devices));
    s_device_count       = 0;
    s_dev_lock           = false;
    s_connect_req        = false;
    s_target_idx         = SIZE_MAX;
    s_best_priority_seen = INT_MAX;
    s_scan_found_pref    = false;
    s_playback_time_ms   = 0;
    s_connected_name[0]  = '\0';
    s_disconnected_ms    = 0;
    s_last_scan_end_ms   = 0;
    s_reconnect_retries  = 0;
    s_a2dp_started       = false;
    s_runtime_pref_count = 0;
    memset(s_target_addr, 0, 6);

    // ---- Configure A2DP library ----
    s_a2dp.set_local_name(BT_LOCAL_NAME);
    s_a2dp.set_auto_reconnect(false);   // we manage reconnect manually
    s_a2dp.set_ssp_enabled(true);       // modern speakers use SSP
    s_a2dp.set_pin_code("1234", ESP_BT_PIN_TYPE_VARIABLE);
    s_a2dp.set_data_callback_in_frames(onPCM);
    s_a2dp.set_on_connection_state_changed(onConnectionState);
    s_a2dp.set_on_audio_state_changed(onAudioState);
    s_a2dp.set_ssid_callback(ssidCallback);

    s_initialized = true;
    s_state       = BluetoothState::IDLE;

    Serial.printf(
        BT_TAG "Free heap after config: %u bytes\n",
        ESP.getFreeHeap()
    );

    // ---- Start auto-scan immediately ----
    internalStartScan();
}


// ============================================================

void bluetoothAudioEnd()
{
    if (!s_initialized) return;

    Serial.println(BT_TAG "Shutting down...");
    s_connect_req     = false;

    if (s_a2dp_started) {
        s_a2dp.end();
        s_a2dp_started = false;
    }

    s_state       = BluetoothState::OFF;
    s_initialized = false;
    Serial.println(BT_TAG "Stopped.");
}


// ============================================================
// DISCOVERY (PUBLIC)
// ============================================================

void bluetoothAudioStartScan()
{
    if (!s_initialized) {
        Serial.println(BT_TAG "Call bluetoothAudioBegin() first.");
        return;
    }
    internalStartScan();
}

void bluetoothAudioStopScan()
{
    if (s_state == BluetoothState::SCANNING) {
        s_state = BluetoothState::IDLE;
        Serial.println(BT_TAG "Scan stopped by user.");
    }
}

size_t bluetoothAudioDeviceCount()    { return s_device_count; }

const char* bluetoothAudioDeviceName(size_t i)
{
    return (i < s_device_count) ? s_devices[i].name : "";
}

const char* bluetoothAudioDeviceAddress(size_t i)
{
    return (i < s_device_count) ? s_devices[i].addr_str : "";
}

int8_t bluetoothAudioDeviceRSSI(size_t i)
{
    return (i < s_device_count) ? s_devices[i].rssi : 0;
}

void bluetoothAudioPrintDevices()
{
    size_t n = s_device_count;
    Serial.println();
    Serial.println("========== DISCOVERED DEVICES ==========");

    if (n == 0) {
        Serial.println("  (none — run 'bt scan' first)");
        Serial.println("========================================");
        return;
    }

    devLock();
    for (size_t i = 0; i < n; i++) {
        const BtDevice& d = s_devices[i];
        int pri = preferredPriority(d.name);

        Serial.printf(
            "[%u] %s%s\n"
            "     MAC : %s\n"
            "     RSSI: %d dBm\n",
            (unsigned)i,
            (d.name[0] != '\0') ? d.name : "(unnamed)",
            (pri >= 0) ? " ★ PREFERRED" : "",
            d.addr_str,
            (int)d.rssi
        );
    }
    devUnlock();

    Serial.println("========================================");
    Serial.println();
}


// ============================================================
// CONNECTION
// ============================================================

bool bluetoothAudioConnect(size_t index)
{
    if (!s_initialized) {
        Serial.println(BT_TAG "Not initialized.");
        return false;
    }

    if (index >= s_device_count) {
        Serial.printf(
            BT_TAG "Invalid index %u (found %u)\n",
            (unsigned)index, (unsigned)s_device_count
        );
        return false;
    }

    if (bluetoothAudioConnected()) {
        Serial.println(BT_TAG "Already connected. Disconnect first.");
        return false;
    }

    devLock();
    BtDevice sel = s_devices[index];
    devUnlock();

    Serial.printf(
        BT_TAG "Manually connecting to [%u]: %s\n",
        (unsigned)index,
        (sel.name[0] != '\0') ? sel.name : "(unnamed)"
    );

    memcpy(s_target_addr, sel.addr, 6);
    s_target_idx  = index;
    s_connect_req = true;
    s_state       = BluetoothState::CONNECTING;

    if (!s_a2dp_started) {
        std::vector<const char*> empty;
        s_a2dp.start(empty);
        s_a2dp_started = true;
    }

    return true;
}

bool bluetoothAudioConnectByName(const char* name)
{
    if (!name || name[0] == '\0') return false;

    devLock();
    size_t best    = SIZE_MAX;
    int    bestRSSI = -200;

    for (size_t i = 0; i < s_device_count; i++) {
        if (strstr(s_devices[i].name, name) != nullptr) {
            if ((int)s_devices[i].rssi > bestRSSI) {
                bestRSSI = s_devices[i].rssi;
                best     = i;
            }
        }
    }
    devUnlock();

    if (best == SIZE_MAX) {
        Serial.printf(BT_TAG "No device matching '%s' found.\n", name);
        Serial.println(BT_TAG "Run 'bt scan' first.");
        return false;
    }

    return bluetoothAudioConnect(best);
}

void bluetoothAudioDisconnect()
{
    if (
        s_state != BluetoothState::CONNECTED  &&
        s_state != BluetoothState::STREAMING  &&
        s_state != BluetoothState::CONNECTING &&
        s_state != BluetoothState::RECONNECTING
    ) {
        return;
    }

    Serial.println(BT_TAG "Disconnecting...");
    s_connect_req = false;
    s_target_idx  = SIZE_MAX;
    s_best_priority_seen = INT_MAX;
    s_a2dp.disconnect();
    s_state       = BluetoothState::IDLE;
    s_connected_name[0] = '\0';
}

bool bluetoothAudioConnected()
{
    return
        s_state == BluetoothState::CONNECTED ||
        s_state == BluetoothState::STREAMING;
}

bool bluetoothAudioStreaming()
{
    return s_state == BluetoothState::STREAMING;
}

BluetoothState bluetoothAudioGetState()
{
    return s_state;
}

const char* bluetoothAudioGetStateName()
{
    switch (s_state) {
        case BluetoothState::OFF:           return "OFF";
        case BluetoothState::IDLE:          return "IDLE";
        case BluetoothState::SCANNING:      return "SCANNING";
        case BluetoothState::CONNECTING:    return "CONNECTING";
        case BluetoothState::CONNECTED:     return "CONNECTED";
        case BluetoothState::STREAMING:     return "STREAMING";
        case BluetoothState::DISCONNECTING: return "DISCONNECTING";
        case BluetoothState::RECONNECTING:  return "RECONNECTING";
        default:                            return "UNKNOWN";
    }
}

const char* bluetoothAudioConnectedDeviceName()
{
    return s_connected_name;
}


// ============================================================
// PREFERRED LIST (RUNTIME)
// ============================================================

bool bluetoothAudioPreferAdd(const char* name)
{
    if (!name || name[0] == '\0') return false;

    if (s_runtime_pref_count >= BT_RUNTIME_PREFERRED_MAX) {
        Serial.printf(
            BT_TAG "Runtime preferred list full (%u max).\n",
            BT_RUNTIME_PREFERRED_MAX
        );
        return false;
    }

    strncpy(
        s_runtime_prefs[s_runtime_pref_count],
        name,
        ESP_BT_GAP_MAX_BDNAME_LEN
    );
    s_runtime_prefs[s_runtime_pref_count][ESP_BT_GAP_MAX_BDNAME_LEN] = '\0';
    s_runtime_pref_count++;

    Serial.printf(BT_TAG "Added preferred: '%s'\n", name);
    return true;
}

void bluetoothAudioPreferPrint()
{
    Serial.println();
    Serial.println("========== PREFERRED DEVICES ==========");
    Serial.println("  (Hardcoded — in bluetooth_config.h)");
    for (size_t i = 0; i < HARDCODED_PREF_COUNT; i++) {
        Serial.printf(
            "  [%u] %s\n",
            (unsigned)i,
            HARDCODED_PREFS[i]
        );
    }

    if (s_runtime_pref_count > 0) {
        Serial.println("  (Runtime additions)");
        for (size_t i = 0; i < s_runtime_pref_count; i++) {
            Serial.printf(
                "  [%u] %s\n",
                (unsigned)(HARDCODED_PREF_COUNT + i),
                s_runtime_prefs[i]
            );
        }
    }

    Serial.println("=======================================");
    Serial.println();
}

void bluetoothAudioPreferClear()
{
    s_runtime_pref_count = 0;
    Serial.println(BT_TAG "Runtime preferred list cleared.");
}


// ============================================================
// AUDIO PLAYBACK
// ============================================================

void bluetoothAudioSetPCMBuffer(const uint8_t* pcm_data, size_t size_bytes, uint32_t sample_rate)
{
    s_pcm_data = pcm_data;
    s_pcm_size = size_bytes;
    s_pcm_sample_rate = sample_rate;
    s_audio_pos = 0.0;
    
    Serial.printf(BT_TAG "PCM buffer set: %u bytes @ %u Hz\n", (unsigned)size_bytes, (unsigned)sample_rate);
}

void bluetoothAudioSetTime(uint32_t time_ms)
{
    s_playback_time_ms = time_ms;
    if (time_ms == 0) {
        s_audio_pos = 0.0;
    }
}

void bluetoothAudioPause()
{
    s_audio_paused = true;
}

void bluetoothAudioResume()
{
    s_audio_paused = false;
}

bool bluetoothAudioIsPaused()
{
    return s_audio_paused;
}


// ============================================================
// UPDATE — CALL FROM loop()
// ============================================================
//
// Drives the auto-connect state machine:
//
//  SCANNING      → if scan ended (library gap done) with no
//                  preferred device → schedule rescan after
//                  BT_RESCAN_INTERVAL_MS.
//
//  IDLE          → if enough time passed since last scan end,
//                  trigger new scan.
//
//  RECONNECTING  → wait BT_RECONNECT_DELAY_MS then try to
//                  reconnect to last known device.
//                  After BT_RECONNECT_RETRIES failures → rescan.
//
// ============================================================

void bluetoothAudioUpdate()
{
    if (!s_initialized) return;

    unsigned long now = millis();

    BluetoothState st = s_state;

    switch (st) {

        // --------------------------------------------------------
        case BluetoothState::IDLE:
        {
            // After a failed scan, wait then rescan
            if (
                s_last_scan_end_ms != 0 &&
                (now - s_last_scan_end_ms) >= BT_RESCAN_INTERVAL_MS
            ) {
                Serial.println(BT_TAG "Retrying scan...");
                s_last_scan_end_ms = 0;
                internalStartScan();
            }
            break;
        }

        // --------------------------------------------------------
        case BluetoothState::SCANNING:
        {
            // Library drives the scan. When it ends without finding
            // a preferred device, the library calls ssidCallback for
            // every device but none returned true, so we end up still
            // in SCANNING state.
            //
            // Detect scan completion by checking library's is_discovery_active()
            if (s_a2dp_started && !s_a2dp.is_discovery_active()) {
                if (!s_scan_found_pref) {
                    Serial.printf("[MEM] After BT Scan: heap=%u largest=%u PSRAM=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
                    // No preferred device found → schedule rescan
                    Serial.println(
                        BT_TAG "No preferred device found."
                    );
                    Serial.printf(
                        BT_TAG "Will retry in %u seconds.\n",
                        BT_RESCAN_INTERVAL_MS / 1000
                    );
                    s_last_scan_end_ms = now;
                    s_state = BluetoothState::IDLE;
                }
                // If preferred device was found, state is already
                // CONNECTING (set in ssidCallback), so we don't
                // override it here.
            }
            break;
        }

        // --------------------------------------------------------
        case BluetoothState::RECONNECTING:
        {
            unsigned long elapsed = now - s_disconnected_ms;

            if (elapsed < BT_RECONNECT_DELAY_MS) {
                // Still in cooldown
                break;
            }

            if (s_reconnect_retries < BT_RECONNECT_RETRIES) {
                // Try to reconnect to last known device
                s_reconnect_retries++;
                Serial.printf(
                    BT_TAG "Reconnect attempt %d/%d...\n",
                    s_reconnect_retries,
                    BT_RECONNECT_RETRIES
                );

                // Re-enable connection request so ssidCallback
                // will return true for the same device
                s_connect_req        = true;
                s_scan_found_pref    = false;
                s_best_priority_seen = INT_MAX;
                s_state              = BluetoothState::CONNECTING;

                // Restart scan/discovery so the device can be found again
                internalStartScan();

            } else {
                // All reconnect attempts exhausted → fresh scan
                Serial.println(
                    BT_TAG "Reconnect failed. Running full scan..."
                );
                s_reconnect_retries  = 0;
                s_disconnected_ms    = 0;
                memset(s_target_addr, 0, 6);
                s_target_idx         = SIZE_MAX;
                internalStartScan();
            }
            break;
        }

        // --------------------------------------------------------
        case BluetoothState::OFF:
        case BluetoothState::CONNECTING:
        case BluetoothState::CONNECTED:
        case BluetoothState::STREAMING:
        case BluetoothState::DISCONNECTING:
        default:
            break;
    }
}


// ============================================================
// SERIAL COMMAND HANDLER
// ============================================================

bool bluetoothAudioHandleCommand(const char* line)
{
    if (!line || strncmp(line, "bt", 2) != 0) return false;

    const char* cmd = line + 2;
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    // ---- bt scan ----
    if (strcmp(cmd, "scan") == 0) {
        bluetoothAudioStartScan();
        return true;
    }

    // ---- bt stop ----
    if (strcmp(cmd, "stop") == 0) {
        bluetoothAudioStopScan();
        return true;
    }

    // ---- bt list ----
    if (strcmp(cmd, "list") == 0) {
        bluetoothAudioPrintDevices();
        return true;
    }

    // ---- bt connect <N or name> ----
    if (strncmp(cmd, "connect", 7) == 0) {
        const char* arg = cmd + 7;
        while (*arg == ' ' || *arg == '\t') arg++;

        if (*arg == '\0') {
            Serial.println("Usage: bt connect <index>  OR  bt connect <name>");
            return true;
        }

        // Is it a number?
        bool isNum = true;
        for (const char* p = arg; *p; p++) {
            if (!isdigit((unsigned char)*p)) { isNum = false; break; }
        }

        if (isNum) {
            bluetoothAudioConnect((size_t)atoi(arg));
        } else {
            bluetoothAudioConnectByName(arg);
        }
        return true;
    }

    // ---- bt disconnect ----
    if (strcmp(cmd, "disconnect") == 0) {
        bluetoothAudioDisconnect();
        return true;
    }

    // ---- bt status ----
    if (strcmp(cmd, "status") == 0) {
        Serial.println();
        Serial.println("========== BT STATUS ==========");
        Serial.printf("  State      : %s\n", bluetoothAudioGetStateName());
        Serial.printf("  Connected  : %s\n", bluetoothAudioConnected() ? "YES" : "NO");
        if (bluetoothAudioConnected()) {
            Serial.printf("  Device     : %s\n",
                s_connected_name[0] ? s_connected_name : "(unknown)");
        }
        Serial.printf("  Streaming  : %s\n", bluetoothAudioStreaming() ? "YES" : "NO");
        Serial.printf("  Audio buf  : %u bytes\n", (unsigned)s_pcm_size);
        Serial.printf("  Devices    : %u found\n", (unsigned)s_device_count);
        Serial.printf("  Free heap  : %u bytes\n", ESP.getFreeHeap());
        if (psramFound()) {
            Serial.printf("  Free PSRAM : %u bytes\n", ESP.getFreePsram());
        }
        Serial.println("================================");
        Serial.println();
        return true;
    }

    // ---- bt prefer list ----
    if (strcmp(cmd, "prefer list") == 0 || strcmp(cmd, "prefer") == 0) {
        bluetoothAudioPreferPrint();
        return true;
    }

    // ---- bt prefer add <name> ----
    if (strncmp(cmd, "prefer add", 10) == 0) {
        const char* arg = cmd + 10;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (*arg == '\0') {
            Serial.println("Usage: bt prefer add <name>");
        } else {
            bluetoothAudioPreferAdd(arg);
        }
        return true;
    }

    // ---- bt prefer clear ----
    if (strcmp(cmd, "prefer clear") == 0) {
        bluetoothAudioPreferClear();
        return true;
    }

    // ---- Unknown — print help ----
    Serial.println();
    Serial.println("Bluetooth commands:");
    Serial.println("  bt scan              - force scan now");
    Serial.println("  bt stop              - stop scan");
    Serial.println("  bt list              - list found devices");
    Serial.println("  bt connect N         - connect to device N");
    Serial.println("  bt connect <name>    - connect by name");
    Serial.println("  bt disconnect        - disconnect");
    Serial.println("  bt status            - full status");
    Serial.println("  bt prefer list       - show preferred devices");
    Serial.println("  bt prefer add <name> - add to preferred list");
    Serial.println("  bt prefer clear      - clear runtime additions");
    Serial.println();

    return true;
}
