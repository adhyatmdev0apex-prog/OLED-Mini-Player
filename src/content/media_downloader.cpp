#include "media_downloader.h"
#include <memory>
#include "../wifi/wifi_manager.h"
#include "../display/oled_display.h"
#include "../audio/bluetooth/bluetooth.h"
#include <WiFiClientSecure.h>
#include <LittleFS.h>

bool MediaDownloader::downloadToPSRAM(const String& url, uint8_t*& outBuffer, size_t& outSize, size_t maxSize, const char* label) {
    if (!WiFiManager::getInstance().isConnected()) {
        Serial.printf("[DOWNLOAD] %s Error: Wi-Fi not connected.\n", label);
        return false;
    }

    std::unique_ptr<WiFiClient> client;
    std::unique_ptr<WiFiClientSecure> secureClient;
    std::unique_ptr<HTTPClient> http(new HTTPClient());
#define CLEANUP_HTTP() do { if(http) { http->end(); http.reset(); } secureClient.reset(); client.reset(); } while(0)

    http->setTimeout(DOWNLOAD_TIMEOUT_MS);
    bool beginSuccess = false;
    if (url.startsWith("https://")) {
        secureClient.reset(new WiFiClientSecure());
        secureClient->setInsecure();
        beginSuccess = http->begin(*secureClient, url);
    } else {
        client.reset(new WiFiClient());
        beginSuccess = http->begin(*client, url);
    }

    if (!beginSuccess) {
        Serial.printf("[DOWNLOAD] %s Error: Failed to initialize HTTP connection.\n", label);
        CLEANUP_HTTP();
        return false;
    }

    http->setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http->addHeader("X-ESP-CLIENT", "ESP32-CAM");

    const char* headerKeys[] = {"Content-Type"};
    http->collectHeaders(headerKeys, 1);

    int httpCode = http->GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[DOWNLOAD] %s Error: HTTP GET failed for %s, status: %d (%s)\n",
                      label, url.c_str(), httpCode, http->errorToString(httpCode).c_str());
        CLEANUP_HTTP();
        return false;
    }

    int contentLength = http->getSize();
    if (contentLength <= 0) {
        Serial.printf("[DOWNLOAD] %s Error: Invalid or missing Content-Length (%d).\n", label, contentLength);
        CLEANUP_HTTP();
        return false;
    }

    size_t requiredSize = (size_t)contentLength;
    Serial.printf("[DOWNLOAD] URL: %s\n", url.c_str());
    Serial.printf("[DOWNLOAD] HTTP status: %d\n", httpCode);
    Serial.printf("[DOWNLOAD] Content-Type: %s\n", http->hasHeader("Content-Type") ? http->header("Content-Type").c_str() : "unknown");
    Serial.printf("[DOWNLOAD] Content-Length: %d\n", contentLength);
    Serial.printf("[DOWNLOAD] Free heap before: %u\n", ESP.getFreeHeap());
    Serial.printf("[DOWNLOAD] Largest heap block before: %u\n", ESP.getMaxAllocHeap());
    Serial.printf("[DOWNLOAD] Free PSRAM before: %u\n", ESP.getFreePsram());

    if (requiredSize > maxSize) {
        Serial.printf("[DOWNLOAD] %s REJECTED: Size %u exceeds max limit %u bytes.\n",
                      label, (unsigned)requiredSize, (unsigned)maxSize);
        CLEANUP_HTTP();
        return false;
    }

    if (!psramFound()) {
        Serial.println("[PSRAM] REJECTED: PSRAM not found!");
        CLEANUP_HTTP();
        return false;
    }

    size_t freePsram = ESP.getFreePsram();
    Serial.printf("[PSRAM] Free before %s allocation: %u bytes\n", label, (unsigned)freePsram);

    if (freePsram < requiredSize + PSRAM_SAFETY_MARGIN_BYTES) {
        Serial.printf("[PSRAM] REJECTED: Insufficient PSRAM for %s (Required: %u bytes, Free: %u bytes)\n",
                      label, (unsigned)(requiredSize + PSRAM_SAFETY_MARGIN_BYTES), (unsigned)freePsram);
        CLEANUP_HTTP();
        return false;
    }

    outBuffer = (uint8_t*)ps_malloc(requiredSize);
    if (!outBuffer) {
        Serial.printf("[PSRAM] Error: Failed to allocate %u bytes in PSRAM for %s.\n", (unsigned)requiredSize, label);
        CLEANUP_HTTP();
        return false;
    }

    Serial.printf("[DOWNLOAD] Downloading %s into PSRAM...\n", label);
    OLEDDisplay::getInstance().showDownloadProgress(label, 0, requiredSize);

    WiFiClient* stream = http->getStreamPtr();
    size_t bytesReadTotal = 0;
    uint32_t startMs = millis();
    uint32_t lastOledMs = 0;
    const size_t chunkSize = 4096;

    while (http->connected() && (bytesReadTotal < requiredSize)) {
        size_t availableBytes = stream->available();
        if (availableBytes > 0) {
            size_t toRead = std::min(availableBytes, std::min(chunkSize, requiredSize - bytesReadTotal));
            int readCount = stream->read(outBuffer + bytesReadTotal, toRead);
            if (readCount > 0) {
                bytesReadTotal += readCount;
                startMs = millis();

                // Smoothly update OLED progress bar every 120ms
                if ((millis() - lastOledMs >= 120) || (bytesReadTotal >= requiredSize)) {
                    lastOledMs = millis();
                    OLEDDisplay::getInstance().showDownloadProgress(label, bytesReadTotal, requiredSize);
                }
            } else if (readCount < 0) {
                Serial.printf("[DOWNLOAD] %s Error: Socket read error.\n", label);
                break;
            }
        } else {
            delay(1);
            if (millis() - startMs > DOWNLOAD_TIMEOUT_MS) {
                Serial.printf("[DOWNLOAD] %s Error: Stream read timeout.\n", label);
                break;
            }
        }
    }

    CLEANUP_HTTP();

    if (bytesReadTotal != requiredSize) {
        Serial.printf("[DOWNLOAD] %s Error: Short read! Expected %u bytes, received %u bytes.\n",
                      label, (unsigned)requiredSize, (unsigned)bytesReadTotal);
        free(outBuffer);
        outBuffer = nullptr;
        return false;
    }

    outSize = requiredSize;
    Serial.printf("[DOWNLOAD] Received: %u / %u\n", (unsigned)bytesReadTotal, (unsigned)requiredSize);
    
    // Explicitly clean up HTTP resources
    CLEANUP_HTTP();
    secureClient.reset();
    client.reset();

    Serial.printf("[DOWNLOAD] Free PSRAM after: %u\n", ESP.getFreePsram());
    Serial.printf("[DOWNLOAD] COMPLETE\n");
    return true;
}

