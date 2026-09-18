#ifndef MEDIA_ITEM_H
#define MEDIA_ITEM_H

#include <Arduino.h>
#include <vector>

// ============================================================
// TRANSPORT-INDEPENDENT MEDIA SELECTION MODEL
// ============================================================
//
// The playback system accepts this structure rather than directly
// depending on HTTP or JSON.
//
// This is critical for future architecture (WROOM -> UART -> MediaItem -> CAM).
//

struct MediaItem {
    String id;
    String name;
    String videoUrl;
    String audioUrl;
    bool isSpiffsCompatible = false;
    size_t totalSize = 0;
    bool isOfflineStored = false;

    bool isValid() const {
        return id.length() > 0 && name.length() > 0 && videoUrl.length() > 0 && audioUrl.length() > 0;
    }
};

struct CataloguePage {
    uint32_t page = 0;
    uint32_t limit = 10;
    uint32_t total = 0;
    std::vector<MediaItem> items;
};

#endif // MEDIA_ITEM_H
