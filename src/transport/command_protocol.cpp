#include "command_protocol.h"
#include "../wifi/wifi_manager.h"
#include "../content/content_manager.h"
#include "../playback/playback_manager.h"
#include "../audio/bluetooth/bluetooth.h"

CommandProtocol& CommandProtocol::getInstance() {
    static CommandProtocol instance;
    return instance;
}

CommandProtocol::CommandProtocol() : m_len(0) {
    m_buf[0] = '\0';
}

void CommandProtocol::begin() {
    m_len = 0;
    m_buf[0] = '\0';
}

void CommandProtocol::pollSerial() {
    while (Serial.available()) {
        char c = (char)Serial.read();

        if (c == '\r') continue;

        if (c == '\n') {
            m_buf[m_len] = '\0';
            handleLine(m_buf);
            m_len = 0;
            return;
        }

        if (m_len < (sizeof(m_buf) - 1)) {
            m_buf[m_len++] = c;
        }
    }
}

void CommandProtocol::handleLine(const char* line) {
    if (line[0] == '\0') {
        Serial.print("> ");
        return;
    }

    // Bluetooth commands
    if (strncmp(line, "bt", 2) == 0) {
        bool handled = bluetoothAudioHandleCommand(line);
        if (!handled) {
            Serial.println("[BT] Unknown bluetooth command. Type 'help'.");
        }
        Serial.print("> ");
        return;
    }

    // Wi-Fi commands
    if (strncmp(line, "wifi", 4) == 0) {
        if (strcmp(line, "wifi status") == 0) {
            Serial.printf("[WIFI] Status: %s | SSID: %s | IP: %s | RSSI: %d dBm\n",
                          WiFiManager::getInstance().getStatusString(),
                          WiFiManager::getInstance().getSSID().c_str(),
                          WiFiManager::getInstance().getIP().c_str(),
                          WiFiManager::getInstance().getRSSI());
        } else if (strncmp(line, "wifi connect", 12) == 0) {
            char ssid[64] = {0};
            char pass[64] = {0};
            int parsed = sscanf(line + 12, "%s %s", ssid, pass);
            if (parsed >= 1) {
                WiFiManager::getInstance().connect(ssid, parsed >= 2 ? pass : "");
            } else {
                Serial.println("Usage: wifi connect <ssid> [password]");
            }
        } else if (strcmp(line, "wifi ip") == 0) {
            Serial.printf("[WIFI] IP Address: %s\n", WiFiManager::getInstance().getIP().c_str());
        } else {
            Serial.println("Unknown wifi command. Try: 'wifi status', 'wifi connect <ssid> <pass>', 'wifi ip'");
        }
        Serial.print("> ");
        return;
    }

    // Content / Catalogue commands
    if (strncmp(line, "content", 7) == 0) {
        if (strcmp(line, "content status") == 0) {
            Serial.printf("[CONTENT] Base Server URL: %s\n", ContentManager::getInstance().getBaseUrl().c_str());
            Serial.printf("[CONTENT] Catalogue Total: %u | Cached Items: %u | Page: %u\n",
                          ContentManager::getInstance().getCatalogue().total,
                          (unsigned)ContentManager::getInstance().getCatalogue().items.size(),
                          ContentManager::getInstance().getCatalogue().page);
        } else if (strcmp(line, "content catalogue") == 0 || strcmp(line, "content list") == 0 || strcmp(line, "content refresh") == 0) {
            ContentManager::getInstance().fetchCatalogue(0);
            ContentManager::getInstance().printCatalogue();
        } else if (strncmp(line, "content fetch", 13) == 0) {
            int page = 0;
            if (sscanf(line + 13, "%d", &page) == 1 && page >= 0) {
                ContentManager::getInstance().fetchCatalogue(page);
                ContentManager::getInstance().printCatalogue();
            } else {
                Serial.println("Usage: content fetch <page_number>");
            }
        } else if (strncmp(line, "content download", 16) == 0) {
            char arg[64] = {0};
            if (sscanf(line + 16, "%s", arg) == 1) {
                int index = -1;
                DownloadedMedia downloaded;
                bool ok = false;
                if (sscanf(arg, "%d", &index) == 1 && index >= 0) {
                    ok = ContentManager::getInstance().downloadItem((size_t)index, downloaded);
                } else {
                    ok = ContentManager::getInstance().downloadItemById(arg, downloaded);
                }

                if (ok) {
                    PlaybackManager::getInstance().setMedia(downloaded);
                    PlaybackManager::getInstance().playCurrent();
                } else {
                    Serial.println("[CONTENT] Download failed. Preserving existing media.");
                }
            } else {
                Serial.println("Usage: content download <index_or_id>");
            }
        } else {
            Serial.println("Unknown content command. Try: 'content catalogue', 'content fetch <page>', 'content download <idx>'");
        }
        Serial.print("> ");
        return;
    }

    // Playurl command
    if (strncmp(line, "playurl", 7) == 0) {
        char vUrl[128] = {0};
        char aUrl[128] = {0};
        if (sscanf(line + 7, "%s %s", vUrl, aUrl) == 2) {
            DownloadedMedia downloaded;
            if (ContentManager::getInstance().downloadCustomUrls(vUrl, aUrl, downloaded)) {
                PlaybackManager::getInstance().setMedia(downloaded);
                PlaybackManager::getInstance().playCurrent();
            } else {
                Serial.println("[PLAYURL] Error: Download of custom URLs failed.");
            }
        } else {
            Serial.println("Usage: playurl <video_url> <audio_url>");
        }
        Serial.print("> ");
        return;
    }

    // Server configuration command
    if (strncmp(line, "server", 6) == 0) {
        char url[128] = {0};
        if (sscanf(line + 6, "%s", url) == 1) {
            ContentManager::getInstance().setBaseUrl(url);
        } else {
            Serial.printf("Current base server URL: %s\nUsage: server <base_url>\n",
                          ContentManager::getInstance().getBaseUrl().c_str());
        }
        Serial.print("> ");
        return;
    }

    // Playback control commands
    if (strcmp(line, "play") == 0) {
        if (PlaybackManager::getInstance().getState() == PlaybackState::PLAYING) {
            Serial.println("[PLAYBACK] Video is already playing.");
        } else if (PlaybackManager::getInstance().isLoaded()) {
            PlaybackManager::getInstance().playCurrent();
        } else {
            Serial.println("[PLAYBACK] No media loaded. Fetch catalogue and download an item first.");
        }
        Serial.print("> ");
        return;
    }

    if (strcmp(line, "stop") == 0) {
        PlaybackManager::getInstance().stop();
        Serial.print("> ");
        return;
    }

    if (strcmp(line, "pause") == 0) {
        PlaybackManager::getInstance().pause();
        Serial.print("> ");
        return;
    }

    if (strcmp(line, "resume") == 0) {
        PlaybackManager::getInstance().resume();
        Serial.print("> ");
        return;
    }

    // Help command
    if (strcmp(line, "help") == 0) {
        Serial.println();
        Serial.println("========================================");
        Serial.println("     ESP32-CAM COMMAND INTERFACE");
        Serial.println("========================================");
        Serial.println("Wi-Fi Commands:");
        Serial.println("  wifi status                    - Show Wi-Fi status and RSSI");
        Serial.println("  wifi connect <ssid> [password] - Connect to Wi-Fi AP");
        Serial.println("  wifi ip                        - Show current IP address");
        Serial.println();
        Serial.println("Content / Web Catalogue Commands:");
        Serial.println("  server <url>                   - Set/show base server URL");
        Serial.println("  content catalogue              - Fetch & print catalogue page 0");
        Serial.println("  content fetch <page>           - Fetch specific catalogue page");
        Serial.println("  content download <idx_or_id>   - Download media item into PSRAM & play");
        Serial.println("  playurl <v_url> <a_url>        - Download custom media URLs & play");
        Serial.println();
        Serial.println("Playback Commands:");
        Serial.println("  play                           - Play/Replay currently loaded PSRAM media");
        Serial.println("  stop                           - Stop playback");
        Serial.println("  pause                          - Pause playback");
        Serial.println("  resume                         - Resume playback");
        Serial.println();
        Serial.println("Bluetooth Commands:");
        Serial.println("  bt scan                        - Discover nearby BT Classic devices");
        Serial.println("  bt stop                        - Stop discovery");
        Serial.println("  bt list                        - List discovered devices");
        Serial.println("  bt connect N                   - Connect to device by index");
        Serial.println("  bt disconnect                  - Disconnect BT");
        Serial.println("  bt status                      - Show full BT status");
        Serial.println("  bt prefer list                 - Show preferred devices");
        Serial.println("  bt prefer add <name>           - Add to preferred list");
        Serial.println("  bt prefer clear                - Clear runtime additions");
        Serial.println("========================================");
        Serial.println();
        Serial.print("> ");
        return;
    }

    Serial.printf("Unknown command: '%s'\n", line);
    Serial.println("Type 'help' for available commands.");
    Serial.print("> ");
}