bool MediaDownloader::validateSTIK(const uint8_t* buffer, size_t size) {
    if (buffer == nullptr || size < 16) {
        Serial.println("[VALIDATE] STIK FAILED: File size less than 16 bytes.");
        return false;
    }

    // Magic: "STIK"
    if (buffer[0] != 'S' || buffer[1] != 'T' || buffer[2] != 'I' || buffer[3] != 'K') {
        Serial.println("[VALIDATE] STIK FAILED: Invalid magic signature.");
        return false;
    }

    uint16_t width = (uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8);
    uint16_t height = (uint16_t)buffer[6] | ((uint16_t)buffer[7] << 8);
    uint8_t fps = buffer[8];
    uint32_t frameCount = (uint32_t)buffer[9] | ((uint32_t)buffer[10] << 8) |
                          ((uint32_t)buffer[11] << 16) | ((uint32_t)buffer[12] << 24);

    Serial.printf("[VALIDATE] STIK Header: %ux%u @ %u FPS, %u frames, Size: %u bytes\n",
                  width, height, fps, frameCount, (unsigned)size);

    if (width != 128 || height != 64) {
        Serial.printf("[VALIDATE] STIK FAILED: Unsupported resolution %ux%u (expected 128x64).\n", width, height);
        return false;
    }

    if (fps == 0) {
        Serial.println("[VALIDATE] STIK FAILED: FPS is 0.");
        return false;
    }

    if (frameCount == 0) {
        Serial.println("[VALIDATE] STIK FAILED: Frame count is 0.");
        return false;
    }

    Serial.println("[VALIDATE] STIK OK");
    return true;
}

