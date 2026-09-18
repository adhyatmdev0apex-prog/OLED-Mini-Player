#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <Arduino.h>

// ============================================================
// ESP32-CAM REMOTE MEDIA SYSTEM CONFIGURATION
// ============================================================

// --- Wi-Fi Configuration ---
#ifndef WIFI_SSID
#define WIFI_SSID "first-priority wifi ssid"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "first-priority wifi pass"
#endif

// Secondary / Fallback Wi-Fi
#ifndef WIFI_FALLBACK_SSID
#define WIFI_FALLBACK_SSID "fallback wifi ssid"
#endif

#ifndef WIFI_FALLBACK_PASSWORD
#define WIFI_FALLBACK_PASSWORD "fallback wifi pass"
#endif

#define WIFI_CONNECT_TIMEOUT_MS 12000
#define WIFI_RECONNECT_INTERVAL_MS 10000

// --- Persistent Offline Storage (LittleFS / SPIFFS) ---
#define OFFLINE_STORAGE_LIMIT_BYTES (2050000UL) // 2.05 MB maximum per offline media package
#define OFFLINE_CATALOGUE_PATH "/offline_catalogue.json"

// --- Server & API Configuration ---
// IMPORTANT: "localhost" will NOT work from the ESP32-CAM!
// Use the computer's LAN IP address (e.g. http://192.168.1.10:3000)
// or a publicly reachable web server URL.
// Do NOT include a trailing slash in MEDIA_SERVER_BASE_URL.
#ifndef MEDIA_SERVER_BASE_URL
#define MEDIA_SERVER_BASE_URL "https://oled-mini-player.onrender.com" // <-- Update this to your server URL
#endif

#define HTTP_TIMEOUT_MS 10000
#define DOWNLOAD_TIMEOUT_MS 60000
#define CATALOGUE_PAGE_SIZE 10

// --- Memory & Download Limits ---
#define MAX_VIDEO_SIZE (3500000UL)       // 3.5 MB
#define MAX_AUDIO_SIZE (2500000UL)       // 2.5 MB (supports full-length audio in PSRAM)
#define MAX_COMBINED_MEDIA_SIZE (3900000UL) // 3.9 MB (fits in 4.19MB PSRAM)
#define PSRAM_SAFETY_MARGIN_BYTES (250000UL) // 250 KB reserved for stack/heap/BT

// --- OLED Pin Configuration ---
#define OLED_SDA 13
#define OLED_SCL 14
#define OLED_I2C_SPEED 800000

// --- Local Fallback Assets ---
#define VIDEO_FILE "/stickman.vs.geometry_dash.bin"
#define AUDIO_FILE "/stickman.vs.geometry_dash.pcm"

// --- Audio Format Configuration ---
#define AUDIO_SAMPLE_RATE 5000 // 5000 Hz, 8-bit unsigned PCM mono
#define BT_AUDIO_SYNC_OFFSET_MS 80 // Pre-lead audio by 80ms to match Bluetooth speaker buffer latency

// --- Physical Button Configuration (Active-LOW to GND) ---
#define PIN_BUTTON_SCROLL 2              // GPIO 2: Scroll down (hold to keep scrolling)
#define PIN_BUTTON_SELECT 15             // GPIO 15: Select / Play / Pause (hold > 1s to stop)
#define BUTTON_DEBOUNCE_MS 40            // Debounce period
#define BUTTON_HOLD_SCROLL_INITIAL_MS 450 // Delay before continuous auto-scroll starts
#define BUTTON_HOLD_SCROLL_RATE_MS 200   // Repeat rate for continuous auto-scroll
#define BUTTON_LONG_PRESS_MS 1000        // 1 sec hold triggers Stop & Return to menu

#endif // APP_CONFIG_H
