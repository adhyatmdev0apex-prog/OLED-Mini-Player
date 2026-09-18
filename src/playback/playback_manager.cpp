#include "playback_manager.h"
#include "../transport/command_protocol.h"
#include "../input/input_manager.h"

struct BluetoothPlaybackSnapshot {
    bool isConnected;
    char deviceName[64];
};

static void captureBTSnapshot(BluetoothPlaybackSnapshot* s) {
    s->isConnected = bluetoothAudioConnected();
    const char* dev = bluetoothAudioConnectedDeviceName();
    strncpy(s->deviceName, dev ? dev : "", sizeof(s->deviceName) - 1);
    s->deviceName[sizeof(s->deviceName) - 1] = '\0';
}

static bool btSnapshotChanged(const BluetoothPlaybackSnapshot& s) {
    bool nowConnected = bluetoothAudioConnected();
    if (nowConnected != s.isConnected) return true;
    if (nowConnected) {
        const char* dev = bluetoothAudioConnectedDeviceName();
        return strcmp(s.deviceName, dev ? dev : "") != 0;
    }
    return false;
}

PlaybackManager& PlaybackManager::getInstance() {
    static PlaybackManager instance;
    return instance;
}

PlaybackManager::PlaybackManager()
    : m_isLoaded(false)
    , m_state(PlaybackState::STOPPED)
    , m_width(128)
    , m_height(64)
    , m_fps(29)
    , m_frameCount(0)
    , m_frameBuffer(nullptr)
    , m_deltaBuffer(nullptr) {
}

void PlaybackManager::begin() {
    if (psramFound()) {
        m_frameBuffer = (uint8_t*)ps_malloc(1024);
        m_deltaBuffer = (uint8_t*)ps_malloc(1024);
    } else {
        m_frameBuffer = (uint8_t*)malloc(1024);
        m_deltaBuffer = (uint8_t*)malloc(1024);
    }

    if (!m_frameBuffer || !m_deltaBuffer) {
        Serial.println("[PLAYBACK] Error: Frame buffer allocation failed!");
    } else {
        memset(m_frameBuffer, 0, 1024);
        memset(m_deltaBuffer, 0, 1024);
    }
}

bool PlaybackManager::setMedia(DownloadedMedia& newMedia) {
    if (newMedia.videoBuffer == nullptr || newMedia.videoSize == 0) {
        Serial.println("[PLAYBACK] Error: Cannot set empty video buffer.");
        return false;
    }

    // Validate STIK header
    if (!parseSTIKHeader(newMedia.videoBuffer, newMedia.videoSize)) {
        Serial.println("[PLAYBACK] Error: Failed to parse STIK header.");
        return false;
    }

    // Stop playback if currently active
    stop();

    // Free previously loaded media buffers in PSRAM
    m_currentMedia.freeBuffers();

    // Swap ownership of new PSRAM media
    m_currentMedia = newMedia;
    newMedia.videoBuffer = nullptr;
    newMedia.audioBuffer = nullptr;
    newMedia.videoSize = 0;
    newMedia.audioSize = 0;

    // Attach audio PCM buffer to Bluetooth Classic A2DP source
    if (m_currentMedia.audioBuffer && m_currentMedia.audioSize > 0) {
        bluetoothAudioPause(); // Ensure audio is paused until playCurrent() starts
        bluetoothAudioSetPCMBuffer(m_currentMedia.audioBuffer, m_currentMedia.audioSize, AUDIO_SAMPLE_RATE);
    } else {
        bluetoothAudioSetPCMBuffer(nullptr, 0, AUDIO_SAMPLE_RATE);
    }

    m_isLoaded = true;
    Serial.printf("[PLAYBACK] Media '%s' loaded into PSRAM! (Video: %u bytes, Audio: %u bytes)\n",
                  m_currentMedia.item.name.c_str(), (unsigned)m_currentMedia.videoSize, (unsigned)m_currentMedia.audioSize);
    return true;
}