bool MediaDownloader::validatePCM(const uint8_t* buffer, size_t size) {
    if (buffer == nullptr || size == 0) {
        Serial.println("[VALIDATE] PCM FAILED: Audio buffer is empty.");
        return false;
    }

    Serial.printf("[VALIDATE] PCM Audio OK: %u bytes\n", (unsigned)size);
    return true;
}

bool MediaDownloader::downloadMedia(const MediaItem& item, DownloadedMedia& outResult) {
    if (!item.isValid()) {
        Serial.println("[DOWNLOAD] Error: Invalid MediaItem details.");
        return false;
    }

    bool wasBtActive = (bluetoothAudioGetState() != BluetoothState::OFF);
    if (wasBtActive) {
        Serial.println("[DOWNLOAD] Pausing Bluetooth stack to maximize heap for HTTPS TLS...");
        bluetoothAudioEnd();
        delay(60);
    }

    struct BtRestorer {
        bool active;
        ~BtRestorer() {
            if (active) {
                Serial.println("[DOWNLOAD] Restoring Bluetooth stack for playback...");
                bluetoothAudioBegin();
            }
        }
    } btRestorer{wasBtActive};

    Serial.println();
    Serial.println("========================================");
    Serial.printf("[CONTENT] Selected Item:\n  ID:   %s\n  Name: %s\n", item.id.c_str(), item.name.c_str());
    Serial.println("========================================");

    DownloadedMedia tempMedia;
    tempMedia.item = item;

    // 1. Download video directly into PSRAM in a single clean stream
    if (!downloadToPSRAM(item.videoUrl, tempMedia.videoBuffer, tempMedia.videoSize, MAX_VIDEO_SIZE, "Video")) {
        Serial.println("[DOWNLOAD] Video download failed.");
        tempMedia.freeBuffers();
        return false;
    }

    // 2. Validate STIK header immediately
    if (!validateSTIK(tempMedia.videoBuffer, tempMedia.videoSize)) {
        Serial.println("[CONTENT] STIK validation FAILED");
        tempMedia.freeBuffers();
        return false;
    }

    // 3. Download audio directly into PSRAM in a single clean stream
    if (!downloadToPSRAM(item.audioUrl, tempMedia.audioBuffer, tempMedia.audioSize, MAX_AUDIO_SIZE, "Audio")) {
        Serial.println("[DOWNLOAD] Audio download failed.");
        tempMedia.freeBuffers();
        return false;
    }

    // 4. Validate PCM audio immediately
    if (!validatePCM(tempMedia.audioBuffer, tempMedia.audioSize)) {
        Serial.println("[CONTENT] PCM validation FAILED");
        tempMedia.freeBuffers();
        return false;
    }

    // 5. Verify combined media size limit
    size_t combinedSize = tempMedia.videoSize + tempMedia.audioSize;
    Serial.printf("[DOWNLOAD] Combined media size: %u bytes\n", (unsigned)combinedSize);

    if (combinedSize > MAX_COMBINED_MEDIA_SIZE) {
        Serial.printf("[DOWNLOAD] REJECTED: Combined size %u exceeds MAX_COMBINED_MEDIA_SIZE %u\n",
                      (unsigned)combinedSize, (unsigned)MAX_COMBINED_MEDIA_SIZE);
        tempMedia.freeBuffers();
        return false;
    }

    // Transfer ownership on clean success
    outResult = tempMedia;
    Serial.println("[DOWNLOAD] Media downloaded and validated successfully!");
    return true;
}

bool MediaDownloader::downloadToFile(const String& url, const String& filePath, size_t maxSize, const char* label, size_t& outSize) {
    if (!WiFiManager::getInstance().isConnected()) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Wi-Fi not connected.\n", label);
        return false;
    }

    std::unique_ptr<WiFiClient> client;
    std::unique_ptr<WiFiClientSecure> secureClient;
    std::unique_ptr<HTTPClient> http(new HTTPClient());
