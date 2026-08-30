#pragma once
// =============================================================================
// BufferManager.h
//
// Owns the single on-disk file (database.db) and mediates ALL access to pages.
// The B+ tree layer never touches the file directly - it only ever calls
// fetchPage(id) and gets back an in-memory Page* it can read/mutate.
//
// Responsibilities:
//   1. Translate a pageID <-> byte offset in database.db (offset = id * PAGE_SIZE)
//      and perform the actual read()/write() syscalls.
//   2. Keep a bounded in-memory pool of "hot" pages (the LRU cache) so that
//      repeated access to the same pages (e.g. upper levels of the tree)
//      doesn't hit disk every time.
//   3. Track which cached pages are "dirty" (modified since last flush) and
//      only write those back to disk - clean pages are simply dropped.
//   4. Evict the Least-Recently-Used page when the pool is full and a new
//      page must be brought in.
//   5. Persist tree-level metadata (root page id, total page count) in a
//      reserved page 0 ("meta page") so the database survives restarts.
//
// LRU implementation: a doubly linked list (std::list<int32_t>) holds page
// ids ordered from Most-Recently-Used (front) to Least-Recently-Used (back).
// A hash map from pageID -> list iterator gives O(1) "move this id to the
// front" on every access, and O(1) "evict the back id" when the pool is full.
// =============================================================================

#include "Page.h"
#include <string>
#include <fstream>
#include <unordered_map>
#include <list>
#include <cstdint>

class BufferManager {
public:
    // Opens (or creates) dbFilePath. poolSizeInPages bounds how many 4KB
    // pages are kept resident in RAM at once (poolSizeInPages * 4KB = RAM used).
    explicit BufferManager(const std::string& dbFilePath, size_t poolSizeInPages = 512);
    ~BufferManager();

    // Non-copyable: this object owns a live file handle and raw pointers.
    BufferManager(const BufferManager&) = delete;
    BufferManager& operator=(const BufferManager&) = delete;

    // Returns a pointer to the requested page, loading it from disk into the
    // cache if it isn't resident already. The returned pointer is valid until
    // the page is evicted - callers should not hold onto it across many
    // other fetchPage() calls once the pool is near capacity.
    Page* fetchPage(int32_t pageID);

    // Marks a cached page as dirty, meaning it must be written back to disk
    // before being evicted or on flushAll(). Call this after ANY mutation.
    void markDirty(int32_t pageID);

    // Allocates a brand new page at the end of the file, initializes it as
    // `type`, inserts it into the cache, and returns its new pageID.
    int32_t allocatePage(PageType type);

    // Forces one page to disk immediately (no-op if not dirty).
    void flushPage(int32_t pageID);

    // Forces every dirty page (and the meta page) to disk. Called on
    // shutdown, and can be called manually for durability checkpoints.
    void flushAll();

    int32_t getRootPageID() const { return rootPageID; }
    void    setRootPageID(int32_t id);

    // ---- Stats, useful for the CLI/benchmark to report cache effectiveness ----
    size_t cacheHits   = 0;
    size_t cacheMisses = 0;
    size_t diskReads    = 0;
    size_t diskWrites   = 0;
    size_t getPoolSize() const { return poolSize; }
    size_t getResidentPageCount() const { return pageTable.size(); }
    int32_t getPageCount() const { return numPages; }

private:
    std::string   dbFilePath;
    std::fstream  dbFile;
    size_t        poolSize;

    int32_t numPages;     // total pages ever allocated, including meta page 0
    int32_t rootPageID;   // cached copy of meta page's root pointer

    // ---- The cache itself ----
    std::unordered_map<int32_t, Page*> pageTable;                 // pageID -> in-memory page
    std::list<int32_t>                 lruList;                   // MRU (front) .. LRU (back)
    std::unordered_map<int32_t, std::list<int32_t>::iterator> lruMap; // pageID -> its node in lruList
    std::unordered_map<int32_t, bool>  dirtyFlags;

    void touchLRU(int32_t pageID);          // move pageID to MRU position
    void evictOneIfFull();                  // evict LRU victim if pool is at capacity
    void readPageFromDisk(int32_t pageID, Page* page);
    void writePageToDisk(int32_t pageID, const Page* page);

    void loadOrCreateMetaPage();            // called once from constructor
    void persistMetaPage();                 // writes page 0 (root id + page count)
};
