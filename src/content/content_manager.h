#ifndef CONTENT_MANAGER_H
#define CONTENT_MANAGER_H

#include <Arduino.h>
#include "media_item.h"
#include "content_api.h"
#include "media_downloader.h"

class ContentManager {
public:
    static ContentManager& getInstance();

    void begin();
    
    void setBaseUrl(const char* baseUrl);
    String getBaseUrl() const { return m_api.getBaseUrl(); }

    bool fetchCatalogue(uint32_t page = 0, uint32_t limit = CATALOGUE_PAGE_SIZE);
    bool fetchNextPage();

    bool hasMorePages() const;
    const CataloguePage& getCatalogue() const { return m_catalogue; }
    size_t getItemCount() const { return m_catalogue.items.size(); }
    const MediaItem* getItem(size_t index) const {
        if (index < m_catalogue.items.size()) return &m_catalogue.items[index];
        return nullptr;
    }

    bool downloadItem(size_t index, DownloadedMedia& outResult);
    bool downloadItemById(const String& id, DownloadedMedia& outResult);
    bool downloadCustomUrls(const String& videoUrl, const String& audioUrl, DownloadedMedia& outResult);

    // Save an offline-compatible item directly to LittleFS flash
    bool saveItemToFlash(size_t index);

    // Switch active catalogue to local offline library
    bool switchToOfflineLibrary();
    bool isOfflineMode() const { return m_isOfflineMode; }
    void setOfflineMode(bool offline) { m_isOfflineMode = offline; }

    void printCatalogue() const;

private:
    ContentManager();
    ~ContentManager() = default;

    ContentManager(const ContentManager&) = delete;
    ContentManager& operator=(const ContentManager&) = delete;

    ContentAPI m_api;
    MediaDownloader m_downloader;
    CataloguePage m_catalogue;
    bool m_isOfflineMode = false;
};

#endif // CONTENT_MANAGER_H