#define CLEANUP_FLASH_HTTP() do { if(http) { http->end(); http.reset(); } secureClient.reset(); client.reset(); } while(0)

    http->setTimeout(DOWNLOAD_TIMEOUT_MS);
    bool beginSuccess = false;
    if (url.startsWith("https://")) {
        secureClient.reset(new WiFiClientSecure());
        secureClient->setInsecure();
        beginSuccess = http->begin(*secureClient, url);
    } else {
        client.reset(new WiFiClient());
        beginSuccess = http->begin(*client, url);
    }

    if (!beginSuccess) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Failed to initialize HTTP connection.\n", label);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    http->setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    http->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http->addHeader("X-ESP-CLIENT", "ESP32-CAM");

    const char* headerKeys[] = {"Content-Type"};
    http->collectHeaders(headerKeys, 1);

    int httpCode = http->GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: HTTP GET failed for %s, status: %d (%s)\n",
                      label, url.c_str(), httpCode, http->errorToString(httpCode).c_str());
        CLEANUP_FLASH_HTTP();
        return false;
    }

    int contentLength = http->getSize();
    if (contentLength <= 0) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Invalid or missing Content-Length (%d).\n", label, contentLength);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    size_t requiredSize = (size_t)contentLength;
    Serial.printf("[DOWNLOAD-FLASH] %s URL: %s\n", label, url.c_str());
    Serial.printf("[DOWNLOAD-FLASH] %s size: %u bytes\n", label, (unsigned)requiredSize);

    if (requiredSize > maxSize) {
        Serial.printf("[DOWNLOAD-FLASH] %s REJECTED: Size %u exceeds max limit %u bytes.\n",
                      label, (unsigned)requiredSize, (unsigned)maxSize);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    size_t freeFlash = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeFlash < requiredSize + 4096) {
        Serial.printf("[DOWNLOAD-FLASH] REJECTED: Insufficient flash space (Need: %u, Free: %u bytes)\n",
                      (unsigned)(requiredSize + 4096), (unsigned)freeFlash);
        CLEANUP_FLASH_HTTP();
        return false;
    }

    if (LittleFS.exists(filePath)) {
        LittleFS.remove(filePath);
    }

    File f = LittleFS.open(filePath, FILE_WRITE);
    if (!f) {
        Serial.printf("[DOWNLOAD-FLASH] Failed to create file '%s' on LittleFS.\n", filePath.c_str());
        CLEANUP_FLASH_HTTP();
        return false;
    }

    Serial.printf("[DOWNLOAD-FLASH] Streaming %s into LittleFS '%s'...\n", label, filePath.c_str());
    OLEDDisplay::getInstance().showDownloadProgress(label, 0, requiredSize);

    WiFiClient* stream = http->getStreamPtr();
    size_t bytesReadTotal = 0;
    uint32_t startMs = millis();
    uint32_t lastOledMs = 0;
    const size_t chunkSize = 2048;
    uint8_t chunkBuf[2048];

    while (http->connected() && (bytesReadTotal < requiredSize)) {
        size_t availableBytes = stream->available();
        if (availableBytes > 0) {
            size_t toRead = std::min(availableBytes, std::min(chunkSize, requiredSize - bytesReadTotal));
            int readCount = stream->read(chunkBuf, toRead);
            if (readCount > 0) {
                f.write(chunkBuf, readCount);
                bytesReadTotal += readCount;
                startMs = millis();

                if ((millis() - lastOledMs >= 120) || (bytesReadTotal >= requiredSize)) {
                    lastOledMs = millis();
                    OLEDDisplay::getInstance().showDownloadProgress(label, bytesReadTotal, requiredSize);
                }
            } else if (readCount < 0) {
                Serial.printf("[DOWNLOAD-FLASH] %s Error: Socket read error.\n", label);
                break;
            }
        } else {
            delay(1);
            if (millis() - startMs > DOWNLOAD_TIMEOUT_MS) {
                Serial.printf("[DOWNLOAD-FLASH] %s Error: Stream read timeout.\n", label);
                break;
            }
        }
    }

    f.flush();
    f.close();
    CLEANUP_FLASH_HTTP();
#undef CLEANUP_FLASH_HTTP

    if (bytesReadTotal != requiredSize) {
        Serial.printf("[DOWNLOAD-FLASH] %s Error: Short write! Expected %u, wrote %u bytes.\n",
                      label, (unsigned)requiredSize, (unsigned)bytesReadTotal);
        LittleFS.remove(filePath);
        return false;
    }

    outSize = requiredSize;
    Serial.printf("[DOWNLOAD-FLASH] %s complete (%u bytes written to %s)\n", label, (unsigned)outSize, filePath.c_str());
    return true;
}

