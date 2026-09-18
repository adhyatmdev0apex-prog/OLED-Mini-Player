#ifndef PLAYBACK_MANAGER_H
#define PLAYBACK_MANAGER_H

#include <Arduino.h>
#include <LittleFS.h>
#include "../content/media_item.h"
#include "../content/media_downloader.h"
#include "../audio/bluetooth/bluetooth.h"
#include "../display/oled_display.h"
#include "../config/app_config.h"

enum class PlaybackState {
    STOPPED,
    PLAYING,
    PAUSED
};

class PlaybackManager {
public:
    static PlaybackManager& getInstance();

    void begin();
    
    // Sets and swaps new PSRAM media cleanly, freeing previous PSRAM buffers.
    bool setMedia(DownloadedMedia& newMedia);

    // Fallback: Loads local LittleFS media assets if present (/stickman.vs.geometry_dash.bin & .pcm)
    bool loadFallbackLittleFS();

    void playCurrent();
    void stop();
    void pause();
    void resume();
    void freeCurrentMedia();

    PlaybackState getState() const { return m_state; }
    bool isLoaded() const { return m_isLoaded; }
    String getCurrentMediaName() const { return m_currentMedia.item.name; }

    // Service loop step called inside application loop or wait loop
    void update();

private:
    PlaybackManager();
    ~PlaybackManager() = default;

    PlaybackManager(const PlaybackManager&) = delete;
    PlaybackManager& operator=(const PlaybackManager&) = delete;

    bool parseSTIKHeader(const uint8_t* data, size_t size);
    bool readNextFrame(const uint8_t* data, size_t size, size_t& offset);
    bool decodeRLE(const uint8_t* payload, size_t payloadLen, uint8_t* output);

    DownloadedMedia m_currentMedia;
    bool m_isLoaded;
    PlaybackState m_state;

    // STIK header metadata
    uint16_t m_width;
    uint16_t m_height;
    uint8_t  m_fps;
    uint32_t m_frameCount;

    // Buffer pointers allocated in PSRAM or Heap
    uint8_t* m_frameBuffer;
    uint8_t* m_deltaBuffer;
};

#endif // PLAYBACK_MANAGER_H
