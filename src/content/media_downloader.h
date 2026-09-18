#ifndef MEDIA_DOWNLOADER_H
#define MEDIA_DOWNLOADER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include "media_item.h"
#include "../config/app_config.h"

struct DownloadedMedia {
    uint8_t* videoBuffer = nullptr;
    size_t videoSize = 0;
    uint8_t* audioBuffer = nullptr;
    size_t audioSize = 0;
    MediaItem item;

    void freeBuffers() {
        if (videoBuffer != nullptr) {
            free(videoBuffer);
            videoBuffer = nullptr;
        }
        videoSize = 0;

        if (audioBuffer != nullptr) {
            free(audioBuffer);
            audioBuffer = nullptr;
        }
        audioSize = 0;
    }
};

class MediaDownloader {
public:
    MediaDownloader() = default;
    ~MediaDownloader() = default;

    // Downloads videoUrl and audioUrl directly into newly allocated PSRAM buffers.
    // Performs Content-Length checks, PSRAM safety checks, streaming downloads,
    // and STIK/PCM binary validations before returning true.
    bool downloadMedia(const MediaItem& item, DownloadedMedia& outResult);

    // Downloads video and audio directly to LittleFS flash without consuming PSRAM.
    bool downloadToFlash(const MediaItem& item, const String& videoFilePath, const String& audioFilePath,
                         size_t& outVideoSize, size_t& outAudioSize);

private:
    bool downloadToPSRAM(const String& url, uint8_t*& outBuffer, size_t& outSize, size_t maxSize, const char* label);
    bool downloadToFile(const String& url, const String& filePath, size_t maxSize, const char* label, size_t& outSize);
    bool validateSTIK(const uint8_t* buffer, size_t size);
    bool validatePCM(const uint8_t* buffer, size_t size);
    bool validateSTIKFile(const String& filePath);
};

#endif // MEDIA_DOWNLOADER_H