bool MediaDownloader::validateSTIKFile(const String& filePath) {
    if (!LittleFS.exists(filePath)) return false;
    File f = LittleFS.open(filePath, FILE_READ);
    if (!f || f.size() < 16) {
        if (f) f.close();
        return false;
    }

    uint8_t header[16];
    size_t readCount = f.read(header, 16);
    f.close();

    if (readCount < 16) return false;
    return validateSTIK(header, readCount);
}

bool MediaDownloader::downloadToFlash(const MediaItem& item, const String& videoFilePath, const String& audioFilePath,
                                     size_t& outVideoSize, size_t& outAudioSize) {
    if (!item.isValid()) {
        Serial.println("[DOWNLOAD-FLASH] Error: Invalid MediaItem details.");
        return false;
    }

    bool wasBtActive = (bluetoothAudioGetState() != BluetoothState::OFF);
    if (wasBtActive) {
        Serial.println("[DOWNLOAD-FLASH] Pausing Bluetooth stack to maximize heap for Flash download...");
        bluetoothAudioEnd();
        delay(60);
    }

    struct BtRestorer {
        bool active;
        ~BtRestorer() {
            if (active) {
                Serial.println("[DOWNLOAD-FLASH] Restoring Bluetooth stack...");
                bluetoothAudioBegin();
            }
        }
    } btRestorer{wasBtActive};

    Serial.println();
    Serial.println("========================================");
    Serial.printf("[DOWNLOAD-FLASH] Storing item to Flash:\n  ID:   %s\n  Name: %s\n", item.id.c_str(), item.name.c_str());
    Serial.println("========================================");

    String tmpVideoPath = videoFilePath + ".tmp";
    String tmpAudioPath = audioFilePath + ".tmp";

    // Clean up any stale temp files first
    if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
    if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);

    // 1. Download video directly to temporary flash file
    size_t vSize = 0;
    if (!downloadToFile(item.videoUrl, tmpVideoPath, OFFLINE_STORAGE_LIMIT_BYTES, "Flash Video", vSize)) {
        Serial.println("[DOWNLOAD-FLASH] Video file download to flash failed.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        return false;
    }

    // 2. Validate STIK header of the temp file on flash
    if (!validateSTIKFile(tmpVideoPath)) {
        Serial.println("[DOWNLOAD-FLASH] Video STIK header validation failed!");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        return false;
    }

    // 3. Download audio directly to temporary flash file
    size_t aSize = 0;
    if (!downloadToFile(item.audioUrl, tmpAudioPath, OFFLINE_STORAGE_LIMIT_BYTES, "Flash Audio", aSize)) {
        Serial.println("[DOWNLOAD-FLASH] Audio file download to flash failed.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    // 4. Validate audio size
    if (aSize == 0) {
        Serial.println("[DOWNLOAD-FLASH] Audio file is empty!");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    // 5. Both files completely downloaded and validated! Atomically replace final files.
    if (LittleFS.exists(videoFilePath)) LittleFS.remove(videoFilePath);
    if (!LittleFS.rename(tmpVideoPath, videoFilePath)) {
        Serial.println("[DOWNLOAD-FLASH] Error: Failed to rename temp video file.");
        if (LittleFS.exists(tmpVideoPath)) LittleFS.remove(tmpVideoPath);
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    if (LittleFS.exists(audioFilePath)) LittleFS.remove(audioFilePath);
    if (!LittleFS.rename(tmpAudioPath, audioFilePath)) {
        Serial.println("[DOWNLOAD-FLASH] Error: Failed to rename temp audio file.");
        if (LittleFS.exists(tmpAudioPath)) LittleFS.remove(tmpAudioPath);
        return false;
    }

    outVideoSize = vSize;
    outAudioSize = aSize;
    Serial.printf("[DOWNLOAD-FLASH] Item '%s' saved to LittleFS! Total: %u bytes\n",
                  item.name.c_str(), (unsigned)(vSize + aSize));
    return true;
}
