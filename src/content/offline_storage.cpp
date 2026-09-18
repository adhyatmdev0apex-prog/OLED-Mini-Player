#include "offline_storage.h"

OfflineStorage& OfflineStorage::getInstance() {
    static OfflineStorage instance;
    return instance;
}

OfflineStorage::OfflineStorage()
    : m_mounted(false) {
}

bool OfflineStorage::begin() {
    Serial.println("[STORAGE] Mounting LittleFS persistent storage...");
    // Mount with formatOnFail = true so unformatted partition formats on first boot
    if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        Serial.println("[STORAGE] Error: LittleFS partition mount failed!");
        m_mounted = false;
        return false;
    }

    m_mounted = true;
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    Serial.printf("[STORAGE] LittleFS mounted successfully. Total: %u bytes, Used: %u bytes, Free: %u bytes\n",
                  (unsigned)total, (unsigned)used, (unsigned)(total - used));

    loadCatalogueFromFlash();
    return true;
}

size_t OfflineStorage::getTotalBytes() const {
    return m_mounted ? LittleFS.totalBytes() : 0;
}

size_t OfflineStorage::getUsedBytes() const {
    return m_mounted ? LittleFS.usedBytes() : 0;
}

size_t OfflineStorage::getFreeBytes() const {
    if (!m_mounted) return 0;
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    return (total > used) ? (total - used) : 0;
}

bool OfflineStorage::isItemStored(const String& id) const {
    if (!m_mounted || id.length() == 0) return false;

    // Check offline index first to avoid VFS open warnings on non-stored files
    for (const auto& item : m_offlineCatalogue.items) {
        if (item.id == id) {
            String vPath = getVideoFilePath(id);
            String aPath = getAudioFilePath(id);
            return LittleFS.exists(vPath) && LittleFS.exists(aPath);
        }
    }
    return false;
}

bool OfflineStorage::loadCatalogueFromFlash() {
    m_offlineCatalogue.items.clear();
    m_offlineCatalogue.page = 0;
    m_offlineCatalogue.total = 0;

    if (!m_mounted || !LittleFS.exists(OFFLINE_CATALOGUE_PATH)) {
        Serial.println("[STORAGE] No offline catalogue file found on flash.");
        return false;
    }

    File f = LittleFS.open(OFFLINE_CATALOGUE_PATH, FILE_READ);
    if (!f) {
        Serial.println("[STORAGE] Failed to open offline catalogue for reading.");
        return false;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[STORAGE] Failed to parse offline catalogue JSON: %s\n", err.c_str());
        return false;
    }

    JsonArray itemsArr = doc["items"].as<JsonArray>();
    for (JsonObject obj : itemsArr) {
        MediaItem it;
        it.id = obj["id"] | "";
        it.name = obj["name"] | "";
        it.videoUrl = obj["video_path"] | getVideoFilePath(it.id);
        it.audioUrl = obj["audio_path"] | getAudioFilePath(it.id);
        it.isSpiffsCompatible = true;
        it.isOfflineStored = true;
        it.totalSize = obj["total_size"] | 0;

        if (it.isValid() && LittleFS.exists(it.videoUrl) && LittleFS.exists(it.audioUrl)) {
            m_offlineCatalogue.items.push_back(it);
        }
    }

    m_offlineCatalogue.total = m_offlineCatalogue.items.size();
    Serial.printf("[STORAGE] Loaded %u offline media items from flash catalogue.\n", (unsigned)m_offlineCatalogue.total);
    return true;
}

bool OfflineStorage::saveCatalogueToFlash() {
    if (!m_mounted) return false;

    File f = LittleFS.open(OFFLINE_CATALOGUE_PATH, FILE_WRITE);
    if (!f) {
        Serial.println("[STORAGE] Failed to open offline catalogue for writing.");
        return false;
    }

    DynamicJsonDocument doc(4096);
    doc["total"] = m_offlineCatalogue.items.size();
    JsonArray itemsArr = doc.createNestedArray("items");

    for (const auto& it : m_offlineCatalogue.items) {
        JsonObject obj = itemsArr.createNestedObject();
        obj["id"] = it.id;
        obj["name"] = it.name;
        obj["video_path"] = it.videoUrl;
        obj["audio_path"] = it.audioUrl;
        obj["total_size"] = it.totalSize;
    }

    serializeJson(doc, f);
    f.close();
    Serial.printf("[STORAGE] Saved offline catalogue to flash (%u items).\n", (unsigned)m_offlineCatalogue.items.size());
    return true;
}

