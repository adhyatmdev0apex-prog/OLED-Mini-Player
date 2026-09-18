#include <Arduino.h>
#include "config/app_config.h"
#include "wifi/wifi_manager.h"
#include "display/oled_display.h"
#include "content/content_manager.h"
#include "playback/playback_manager.h"
#include "transport/command_protocol.h"
#include "audio/bluetooth/bluetooth.h"
#include "input/input_manager.h"

// ============================================================
// ESP32-CAM REMOTE MEDIA FETCHING & PLAYBACK SYSTEM (PHASE 1)
// ============================================================

enum class AppState {
    BOOT,
    INIT,
    WIFI_CONNECTING,
    FETCHING_CATALOGUE,
    CATALOGUE_BROWSE,
    DOWNLOADING,
    SAVING_OFFLINE,
    PLAYING
};

static AppState s_appState = AppState::BOOT;
static bool s_initialCatalogueFetched = false;
static size_t s_selectedCatalogueIndex = 0;

static void displayCurrentCatalogueItem() {
    size_t total = ContentManager::getInstance().getItemCount();
    if (total == 0) {
        if (ContentManager::getInstance().isOfflineMode()) {
            OLEDDisplay::getInstance().showStatusScreen("Offline Storage", "No Offline Videos", "Connect Wi-Fi");
        } else {
            OLEDDisplay::getInstance().showStatusScreen("Catalogue Empty", "Upload via Web UI", "oled-mini-player");
        }
        return;
    }
    if (s_selectedCatalogueIndex >= total) {
        s_selectedCatalogueIndex = 0;
    }
    const MediaItem* item = ContentManager::getInstance().getItem(s_selectedCatalogueIndex);
    if (item) {
        OLEDDisplay::getInstance().showCatalogueScreen(
            s_selectedCatalogueIndex,
            total,
            item->name.c_str(),
            item->id.c_str(),
            item->isSpiffsCompatible,
            item->isOfflineStored,
            ContentManager::getInstance().isOfflineMode()
        );
    }
}

void setup() {
    // 1. Serial Initialization (for development/debugging)
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("========================================");
    Serial.println("   ESP32-CAM REMOTE MEDIA SYSTEM v1.0   ");
    Serial.println("========================================");
    Serial.println();

    // 2. PSRAM & Heap Memory Safety Checks
    Serial.printf("[SYSTEM] PSRAM Available : %s\n", psramFound() ? "YES" : "NO");
    if (psramFound()) {
        Serial.printf("[SYSTEM] Total PSRAM     : %u bytes\n", (unsigned)ESP.getPsramSize());
        Serial.printf("[SYSTEM] Free PSRAM      : %u bytes\n", (unsigned)ESP.getFreePsram());
    } else {
        Serial.println("[SYSTEM] WARNING: PSRAM not detected!");
    }
    Serial.printf("[SYSTEM] Free Heap       : %u bytes\n\n", (unsigned)ESP.getFreeHeap());

    // 3. OLED Display Initialization
    Serial.println("[SYSTEM] Initializing OLED Display...");
    OLEDDisplay::getInstance().begin();
    OLEDDisplay::getInstance().showSplashScreen();

    // 4. Physical Push Buttons Initialization (GPIO 2 & 4)
    Serial.println("[SYSTEM] Initializing Physical Button Controls...");
    InputManager::getInstance().begin();

    // 5. Playback Manager Initialization
    Serial.println("[SYSTEM] Initializing Playback Engine...");
    PlaybackManager::getInstance().begin();

    // 6. Bluetooth Classic A2DP Source: Deferred until after initial network catalogue fetch
    Serial.println("[SYSTEM] Bluetooth Classic A2DP Source: Standing by (will activate after catalogue sync)...");

    // 7. Content Manager Initialization
    Serial.println("[SYSTEM] Initializing Content API Manager...");
    ContentManager::getInstance().begin();

    // 8. Command Protocol & Serial Shell Initialization (for dev tools)
    Serial.println("[SYSTEM] Initializing Serial Command Shell...");
    CommandProtocol::getInstance().begin();

    // 9. Attempt LittleFS Fallback Load (if assets exist)
    Serial.println("[SYSTEM] Checking for LittleFS local fallback media...");
    if (PlaybackManager::getInstance().loadFallbackLittleFS()) {
        Serial.println("[SYSTEM] Loaded LittleFS fallback media into PSRAM.");
    } else {
        Serial.println("[SYSTEM] No local fallback media in LittleFS.");
    }

    // 10. Wi-Fi Connection Start (Primary with Fallback Profile)
    Serial.println("[SYSTEM] Starting Wi-Fi Manager (Dual-Network Fallback)...");
    WiFiManager::getInstance().begin();

    s_appState = AppState::WIFI_CONNECTING;
    Serial.println("\n[SYSTEM] Standalone Product Mode Active.");
    Serial.println("  Button IO2:  Scroll down catalogue (hold to continuously scroll)");
    Serial.println("  Button IO15: Select / Play / Pause (hold 1s to stop & return to menu)");
    Serial.println("  Serial commands also available. Type 'help' for shell.\n> ");
}