bool PlaybackManager::loadFallbackLittleFS() {
    if (!LittleFS.begin(false)) {
        Serial.println("[PLAYBACK] LittleFS not available for fallback.");
        return false;
    }

    if (!LittleFS.exists(VIDEO_FILE)) {
        Serial.println("[PLAYBACK] Fallback video file not found in LittleFS.");
        return false;
    }

    File vid = LittleFS.open(VIDEO_FILE, FILE_READ);
    if (!vid) return false;
    size_t vidSize = vid.size();

    File aud;
    size_t audSize = 0;
    if (LittleFS.exists(AUDIO_FILE)) {
        aud = LittleFS.open(AUDIO_FILE, FILE_READ);
        if (aud) audSize = aud.size();
    }

    if (!psramFound()) {
        Serial.println("[PLAYBACK] PSRAM missing, cannot load fallback.");
        vid.close();
        if (aud) aud.close();
        return false;
    }

    DownloadedMedia fallback;
    fallback.item.id = "local_fallback";
    fallback.item.name = "Local LittleFS Fallback";
    fallback.item.videoUrl = VIDEO_FILE;
    fallback.item.audioUrl = AUDIO_FILE;

    fallback.videoSize = vidSize;
    fallback.videoBuffer = (uint8_t*)ps_malloc(vidSize);
    if (!fallback.videoBuffer) {
        vid.close();
        if (aud) aud.close();
        return false;
    }
    vid.read(fallback.videoBuffer, vidSize);
    vid.close();

    if (audSize > 0) {
        fallback.audioSize = audSize;
        fallback.audioBuffer = (uint8_t*)ps_malloc(audSize);
        if (fallback.audioBuffer) {
            aud.read(fallback.audioBuffer, audSize);
        }
        aud.close();
    }

    return setMedia(fallback);
}

bool PlaybackManager::parseSTIKHeader(const uint8_t* data, size_t size) {
    if (!data || size < 16) return false;
    if (data[0] != 'S' || data[1] != 'T' || data[2] != 'I' || data[3] != 'K') return false;

    m_width = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    m_height = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    m_fps = data[8];
    m_frameCount = (uint32_t)data[9] | ((uint32_t)data[10] << 8) |
                   ((uint32_t)data[11] << 16) | ((uint32_t)data[12] << 24);

    return m_width == 128 && m_height == 64 && m_fps > 0 && m_frameCount > 0;
}

bool PlaybackManager::decodeRLE(const uint8_t* payload, size_t payloadLen, uint8_t* output) {
    size_t inIdx = 0;
    size_t outIdx = 0;

    while (inIdx < payloadLen && outIdx < 1024) {
        uint8_t ctrl = payload[inIdx++];
        if ((ctrl & 0x80) == 0) {
            // ZERO RUN
            size_t count = (size_t)ctrl + 1;
            for (size_t k = 0; k < count && outIdx < 1024; k++) {
                output[outIdx++] = 0;
            }
        } else {
            // LITERAL RUN
            size_t count = (size_t)(ctrl & 0x7F) + 1;
            for (size_t k = 0; k < count && inIdx < payloadLen && outIdx < 1024; k++) {
                output[outIdx++] = payload[inIdx++];
            }
        }
    }
    return outIdx == 1024;
}

bool PlaybackManager::readNextFrame(const uint8_t* data, size_t size, size_t& offset) {
    if (!data || offset >= size) return false;

    uint8_t type = data[offset++];
    if (offset + 2 > size) return false;

    uint16_t payloadLen = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
    offset += 2;

    if (offset + payloadLen > size) return false;

    if (type == 0x01) {
        // RAW FRAME (1024 bytes)
        if (payloadLen != 1024) return false;
        memcpy(m_frameBuffer, data + offset, 1024);
        offset += 1024;
        return true;
    } else if (type == 0x02) {
        // XOR-RLE DELTA
        if (!decodeRLE(data + offset, payloadLen, m_deltaBuffer)) {
            return false;
        }
        offset += payloadLen;
        for (int i = 0; i < 1024; i++) {
            m_frameBuffer[i] ^= m_deltaBuffer[i];
        }
        return true;
    }

    return false;
}

