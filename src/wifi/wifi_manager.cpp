#include "wifi_manager.h"

WiFiManager& WiFiManager::getInstance() {
    static WiFiManager instance;
    return instance;
}

WiFiManager::WiFiManager()
    : m_currentProfile(0)
    , m_exhaustedAll(false)
    , m_state(WiFiState::DISCONNECTED)
    , m_connectStartMs(0)
    , m_lastReconnectAttemptMs(0) {
    m_profiles[0] = { WIFI_SSID, WIFI_PASSWORD, "Primary" };
    m_profiles[1] = { WIFI_FALLBACK_SSID, WIFI_FALLBACK_PASSWORD, "Fallback" };
}

void WiFiManager::begin() {
    m_currentProfile = 0;
    m_exhaustedAll = false;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    connectProfile(0);
}

bool WiFiManager::connectProfile(size_t profileIndex) {
    if (profileIndex >= 2) return false;
    m_currentProfile = profileIndex;

    const auto& prof = m_profiles[m_currentProfile];
    if (prof.ssid.length() == 0 || prof.ssid == "Your_WiFi_SSID") {
        Serial.printf("[WIFI] %s Wi-Fi not configured. Skipping.\n", prof.label);
        return false;
    }

    Serial.printf("[WIFI] Connecting to %s ('%s')...\n", prof.label, prof.ssid.c_str());
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(prof.ssid.c_str(), prof.password.c_str());

    m_state = WiFiState::CONNECTING;
    m_connectStartMs = millis();
    return true;
}

bool WiFiManager::connect(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0) return false;
    m_profiles[0].ssid = ssid;
    m_profiles[0].password = password ? password : "";
    m_profiles[0].label = "Custom";
    return connectProfile(0);
}

void WiFiManager::disconnect() {
    Serial.println("[WIFI] Disconnecting...");
    WiFi.disconnect(true);
    m_state = WiFiState::DISCONNECTED;
}

void WiFiManager::resetRetryCycle() {
    m_currentProfile = 0;
    m_exhaustedAll = false;
    connectProfile(0);
}

void WiFiManager::update() {
    wl_status_t status = WiFi.status();

    switch (m_state) {
        case WiFiState::CONNECTING:
            if (status == WL_CONNECTED) {
                m_state = WiFiState::CONNECTED;
                m_exhaustedAll = false;
                Serial.printf("[WIFI] Connected via %s: %s (IP: %s, RSSI: %d dBm)\n",
                              m_profiles[m_currentProfile].label,
                              m_profiles[m_currentProfile].ssid.c_str(),
                              WiFi.localIP().toString().c_str(), WiFi.RSSI());
            } else if (millis() - m_connectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
                Serial.printf("[WIFI] Connection timeout on %s ('%s').\n",
                              m_profiles[m_currentProfile].label,
                              m_profiles[m_currentProfile].ssid.c_str());

                // Try next profile if available
                if (m_currentProfile == 0 && m_profiles[1].ssid.length() > 0 && m_profiles[1].ssid != "Your_WiFi_SSID") {
                    Serial.println("[WIFI] Switching to Fallback Wi-Fi...");
                    connectProfile(1);
                } else {
                    m_state = WiFiState::FAILED;
                    m_exhaustedAll = true;
                    m_lastReconnectAttemptMs = millis();
                    Serial.println("[WIFI] All Wi-Fi profiles exhausted. Operating in OFFLINE mode.");
                }
            }
            break;

        case WiFiState::CONNECTED:
            if (status != WL_CONNECTED) {
                m_state = WiFiState::DISCONNECTED;
                m_lastReconnectAttemptMs = millis();
                Serial.println("[WIFI] Wi-Fi link dropped.");
            }
            break;

        case WiFiState::DISCONNECTED:
        case WiFiState::FAILED:
            if (status == WL_CONNECTED) {
                m_state = WiFiState::CONNECTED;
                m_exhaustedAll = false;
                Serial.printf("[WIFI] Reconnected: %s (RSSI: %d dBm)\n",
                              WiFi.localIP().toString().c_str(), WiFi.RSSI());
            } else if (millis() - m_lastReconnectAttemptMs > WIFI_RECONNECT_INTERVAL_MS) {
                Serial.println("[WIFI] Periodic reconnect attempt...");
                m_currentProfile = 0;
                connectProfile(0);
            }
            break;
    }
}

bool WiFiManager::isConnected() const {
    return m_state == WiFiState::CONNECTED && (WiFi.status() == WL_CONNECTED);
}

String WiFiManager::getIP() const {
    if (isConnected()) {
        return WiFi.localIP().toString();
    }
    return "0.0.0.0";
}

String WiFiManager::getSSID() const {
    return m_profiles[m_currentProfile].ssid;
}

int32_t WiFiManager::getRSSI() const {
    if (isConnected()) {
        return WiFi.RSSI();
    }
    return 0;
}

const char* WiFiManager::getCurrentProfileLabel() const {
    return m_profiles[m_currentProfile].label;
}

const char* WiFiManager::getStatusString() const {
    switch (m_state) {
        case WiFiState::DISCONNECTED: return "Disconnected";
        case WiFiState::CONNECTING:   return "Connecting...";
        case WiFiState::CONNECTED:    return "Connected";
        case WiFiState::FAILED:       return "Connection Failed";
        default:                      return "Unknown";
    }
}
