#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include "../config/app_config.h"

enum class WiFiState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    FAILED
};

struct WiFiNetworkProfile {
    String ssid;
    String password;
    const char* label;
};

class WiFiManager {
public:
    static WiFiManager& getInstance();

    void begin();
    void update();
    
    bool connectProfile(size_t profileIndex);
    bool connect(const char* ssid, const char* password);
    void disconnect();
    
    bool isConnected() const;
    bool hasExhaustedAllNetworks() const { return m_exhaustedAll; }
    void resetRetryCycle();
    WiFiState getState() const { return m_state; }
    
    String getIP() const;
    String getSSID() const;
    int32_t getRSSI() const;
    const char* getStatusString() const;
    size_t getCurrentProfileIndex() const { return m_currentProfile; }
    const char* getCurrentProfileLabel() const;

private:
    WiFiManager();
    ~WiFiManager() = default;

    WiFiManager(const WiFiManager&) = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;

    WiFiNetworkProfile m_profiles[2];
    size_t m_currentProfile;
    bool m_exhaustedAll;

    WiFiState m_state;
    uint32_t m_connectStartMs;
    uint32_t m_lastReconnectAttemptMs;
};

#endif // WIFI_MANAGER_H