void PlaybackManager::stop() {
    m_state = PlaybackState::STOPPED;
    bluetoothAudioPause();
    bluetoothAudioSetTime(0);
}

void PlaybackManager::pause() {
    if (m_state == PlaybackState::PLAYING) {
        m_state = PlaybackState::PAUSED;
        bluetoothAudioPause();
    }
}

void PlaybackManager::resume() {
    if (m_state == PlaybackState::PAUSED) {
        m_state = PlaybackState::PLAYING;
        bluetoothAudioResume();
    }
}

void PlaybackManager::freeCurrentMedia() {
    stop();
    m_currentMedia.freeBuffers();
    m_isLoaded = false;
    bluetoothAudioSetPCMBuffer(nullptr, 0, AUDIO_SAMPLE_RATE);
    Serial.println("[PLAYBACK] Current media PSRAM buffers freed.");
}

void PlaybackManager::playCurrent() {
    if (!m_isLoaded || m_currentMedia.videoBuffer == nullptr) {
        Serial.println("[PLAYBACK] Cannot play: No media loaded in PSRAM.");
        return;
    }

    if (m_state == PlaybackState::PLAYING) {
        Serial.println("[PLAYBACK] Already playing. Use 'stop' or 'pause' first.");
        return;
    }

    m_state = PlaybackState::PLAYING;
    bluetoothAudioResume();
    OLEDDisplay::getInstance().showPlayingScreen(m_currentMedia.item.name.c_str());

    const uint32_t fps = (m_fps > 0 ? m_fps : 24);
    const uint32_t frameTimeUs = 1000000UL / fps;

restartActualAnimation:
    bluetoothAudioUpdate();
    BluetoothPlaybackSnapshot bluetoothAtStart;
    captureBTSnapshot(&bluetoothAtStart);

    size_t offset = 16; // Skip 16-byte STIK header
    memset(m_frameBuffer, 0, 1024);

    uint32_t playbackStartUs = micros();
    if (m_currentMedia.audioBuffer && m_currentMedia.audioSize > 0) {
        bluetoothAudioSetTime(0); // Snap audio position strictly to start
        bluetoothAudioResume();
    }

    uint32_t actualFrames = 0;

    for (uint32_t frame = 0; frame < m_frameCount; frame++) {
        if (m_state == PlaybackState::STOPPED) {
            Serial.println("[PLAYBACK] Playback stopped by user.");
            break;
        }

        if (m_state == PlaybackState::PAUSED) {
            bluetoothAudioPause();
            uint32_t pauseStartMs = millis();
            OLEDDisplay::getInstance().showPauseScreen(m_currentMedia.item.name.c_str());

            while (m_state == PlaybackState::PAUSED) {
                delay(20);
                bluetoothAudioUpdate();
                CommandProtocol::getInstance().pollSerial();

                InputEvent pEv = InputManager::getInstance().poll();
                if (pEv == InputEvent::SELECT_SHORT_PRESS) {
                    resume();
                    break;
                } else if (pEv == InputEvent::SELECT_LONG_PRESS) {
                    Serial.println("[INPUT] Long press while paused -> Stopping playback.");
                    stop();
                    break;
                }
            }

            if (m_state == PlaybackState::STOPPED) break;

            // Compensate frame clocks for the duration of the pause
            uint32_t pausedUs = (millis() - pauseStartMs) * 1000UL;
            playbackStartUs += pausedUs;
            bluetoothAudioResume();
        }

        // Bluetooth snapshot sync check (Requirement #21)
        bluetoothAudioUpdate();
        if (btSnapshotChanged(bluetoothAtStart)) {
            Serial.println("BT STATE/DEVICE CHANGED - RESTARTING ANIMATION");
            if (m_currentMedia.audioBuffer && m_currentMedia.audioSize > 0) {
                bluetoothAudioSetTime(0);
            }
            goto restartActualAnimation;
        }

        // Presentation timestamp of this video frame (PTS in milliseconds)
        uint32_t framePtsMs = (uint32_t)(((uint64_t)frame * 1000ULL) / fps);

        // Audio clock update: lock audio strictly to current video presentation timestamp
        if (m_currentMedia.audioBuffer && m_currentMedia.audioSize > 0) {
            bluetoothAudioSetTime(framePtsMs);
        }

        // Poll physical buttons before frame decode
        InputEvent preEv = InputManager::getInstance().poll();
        if (preEv == InputEvent::SELECT_SHORT_PRESS) {
            pause();
            continue;
        } else if (preEv == InputEvent::SELECT_LONG_PRESS) {
            Serial.println("[INPUT] Long press (hold 1s) -> Stopping playback.");
            stop();
            break;
        }

        // Read STIK frame
        if (!readNextFrame(m_currentMedia.videoBuffer, m_currentMedia.videoSize, offset)) {
            Serial.printf("\n[PLAYBACK] Reached end of video stream at frame %lu / %lu.\n",
                          (unsigned long)frame, (unsigned long)m_frameCount);
            break;
        }

        // FAST native 128x64 OLED rendering (13ms transfer, no 24-byte packet fragmentation)
        OLEDDisplay::getInstance().renderFrame(m_frameBuffer);

        actualFrames++;

        // Target presentation timestamp for NEXT frame
        uint32_t targetNextUs = playbackStartUs + (uint32_t)(((uint64_t)(frame + 1) * 1000000ULL) / fps);

        // Lag compensation: if rendering fell behind target by > 2 frames (>70ms),
        // re-anchor playbackStartUs to avoid runaway cumulative delay:
        int32_t lagUs = (int32_t)(micros() - targetNextUs);
        if (lagUs > (int32_t)(frameTimeUs * 2)) {
            playbackStartUs = micros() - (uint32_t)(((uint64_t)(frame + 1) * 1000000ULL) / fps);
            targetNextUs = micros();
        }

        // High-precision sub-millisecond wait loop without FreeRTOS tick overshoot
        while ((int32_t)(micros() - targetNextUs) < 0) {
            int32_t remainingUs = (int32_t)(targetNextUs - micros());
            if (remainingUs > 2500) {
                delay(1); // Sleep 1 FreeRTOS tick when enough time exists
            } else if (remainingUs > 50) {
                delayMicroseconds(50); // Microsecond precision for the final fraction
            } else {
                taskYIELD();
            }

            bluetoothAudioUpdate();
            if (btSnapshotChanged(bluetoothAtStart)) {
                Serial.println("BT STATE/DEVICE CHANGED - RESTARTING ANIMATION");
                if (m_currentMedia.audioBuffer && m_currentMedia.audioSize > 0) {
                    bluetoothAudioSetTime(0);
                }
                goto restartActualAnimation;
            }

            // Poll physical buttons during wait
            InputEvent btnEv = InputManager::getInstance().poll();
            if (btnEv == InputEvent::SELECT_SHORT_PRESS) {
                pause();
                break;
            } else if (btnEv == InputEvent::SELECT_LONG_PRESS) {
                Serial.println("[INPUT] Long press (hold 1s) -> Stopping playback.");
                stop();
                break;
            }

            CommandProtocol::getInstance().pollSerial();
        }
    }

    if (m_currentMedia.audioBuffer && m_currentMedia.audioSize > 0) {
        bluetoothAudioPause();
        bluetoothAudioSetTime(0);
    }

    m_state = PlaybackState::STOPPED;
    Serial.printf("[PLAYBACK] Video finished (%lu frames played).\n", (unsigned long)actualFrames);
}

void PlaybackManager::update() {
    bluetoothAudioUpdate();
}
