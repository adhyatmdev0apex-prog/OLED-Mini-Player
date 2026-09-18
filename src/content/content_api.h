#ifndef CONTENT_API_H
#define CONTENT_API_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "media_item.h"
#include "../config/app_config.h"

class ContentAPI {
public:
    ContentAPI();
    ~ContentAPI() = default;

    void setBaseUrl(const char* baseUrl);
    String getBaseUrl() const { return m_baseUrl; }

    bool fetchCatalogue(uint32_t page, uint32_t limit, CataloguePage& outPage);

    // Helper: Combines base URL with relative path, respecting exact server-provided paths
    static String buildAbsoluteUrl(const String& baseUrl, const String& path);

private:
    String m_baseUrl;
};

#endif // CONTENT_API_H
