#include "content_api.h"
#include <memory>
#include "../wifi/wifi_manager.h"
#include <WiFiClientSecure.h>
#include "offline_storage.h"

ContentAPI::ContentAPI()
    : m_baseUrl(MEDIA_SERVER_BASE_URL) {
    // Strip trailing slash if present
    while (m_baseUrl.endsWith("/")) {
        m_baseUrl.remove(m_baseUrl.length() - 1);
    }
}

void ContentAPI::setBaseUrl(const char* baseUrl) {
    if (baseUrl == nullptr || strlen(baseUrl) == 0) return;
    m_baseUrl = baseUrl;
    while (m_baseUrl.endsWith("/")) {
        m_baseUrl.remove(m_baseUrl.length() - 1);
    }
    Serial.printf("[API] Server base URL set to: %s\n", m_baseUrl.c_str());
}

String ContentAPI::buildAbsoluteUrl(const String& baseUrl, const String& path) {
    if (path.startsWith("http://") || path.startsWith("https://")) {
        return path;
    }
    
    String cleanBase = baseUrl;
    while (cleanBase.endsWith("/")) {
        cleanBase.remove(cleanBase.length() - 1);
    }
    
    String cleanPath = path;
    if (!cleanPath.startsWith("/")) {
        cleanPath = "/" + cleanPath;
    }
    
    return cleanBase + cleanPath;
}

bool ContentAPI::fetchCatalogue(uint32_t page, uint32_t limit, CataloguePage& outPage) {
    if (!WiFiManager::getInstance().isConnected()) {
        Serial.println("[API] Error: Cannot fetch catalogue, Wi-Fi not connected.");
        return false;
    }

    String apiUrl = m_baseUrl + "/api/videos?page=" + String(page) + "&limit=" + String(limit);
    Serial.printf("[API] GET %s\n", apiUrl.c_str());

    std::unique_ptr<WiFiClient> client;
    std::unique_ptr<WiFiClientSecure> secureClient;
    std::unique_ptr<HTTPClient> http(new HTTPClient());

    http->setTimeout(HTTP_TIMEOUT_MS);
    
    bool beginSuccess = false;
    
    Serial.printf("[MEM] Before HTTPS: heap=%u largest=%u PSRAM=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
    
    if (apiUrl.startsWith("https://")) {
        secureClient.reset(new WiFiClientSecure());
        secureClient->setInsecure();
        beginSuccess = http->begin(*secureClient, apiUrl);
    } else {
        client.reset(new WiFiClient());
        beginSuccess = http->begin(*client, apiUrl);
    }

    if (!beginSuccess) {
        Serial.println("[API] Error: Failed to initialize HTTP client connection.");
        http.reset();
        secureClient.reset();
        client.reset();
        return false;
    }

    http->setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http->addHeader("Accept", "application/json");
    http->addHeader("X-ESP-CLIENT", "ESP32-CAM");

    int httpCode = http->GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[API] HTTP GET failed, status code: %d (%s)\n",
                      httpCode, http->errorToString(httpCode).c_str());
        http->end();
        http.reset();
        secureClient.reset();
        client.reset();
        return false;
    }

    Serial.println("[API] HTTP 200 OK received");

    // Use heap allocation for large JSON document
    DynamicJsonDocument doc(4096);
    
    String payload = http->getString();
    Serial.println("[API] Raw Response:");
    Serial.println(payload);
    
    DeserializationError error = deserializeJson(doc, payload);

    http->end();
    http.reset();
    secureClient.reset();
    client.reset();
    
    Serial.printf("[MEM] After HTTPS: heap=%u largest=%u PSRAM=%u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());

    if (error) {
        Serial.printf("[API] JSON parsing failed: %s\n", error.c_str());

        return false;
    }

    if (!doc.containsKey("page") || !doc.containsKey("limit") || !doc.containsKey("total") || !doc.containsKey("items")) {
        Serial.println("[API] Error: Invalid JSON schema (missing required fields).");

        return false;
    }

    outPage.page = doc["page"].as<uint32_t>();
    outPage.limit = doc["limit"].as<uint32_t>();
    outPage.total = doc["total"].as<uint32_t>();
    outPage.items.clear();

    JsonArray itemsArray = doc["items"].as<JsonArray>();
    Serial.printf("[API] Received %u items (Total: %u, Page: %u)\n",
                  (unsigned)itemsArray.size(), outPage.total, outPage.page);

    for (JsonObject itemObj : itemsArray) {
        MediaItem item;
        item.id = itemObj["id"] | "";
        item.name = itemObj["name"] | "";
        String rawVideoUrl = itemObj["video_url"] | "";
        String rawAudioUrl = itemObj["audio_url"] | "";

        if (item.id.length() == 0 || item.name.length() == 0 || rawVideoUrl.length() == 0 || rawAudioUrl.length() == 0) {
            Serial.printf("[API] Skipping invalid item (missing required fields)\n");
            continue;
        }

        // CRITICAL RULE: Construct absolute URLs using the server-provided relative/absolute paths
        item.videoUrl = buildAbsoluteUrl(m_baseUrl, rawVideoUrl);
        item.audioUrl = buildAbsoluteUrl(m_baseUrl, rawAudioUrl);

        // Parse offline / SPIFFS qualification
        // C. Item marked offline_download=true appears as downloadable
        // D. Item marked false cannot be downloaded offline
        bool isOffline = false;
        if (itemObj.containsKey("offline_download")) {
            isOffline = itemObj["offline_download"].as<bool>();
        } else if (itemObj.containsKey("metadata") && itemObj["metadata"].is<JsonObject>() && itemObj["metadata"].containsKey("offline_download")) {
            isOffline = itemObj["metadata"]["offline_download"].as<bool>();
        } else if (itemObj.containsKey("metadata") && itemObj["metadata"].is<JsonObject>() && itemObj["metadata"].containsKey("spiffs_compatible")) {
            isOffline = itemObj["metadata"]["spiffs_compatible"].as<bool>();
        }

        if (itemObj.containsKey("sizes") && itemObj["sizes"].is<JsonObject>()) {
            size_t totalBytes = itemObj["sizes"]["total"] | 0;
            item.totalSize = totalBytes;
            // Verify size safety margin on LittleFS partition
            if (totalBytes > 0 && (totalBytes + 4096) > OFFLINE_STORAGE_LIMIT_BYTES) {
                isOffline = false;
            }
        }

        item.isSpiffsCompatible = isOffline;
        item.isOfflineStored = OfflineStorage::getInstance().isItemStored(item.id);

        outPage.items.push_back(item);
        Serial.printf("  - [%s] %s (OfflineComp: %s, Stored: %s)\n    Video: %s\n    Audio: %s\n",
                      item.id.c_str(), item.name.c_str(),
                      item.isSpiffsCompatible ? "YES" : "NO",
                      item.isOfflineStored ? "SAVED" : "NO",
                      item.videoUrl.c_str(), item.audioUrl.c_str());
    }

    return true;
}
