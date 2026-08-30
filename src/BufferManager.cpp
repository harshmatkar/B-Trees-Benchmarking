#include "BufferManager.h"
#include <iostream>
#include <stdexcept>

// Constructor 
BufferManager::BufferManager(const std::string& path, size_t poolSizeInPages)
    : dbFilePath(path), poolSize(poolSizeInPages), numPages(0), rootPageID(INVALID_PAGE_ID)
{
    // Open for read+write in binary mode. std::ios::in requires the file to
    // already exist, so we probe first and create it if missing.
    std::fstream probe(dbFilePath, std::ios::in | std::ios::binary);
    bool exists = probe.good();
    probe.close();

    if (!exists) {
        // Create an empty file.
        // exceptional case because i am providing database.db
        std::ofstream create(dbFilePath, std::ios::out | std::ios::binary);
        create.close();
    }

    dbFile.open(dbFilePath, std::ios::in | std::ios::out | std::ios::binary);
    if (!dbFile.is_open()) {
        throw std::runtime_error("BufferManager: failed to open " + dbFilePath);
    }

    loadOrCreateMetaPage();
}

// Destructor
BufferManager::~BufferManager() {
    flushAll();
    for (auto& [id, page] : pageTable) {
        delete page;
    }
    if (dbFile.is_open()) dbFile.close();
}

// -----------------------------------------------------------------------------
// Meta page (page 0): [rootPageID:int32][pageCount:int32]
// This is the only page whose contents mean something outside the B+ tree
// structure itself - it's how we find the root again after a restart.
// -----------------------------------------------------------------------------
void BufferManager::loadOrCreateMetaPage() {
    dbFile.seekg(0, std::ios::end);
    std::streamoff fileSize = dbFile.tellg();

    if (fileSize >= static_cast<std::streamoff>(PAGE_SIZE)) {
        // Database already has at least a meta page - load it.
        Page meta;
        dbFile.seekg(0, std::ios::beg);
        dbFile.read(meta.data, PAGE_SIZE);
        rootPageID = meta.metaRootPageID();
        numPages   = meta.metaPageCount();
    } else {
        // Fresh database: write an initial meta page (page 0) plus an
        // empty root leaf (page 1), then persist the meta page pointing
        // at it.
        numPages = 1; // page 0 (meta) accounted for
        int32_t rootID = allocatePage(PageType::LEAF); // becomes page 1
        rootPageID = rootID;
        persistMetaPage();
        flushAll();
    }
}

void BufferManager::persistMetaPage() {
    Page meta;
    meta.header()->pageType = PageType::META;
    meta.header()->pageID   = 0;
    meta.metaRootPageID()   = rootPageID;
    meta.metaPageCount()    = numPages;

    dbFile.seekp(0, std::ios::beg);
    dbFile.write(meta.data, PAGE_SIZE);
    dbFile.flush();
    diskWrites++;
}

void BufferManager::setRootPageID(int32_t id) {
    rootPageID = id;
    persistMetaPage();
}

// -----------------------------------------------------------------------------
// Core cache access
// -----------------------------------------------------------------------------
Page* BufferManager::fetchPage(int32_t pageID) {
    auto it = pageTable.find(pageID);
    if (it != pageTable.end()) {
        cacheHits++;
        touchLRU(pageID);
        return it->second;
    }

    cacheMisses++;
    evictOneIfFull();

    Page* page = new Page();
    readPageFromDisk(pageID, page);

    pageTable[pageID] = page;
    lruList.push_front(pageID);
    lruMap[pageID] = lruList.begin();
    dirtyFlags[pageID] = false;

    return page;
}

void BufferManager::markDirty(int32_t pageID) {
    dirtyFlags[pageID] = true;
}

int32_t BufferManager::allocatePage(PageType type) {
    int32_t newID = numPages;
    numPages++;

    Page* page = new Page();
    page->init(type, newID);

    // Grow the underlying file immediately so future seeks/reads are valid,
    // and insert straight into the cache since the caller will populate it.
    evictOneIfFull();
    writePageToDisk(newID, page);

    pageTable[newID] = page;
    lruList.push_front(newID);
    lruMap[newID] = lruList.begin();
    dirtyFlags[newID] = true; // newly allocated pages start dirty

    return newID;
}

void BufferManager::flushPage(int32_t pageID) {
    auto it = pageTable.find(pageID);
    if (it == pageTable.end()) return;
    if (!dirtyFlags[pageID]) return;

    writePageToDisk(pageID, it->second);
    dirtyFlags[pageID] = false;
}

void BufferManager::flushAll() {
    for (auto& [id, page] : pageTable) {
        if (dirtyFlags[id]) {
            writePageToDisk(id, page);
            dirtyFlags[id] = false;
        }
    }
    persistMetaPage();
}

// -----------------------------------------------------------------------------
// LRU bookkeeping
// -----------------------------------------------------------------------------
void BufferManager::touchLRU(int32_t pageID) {
    lruList.erase(lruMap[pageID]);
    lruList.push_front(pageID);
    lruMap[pageID] = lruList.begin();
}

void BufferManager::evictOneIfFull() {
    if (pageTable.size() < poolSize) return;

    int32_t victimID = lruList.back();
    lruList.pop_back();
    lruMap.erase(victimID);

    if (dirtyFlags[victimID]) {
        writePageToDisk(victimID, pageTable[victimID]);
    }

    delete pageTable[victimID];
    pageTable.erase(victimID);
    dirtyFlags.erase(victimID);
}

// -----------------------------------------------------------------------------
// Raw disk I/O - one page = one seek + one read/write of exactly PAGE_SIZE bytes.
// -----------------------------------------------------------------------------
void BufferManager::readPageFromDisk(int32_t pageID, Page* page) {
    std::streamoff offset = static_cast<std::streamoff>(pageID) * PAGE_SIZE;
    dbFile.seekg(offset, std::ios::beg);
    dbFile.read(page->data, PAGE_SIZE);
    diskReads++;

    if (dbFile.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        // Reading past current EOF (e.g. page was allocated but never
        // flushed yet in some edge case) - treat as a zeroed page.
        dbFile.clear();
        std::memset(page->data, 0, PAGE_SIZE);
    }
}

void BufferManager::writePageToDisk(int32_t pageID, const Page* page) {
    std::streamoff offset = static_cast<std::streamoff>(pageID) * PAGE_SIZE;
    dbFile.seekp(offset, std::ios::beg);
    dbFile.write(page->data, PAGE_SIZE);
    dbFile.flush();
    diskWrites++;
}