void loop() {
    // Service Serial Shell Input (development port)
    CommandProtocol::getInstance().pollSerial();

    // Service Wi-Fi State Machine
    WiFiManager::getInstance().update();

    // Service Bluetooth A2DP State Machine
    PlaybackManager::getInstance().update();

    // Main Application Standalone State Machine
    switch (s_appState) {
        case AppState::WIFI_CONNECTING:
            if (WiFiManager::getInstance().isConnected()) {
                OLEDDisplay::getInstance().showLoadingCatalogue();
                s_appState = AppState::FETCHING_CATALOGUE;
            } else if (WiFiManager::getInstance().hasExhaustedAllNetworks()) {
                Serial.println("[SYSTEM] All Wi-Fi connections failed. Launching Offline Media Library...");
                OLEDDisplay::getInstance().showStatusScreen("Wi-Fi Unavailable", "Loading Flash...", "Offline Mode");
                delay(1500);
                ContentManager::getInstance().switchToOfflineLibrary();
                s_selectedCatalogueIndex = 0;
                displayCurrentCatalogueItem();
                s_appState = AppState::CATALOGUE_BROWSE;

                Serial.println("[SYSTEM] Starting Bluetooth Classic A2DP Source for Offline Playback...");
                bluetoothAudioBegin();
            }
            break;

        case AppState::FETCHING_CATALOGUE:
            if (!s_initialCatalogueFetched) {
                if (ContentManager::getInstance().fetchCatalogue(0)) {
                    s_initialCatalogueFetched = true;
                    ContentManager::getInstance().printCatalogue();
                    s_selectedCatalogueIndex = 0;
                    displayCurrentCatalogueItem();
                    s_appState = AppState::CATALOGUE_BROWSE;

                    // Activate Bluetooth Audio now that initial catalogue sync is done
                    Serial.println("[SYSTEM] Starting Bluetooth Classic A2DP Source...");
                    bluetoothAudioBegin();
                } else {
                    OLEDDisplay::getInstance().showStatusScreen("Wi-Fi Connected",
                                                                "Catalogue Fetch Fail",
                                                                "Retrying...");
                    delay(3000);
                    // The loop will naturally re-enter this state and try again.
                }
            }
            break;

        case AppState::CATALOGUE_BROWSE: {
            // Poll physical hardware buttons
            InputEvent ev = InputManager::getInstance().poll();

            if (ev == InputEvent::SCROLL_NEXT) {
                // Button 1 (GPIO 2): Scroll to next video
                size_t total = ContentManager::getInstance().getItemCount();
                if (total > 0) {
                    s_selectedCatalogueIndex = (s_selectedCatalogueIndex + 1) % total;
                    displayCurrentCatalogueItem();
                    const MediaItem* cur = ContentManager::getInstance().getItem(s_selectedCatalogueIndex);
                    Serial.printf("[BUTTON] Scrolled to [%u/%u]: %s\n",
                                  (unsigned)(s_selectedCatalogueIndex + 1),
                                  (unsigned)total,
                                  cur ? cur->name.c_str() : "");
                }
            } else if (ev == InputEvent::SELECT_SHORT_PRESS) {
                // Button 2 (GPIO 4): Select media
                size_t total = ContentManager::getInstance().getItemCount();
                if (total > 0 && s_selectedCatalogueIndex < total) {
                    const MediaItem* cur = ContentManager::getInstance().getItem(s_selectedCatalogueIndex);
                    Serial.printf("[BUTTON] Selected [%u]: %s\n", (unsigned)s_selectedCatalogueIndex, cur ? cur->name.c_str() : "");

                    // If connected to Wi-Fi, item is offline-compatible, and not yet saved to flash:
                    // Action is DOWNLOAD TO FLASH, verify, and return to menu!
                    if (!ContentManager::getInstance().isOfflineMode() && cur && cur->isSpiffsCompatible && !cur->isOfflineStored) {
                        OLEDDisplay::getInstance().showDownloadingScreen(cur->name.c_str());
                        s_appState = AppState::SAVING_OFFLINE;
                    } else {
                        // Normal streaming item or local offline item -> stage to PSRAM and play!
                        OLEDDisplay::getInstance().showDownloadingScreen(cur ? cur->name.c_str() : "");
                        s_appState = AppState::DOWNLOADING;
                    }
                }
            }
            break;
        }

        case AppState::SAVING_OFFLINE: {
            const MediaItem* cur = ContentManager::getInstance().getItem(s_selectedCatalogueIndex);
            String name = cur ? cur->name : "Offline Media";
            if (ContentManager::getInstance().saveItemToFlash(s_selectedCatalogueIndex)) {
                OLEDDisplay::getInstance().showOfflineSavedSuccess(name.c_str());
                delay(2000);
            } else {
                OLEDDisplay::getInstance().showErrorScreen("Save to Flash Fail");
                delay(2000);
            }

            displayCurrentCatalogueItem();
            s_appState = AppState::CATALOGUE_BROWSE;
            break;
        }

        case AppState::DOWNLOADING: {
            DownloadedMedia downloaded;
            bool success = ContentManager::getInstance().downloadItem(s_selectedCatalogueIndex, downloaded);

            if (success) {
                PlaybackManager::getInstance().setMedia(downloaded);
                s_appState = AppState::PLAYING;

                // Start video playback on OLED & audio on BT (blocking frame loop with button polling)
                PlaybackManager::getInstance().playCurrent();

                // When video finishes or user stops by holding GPIO 4:
                displayCurrentCatalogueItem();
                s_appState = AppState::CATALOGUE_BROWSE;
            } else {
                OLEDDisplay::getInstance().showErrorScreen("Download Failed");
                delay(2500);
                displayCurrentCatalogueItem();
                s_appState = AppState::CATALOGUE_BROWSE;
            }
            break;
        }

        case AppState::PLAYING:
        default:
            break;
    }

    delay(1);
}
