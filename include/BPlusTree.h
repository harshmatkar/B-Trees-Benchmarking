#pragma once
// =============================================================================
// BPlusTree.h
//
// Classic disk-backed B+ tree built entirely on top of BufferManager::fetchPage.
// The tree never allocates its own memory for nodes - every "node" IS a Page
// living in the buffer pool, identified by its int32 pageID.
//
// Key properties:
//   - All data values (record offsets) live ONLY in leaf pages.
//   - Internal pages store routing keys + child pageIDs only.
//   - Leaf pages form a doubly linked list (via header()->nextPageID/prevPageID)
//     so range scans never need to touch internal pages once they've
//     descended to the first leaf.
//   - Insertion is top-down: we first find & remember the root-to-leaf path,
//     then split bottom-up along that remembered path if needed. This avoids
//     the need to store parent pointers in every page.
// =============================================================================

#include "BufferManager.h"
#include <cstdint>
#include <optional>
#include <vector>
#include <utility>

class BPlusTree {
public:
    explicit BPlusTree(BufferManager& bm);

    // Insert or update (key -> value). O(log N) page accesses.
    void insert(int64_t key, int64_t value);

    // Point lookup. O(log N) page accesses.
    std::optional<int64_t> search(int64_t key);

    // Inclusive range scan [startKey, endKey]. Finds the starting leaf in
    // O(log N), then walks the leaf linked list in O(K/LEAF_MAX_KEYS) where
    // K is the number of matching records - no repeated tree descents.
    std::vector<std::pair<int64_t, int64_t>> rangeQuery(int64_t startKey, int64_t endKey);

    // Diagnostics for the CLI/benchmark output.
    int treeHeight();
    struct Stats {
        int height;
        uint32_t totalPages;
        uint32_t leafPages;
        uint32_t internalPages;
    };
    Stats computeStats();

private:
    BufferManager& bufferManager;

    // Descends from root to the leaf that would contain `key`, recording
    // every internal page visited into `path` (root first). Returns the
    // leaf's pageID.
    int32_t findLeafPageID(int64_t key, std::vector<int32_t>* path = nullptr);

    // Inserts (key,value) into an already-non-full leaf, keeping it sorted.
    void insertIntoLeafSorted(Page* leaf, int64_t key, int64_t value);

    // Leaf is full: split it into two, then propagate the new separator
    // key up to the parent chain recorded in `path`.
    void splitLeafAndInsert(int32_t leafID, int64_t key, int64_t value,
                             std::vector<int32_t>& path);

    // Inserts (separatorKey -> rightChildID) into the parent found at the
    // top of `path` (or creates a new root if `path` is empty), splitting
    // that parent first if it's full.
    void insertIntoParent(int32_t leftChildID, int64_t separatorKey, int32_t rightChildID,
                           std::vector<int32_t>& path);

    // Internal page is full: split it, pushing its middle key further up
    // the recorded path.
    void splitInternalAndInsert(int32_t internalID, int64_t key, int32_t rightChildID,
                                 std::vector<int32_t>& path);
};