bool OfflineStorage::registerDownloadedItem(const MediaItem& item, size_t videoSize, size_t audioSize) {
    if (!m_mounted) return false;

    // Remove existing if replacing
    removeStoredItem(item.id);

    MediaItem stored = item;
    stored.videoUrl = getVideoFilePath(item.id);
    stored.audioUrl = getAudioFilePath(item.id);
    stored.isSpiffsCompatible = true;
    stored.isOfflineStored = true;
    stored.totalSize = videoSize + audioSize;

    m_offlineCatalogue.items.push_back(stored);
    m_offlineCatalogue.total = m_offlineCatalogue.items.size();

    return saveCatalogueToFlash();
}

bool OfflineStorage::removeStoredItem(const String& id) {
    if (!m_mounted) return false;

    String vPath = getVideoFilePath(id);
    String aPath = getAudioFilePath(id);

    if (LittleFS.exists(vPath)) LittleFS.remove(vPath);
    if (LittleFS.exists(aPath)) LittleFS.remove(aPath);

    for (auto it = m_offlineCatalogue.items.begin(); it != m_offlineCatalogue.items.end(); ++it) {
        if (it->id == id) {
            m_offlineCatalogue.items.erase(it);
            m_offlineCatalogue.total = m_offlineCatalogue.items.size();
            saveCatalogueToFlash();
            return true;
        }
    }
    return false;
}

bool OfflineStorage::loadItemToPSRAM(const MediaItem& item, DownloadedMedia& outResult) {
    if (!m_mounted) {
        Serial.println("[STORAGE] LittleFS not mounted.");
        return false;
    }

    String vPath = item.videoUrl.startsWith("/") ? item.videoUrl : getVideoFilePath(item.id);
    String aPath = item.audioUrl.startsWith("/") ? item.audioUrl : getAudioFilePath(item.id);

    if (!LittleFS.exists(vPath) || !LittleFS.exists(aPath)) {
        Serial.printf("[STORAGE] Missing offline files for item %s.\n", item.id.c_str());
        return false;
    }

    File vFile = LittleFS.open(vPath, FILE_READ);
    File aFile = LittleFS.open(aPath, FILE_READ);

    if (!vFile || !aFile) {
        Serial.println("[STORAGE] Failed to open offline files.");
        if (vFile) vFile.close();
        if (aFile) aFile.close();
        return false;
    }

    size_t vSize = vFile.size();
    size_t aSize = aFile.size();

    if (!psramFound()) {
        Serial.println("[STORAGE] PSRAM not found, cannot stage offline media.");
        vFile.close();
        aFile.close();
        return false;
    }

    size_t freePsram = ESP.getFreePsram();
    if (freePsram < (vSize + aSize + PSRAM_SAFETY_MARGIN_BYTES)) {
        Serial.printf("[STORAGE] Insufficient PSRAM to stage offline media (Need %u, Have %u).\n",
                      (unsigned)(vSize + aSize + PSRAM_SAFETY_MARGIN_BYTES), (unsigned)freePsram);
        vFile.close();
        aFile.close();
        return false;
    }

    DownloadedMedia temp;
    temp.item = item;
    temp.item.isOfflineStored = true;

    temp.videoBuffer = (uint8_t*)ps_malloc(vSize);
    temp.audioBuffer = (uint8_t*)ps_malloc(aSize);

    if (!temp.videoBuffer || !temp.audioBuffer) {
        Serial.println("[STORAGE] ps_malloc allocation failed for offline media.");
        temp.freeBuffers();
        vFile.close();
        aFile.close();
        return false;
    }

    Serial.printf("[STORAGE] Staging offline media '%s' into PSRAM (Video: %u, Audio: %u bytes)...\n",
                  item.name.c_str(), (unsigned)vSize, (unsigned)aSize);

    vFile.read(temp.videoBuffer, vSize);
    aFile.read(temp.audioBuffer, aSize);

    vFile.close();
    aFile.close();

    temp.videoSize = vSize;
    temp.audioSize = aSize;

    outResult = temp;
    Serial.println("[STORAGE] Offline media staged into PSRAM successfully!");
    return true;
}
