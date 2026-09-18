#include "content_manager.h"
#include "../playback/playback_manager.h"
#include "offline_storage.h"

ContentManager& ContentManager::getInstance() {
    static ContentManager instance;
    return instance;
}

ContentManager::ContentManager()
    : m_isOfflineMode(false) {
}

void ContentManager::begin() {
    OfflineStorage::getInstance().begin();
}

void ContentManager::setBaseUrl(const char* baseUrl) {
    m_api.setBaseUrl(baseUrl);
}

bool ContentManager::fetchCatalogue(uint32_t page, uint32_t limit) {
    Serial.printf("[CONTENT] Fetching catalogue page %u (limit %u)...\n", page, limit);
    m_isOfflineMode = false;
    return m_api.fetchCatalogue(page, limit, m_catalogue);
}

bool ContentManager::fetchNextPage() {
    if (m_isOfflineMode || !hasMorePages()) {
        Serial.println("[CONTENT] No more pages available.");
        return false;
    }
    return fetchCatalogue(m_catalogue.page + 1, m_catalogue.limit);
}

bool ContentManager::hasMorePages() const {
    if (m_isOfflineMode || m_catalogue.limit == 0) return false;
    return (m_catalogue.page + 1) * m_catalogue.limit < m_catalogue.total;
}

bool ContentManager::downloadItem(size_t index, DownloadedMedia& outResult) {
    if (index >= m_catalogue.items.size()) {
        Serial.printf("[CONTENT] Error: Index %u out of range (Catalogue size: %u).\n",
                      (unsigned)index, (unsigned)m_catalogue.items.size());
        return false;
    }

    PlaybackManager::getInstance().freeCurrentMedia();

    const auto& item = m_catalogue.items[index];
    if (m_isOfflineMode || item.isOfflineStored) {
        Serial.printf("[CONTENT] Loading item [%u] '%s' directly from Flash...\n", (unsigned)index, item.name.c_str());
        return OfflineStorage::getInstance().loadItemToPSRAM(item, outResult);
    }

    return m_downloader.downloadMedia(item, outResult);
}

bool ContentManager::saveItemToFlash(size_t index) {
    if (index >= m_catalogue.items.size()) {
        Serial.printf("[CONTENT] Error: Index %u out of range.\n", (unsigned)index);
        return false;
    }

    const auto& item = m_catalogue.items[index];
    String vPath = OfflineStorage::getInstance().getVideoFilePath(item.id);
    String aPath = OfflineStorage::getInstance().getAudioFilePath(item.id);

    size_t vSize = 0, aSize = 0;
    if (!m_downloader.downloadToFlash(item, vPath, aPath, vSize, aSize)) {
        Serial.printf("[CONTENT] Failed to download item '%s' to flash.\n", item.name.c_str());
        return false;
    }

    if (!OfflineStorage::getInstance().registerDownloadedItem(item, vSize, aSize)) {
        Serial.printf("[CONTENT] Failed to register item '%s' in offline catalogue.\n", item.name.c_str());
        return false;
    }

    m_catalogue.items[index].isOfflineStored = true;
    return true;
}

bool ContentManager::switchToOfflineLibrary() {
    m_isOfflineMode = true;
    m_catalogue = OfflineStorage::getInstance().getOfflineCatalogue();
    Serial.printf("[CONTENT] Switched to Offline Media Library (%u items found).\n", (unsigned)m_catalogue.total);
    printCatalogue();
    return m_catalogue.total > 0;
}

bool ContentManager::downloadItemById(const String& id, DownloadedMedia& outResult) {
    for (const auto& item : m_catalogue.items) {
        if (item.id == id) {
            PlaybackManager::getInstance().freeCurrentMedia();
            return m_downloader.downloadMedia(item, outResult);
        }
    }
    Serial.printf("[CONTENT] Error: Item ID '%s' not found in current catalogue page.\n", id.c_str());
    return false;
}

bool ContentManager::downloadCustomUrls(const String& videoUrl, const String& audioUrl, DownloadedMedia& outResult) {
    MediaItem item;
    item.id = "custom";
    item.name = "Custom URL Media";
    item.videoUrl = ContentAPI::buildAbsoluteUrl(m_api.getBaseUrl(), videoUrl);
    item.audioUrl = ContentAPI::buildAbsoluteUrl(m_api.getBaseUrl(), audioUrl);

    PlaybackManager::getInstance().freeCurrentMedia();
    return m_downloader.downloadMedia(item, outResult);
}

void ContentManager::printCatalogue() const {
    Serial.println();
    Serial.println("========================================");
    Serial.printf("         MEDIA CATALOGUE (Page %u)\n", m_catalogue.page);
    Serial.println("========================================");
    Serial.printf("Total videos: %u (Limit: %u per page)\n\n", m_catalogue.total, m_catalogue.limit);

    if (m_catalogue.items.empty()) {
        Serial.println("  (No items in catalogue page)");
    } else {
        for (size_t i = 0; i < m_catalogue.items.size(); i++) {
            const auto& item = m_catalogue.items[i];
            Serial.printf("  [%u] ID: %s | Name: %s\n", (unsigned)i, item.id.c_str(), item.name.c_str());
            Serial.printf("      Video: %s\n", item.videoUrl.c_str());
            Serial.printf("      Audio: %s\n", item.audioUrl.c_str());
        }
    }

    if (hasMorePages()) {
        Serial.printf("\n[Notice] More pages available (Page %u of %u)\n",
                      m_catalogue.page + 1, (m_catalogue.total + m_catalogue.limit - 1) / m_catalogue.limit);
    }
    Serial.println("========================================");
    Serial.println();
}
