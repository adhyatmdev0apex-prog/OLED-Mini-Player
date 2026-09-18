#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "../config/app_config.h"

class OLEDDisplay {
public:
    static OLEDDisplay& getInstance();

    void begin();
    void clear();
    
    void showSplashScreen();
    void showStatusScreen(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr);
    void showLoadingCatalogue();
    void showCatalogueScreen(size_t currentIndex, size_t totalItems, const char* title, const char* id,
                             bool isOfflineCompatible = false, bool isOfflineStored = false, bool isOfflineMode = false);
    void showOfflineSavedSuccess(const char* title);
    void showDownloadingScreen(const char* title);
    void showDownloadProgress(const char* label, size_t currentBytes, size_t totalBytes);
    void showPlayingScreen(const char* title);
    void showPauseScreen(const char* title);
    void showErrorScreen(const char* errorMsg);

    // Direct high-speed 128x64 frame renderer (12-14ms transfer, bypasses U8g2 24-byte packet fragmentation)
    void renderFrame(const uint8_t* frame1024);

    U8G2_SSD1306_128X64_NONAME_F_HW_I2C& getU8g2() { return m_u8g2; }

private:
    OLEDDisplay();
    ~OLEDDisplay() = default;

    OLEDDisplay(const OLEDDisplay&) = delete;
    OLEDDisplay& operator=(const OLEDDisplay&) = delete;

    U8G2_SSD1306_128X64_NONAME_F_HW_I2C m_u8g2;
};

#endif // OLED_DISPLAY_H
