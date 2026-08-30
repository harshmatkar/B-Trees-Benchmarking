#include "BPlusTree.h"
#include <algorithm>

BPlusTree::BPlusTree(BufferManager& bm) : bufferManager(bm) {}

// -----------------------------------------------------------------------------
// Descend from root to the leaf that owns `key`. Internal page routing rule:
// child[i] covers keys in [ key[i-1], key[i] )  (child[0] covers < key[0],
// child[n] covers >= key[n-1]). So we walk keys left-to-right and stop at
// the first key STRICTLY GREATER than the search key.
// -----------------------------------------------------------------------------
int32_t BPlusTree::findLeafPageID(int64_t key, std::vector<int32_t>* path) {
    int32_t currentID = bufferManager.getRootPageID();
    Page* current = bufferManager.fetchPage(currentID);

    while (current->isInternal()) {
        if (path) path->push_back(currentID);

        uint32_t numKeys = current->header()->numKeys;
        uint32_t idx = 0;
        while (idx < numKeys && key >= current->internalKey(idx)) idx++;

        currentID = current->internalChild(idx);
        current = bufferManager.fetchPage(currentID);
    }
    return currentID;
}

// This is linear scan within one page
std::optional<int64_t> BPlusTree::search(int64_t key) {
    int32_t leafID = findLeafPageID(key);
    Page* leaf = bufferManager.fetchPage(leafID);

    // Linear scan within one page is O(1)-bounded (<= LEAF_MAX_KEYS ~254
    // entries), so overall search remains O(log N) in page accesses.
    uint32_t n = leaf->header()->numKeys;
    for (uint32_t i = 0; i < n; i++) {
        if (leaf->leafKey(i) == key) return leaf->leafValue(i);
    }
    return std::nullopt;
}

void BPlusTree::insertIntoLeafSorted(Page* leaf, int64_t key, int64_t value) {
    uint32_t n = leaf->header()->numKeys;
    uint32_t pos = 0;
    while (pos < n && leaf->leafKey(pos) < key) pos++;

    if (pos < n && leaf->leafKey(pos) == key) {
        // Key already exists: this is an update, not an insert.
        leaf->leafValue(pos) = value;
        return;
    }

    // Shift everything right of `pos` one slot to the right to make room.
    for (uint32_t i = n; i > pos; i--) {
        leaf->leafKey(i)   = leaf->leafKey(i - 1);
        leaf->leafValue(i) = leaf->leafValue(i - 1);
    }
    leaf->leafKey(pos)   = key;
    leaf->leafValue(pos) = value;
    leaf->header()->numKeys = n + 1;
}

void BPlusTree::insert(int64_t key, int64_t value) {
    std::vector<int32_t> path; // internal pages visited, root-first
    int32_t leafID = findLeafPageID(key, &path);
    Page* leaf = bufferManager.fetchPage(leafID);

    if (!leaf->isFull()) {
        insertIntoLeafSorted(leaf, key, value);
        bufferManager.markDirty(leafID);
        return;
    }

    // Leaf is full. If the key already exists we can still update in place
    // without needing a split at all.
    uint32_t n = leaf->header()->numKeys;
    for (uint32_t i = 0; i < n; i++) {
        if (leaf->leafKey(i) == key) {
            leaf->leafValue(i) = value;
            bufferManager.markDirty(leafID);
            return;
        }
    }

    splitLeafAndInsert(leafID, key, value, path);
}

// -----------------------------------------------------------------------------
// Split a full leaf into two roughly-equal leaves and push the first key of
// the new right leaf up to the parent as a separator. Note: unlike internal
// splits, the separator key is COPIED up, not removed - the leaf level must
// retain every key so linear leaf-chain scans stay correct.
// -----------------------------------------------------------------------------
void BPlusTree::splitLeafAndInsert(int32_t leafID, int64_t key, int64_t value,
                                    std::vector<int32_t>& path) {
    Page* leaf = bufferManager.fetchPage(leafID);
    uint32_t n = leaf->header()->numKeys; // == LEAF_MAX_KEYS (page is full)

    // Merge existing entries with the new one into a sorted temp buffer.
    std::vector<std::pair<int64_t, int64_t>> merged;
    merged.reserve(n + 1);
    for (uint32_t i = 0; i < n; i++) merged.emplace_back(leaf->leafKey(i), leaf->leafValue(i));
    merged.emplace_back(key, value);
    std::sort(merged.begin(), merged.end());

    uint32_t total      = static_cast<uint32_t>(merged.size()); // LEAF_MAX_KEYS + 1
    uint32_t leftCount  = (total + 1) / 2;                      // left gets the extra one if odd
    uint32_t rightCount = total - leftCount;

    // allocatePage() may trigger an LRU eviction, which can invalidate any
    // Page* we already hold (including `leaf` itself) - so we allocate FIRST,
    // then re-fetch every pointer we need before writing through them.
    int32_t newLeafID = bufferManager.allocatePage(PageType::LEAF);
    Page* leftLeaf  = bufferManager.fetchPage(leafID);
    Page* newLeaf   = bufferManager.fetchPage(newLeafID);

    for (uint32_t i = 0; i < leftCount; i++) {
        leftLeaf->leafKey(i)   = merged[i].first;
        leftLeaf->leafValue(i) = merged[i].second;
    }
    leftLeaf->header()->numKeys = leftCount;

    for (uint32_t i = 0; i < rightCount; i++) {
        newLeaf->leafKey(i)   = merged[leftCount + i].first;
        newLeaf->leafValue(i) = merged[leftCount + i].second;
    }
    newLeaf->header()->numKeys = rightCount;

    // Splice newLeaf into the doubly linked leaf chain: left -> new -> oldNext
    int32_t oldNext = leftLeaf->header()->nextPageID;
    newLeaf->header()->nextPageID = oldNext;
    newLeaf->header()->prevPageID = leafID;
    leftLeaf->header()->nextPageID = newLeafID;

    if (oldNext != INVALID_PAGE_ID) {
        Page* oldNextPage = bufferManager.fetchPage(oldNext);
        oldNextPage->header()->prevPageID = newLeafID;
        bufferManager.markDirty(oldNext);
    }

    bufferManager.markDirty(leafID);
    bufferManager.markDirty(newLeafID);

    int64_t separator = newLeaf->leafKey(0);
    insertIntoParent(leafID, separator, newLeafID, path);
}

// -----------------------------------------------------------------------------
// Insert (separatorKey -> rightChildID) into the parent of leftChildID.
// `path` holds the root-to-parent chain of internal page ids, so the parent
// is simply path.back() - no stored parent pointers needed anywhere.
// -----------------------------------------------------------------------------
void BPlusTree::insertIntoParent(int32_t leftChildID, int64_t separatorKey, int32_t rightChildID,
                                  std::vector<int32_t>& path) {
    if (path.empty()) {
        // leftChildID was the root - grow the tree by one level.
        int32_t newRootID = bufferManager.allocatePage(PageType::INTERNAL);
        Page* newRoot = bufferManager.fetchPage(newRootID);
        newRoot->internalKey(0)   = separatorKey;
        newRoot->internalChild(0) = leftChildID;
        newRoot->internalChild(1) = rightChildID;
        newRoot->header()->numKeys = 1;
        bufferManager.markDirty(newRootID);
        bufferManager.setRootPageID(newRootID);
        return;
    }

    int32_t parentID = path.back();
    path.pop_back();
    Page* parent = bufferManager.fetchPage(parentID);

    if (!parent->isFull()) {
        uint32_t n = parent->header()->numKeys;
        uint32_t pos = 0;
        while (pos < n && parent->internalKey(pos) < separatorKey) pos++;

        // Shift keys [pos..n-1] right by one to open a slot at `pos`.
        for (uint32_t i = n; i > pos; i--) parent->internalKey(i) = parent->internalKey(i - 1);
        // Shift children [pos+1..n] right by one to open a slot at `pos+1`.
        for (uint32_t i = n + 1; i > pos + 1; i--) parent->internalChild(i) = parent->internalChild(i - 1);

        parent->internalKey(pos)       = separatorKey;
        parent->internalChild(pos + 1) = rightChildID;
        parent->header()->numKeys = n + 1;
        bufferManager.markDirty(parentID);
    } else {
        splitInternalAndInsert(parentID, separatorKey, rightChildID, path);
    }
}

// -----------------------------------------------------------------------------
// Split a full internal page. Unlike leaf splits, the middle key moves UP
// and does NOT remain in either child (standard B-tree/B+-tree internal
// split) - it becomes the new separator between the two resulting pages.
// -----------------------------------------------------------------------------
void BPlusTree::splitInternalAndInsert(int32_t internalID, int64_t key, int32_t rightChildID,
                                        std::vector<int32_t>& path) {
    Page* node = bufferManager.fetchPage(internalID);
    uint32_t n = node->header()->numKeys; // == INTERNAL_MAX_KEYS (page is full)

    // Find where the new (key, rightChildID) belongs among existing entries.
    uint32_t pos = 0;
    while (pos < n && node->internalKey(pos) < key) pos++;

    // Build merged temp arrays: n+1 keys, n+2 children.
    std::vector<int64_t> tempKeys;
    std::vector<int32_t> tempChildren;
    tempKeys.reserve(n + 1);
    tempChildren.reserve(n + 2);

    for (uint32_t i = 0; i < pos; i++) tempKeys.push_back(node->internalKey(i));
    tempKeys.push_back(key);
    for (uint32_t i = pos; i < n; i++) tempKeys.push_back(node->internalKey(i));

    for (uint32_t i = 0; i <= pos; i++) tempChildren.push_back(node->internalChild(i));
    tempChildren.push_back(rightChildID);
    for (uint32_t i = pos + 1; i <= n; i++) tempChildren.push_back(node->internalChild(i));

    uint32_t mid = static_cast<uint32_t>(tempKeys.size()) / 2;
    int64_t middleKey = tempKeys[mid];

    // Allocate the new right sibling BEFORE writing through any pointer,
    // since allocation can evict and invalidate `node`.
    int32_t newInternalID = bufferManager.allocatePage(PageType::INTERNAL);
    Page* leftNode  = bufferManager.fetchPage(internalID);
    Page* rightNode = bufferManager.fetchPage(newInternalID);

    // Left keeps tempKeys[0..mid-1] and tempChildren[0..mid].
    for (uint32_t i = 0; i < mid; i++) leftNode->internalKey(i) = tempKeys[i];
    for (uint32_t i = 0; i <= mid; i++) leftNode->internalChild(i) = tempChildren[i];
    leftNode->header()->numKeys = mid;

    // Right gets tempKeys[mid+1..end] and tempChildren[mid+1..end].
    uint32_t rightKeyCount = static_cast<uint32_t>(tempKeys.size()) - mid - 1;
    for (uint32_t i = 0; i < rightKeyCount; i++) rightNode->internalKey(i) = tempKeys[mid + 1 + i];
    for (uint32_t i = 0; i <= rightKeyCount; i++) rightNode->internalChild(i) = tempChildren[mid + 1 + i];
    rightNode->header()->numKeys = rightKeyCount;

    bufferManager.markDirty(internalID);
    bufferManager.markDirty(newInternalID);

    insertIntoParent(internalID, middleKey, newInternalID, path);
}

// -----------------------------------------------------------------------------
// Range scan: ONE tree descent to find the starting leaf, then follow
// nextPageID sibling pointers - no repeated root-to-leaf descents.
// -----------------------------------------------------------------------------
std::vector<std::pair<int64_t, int64_t>> BPlusTree::rangeQuery(int64_t startKey, int64_t endKey) {
    std::vector<std::pair<int64_t, int64_t>> results;
    if (startKey > endKey) return results;

    int32_t leafID = findLeafPageID(startKey);
    Page* leaf = bufferManager.fetchPage(leafID);

    uint32_t n = leaf->header()->numKeys;
    uint32_t idx = 0;
    while (idx < n && leaf->leafKey(idx) < startKey) idx++;

    while (true) {
        n = leaf->header()->numKeys;
        while (idx < n) {
            int64_t k = leaf->leafKey(idx);
            if (k > endKey) return results;
            results.emplace_back(k, leaf->leafValue(idx));
            idx++;
        }
        int32_t nextID = leaf->header()->nextPageID;
        if (nextID == INVALID_PAGE_ID) break;
        leafID = nextID;
        leaf = bufferManager.fetchPage(leafID);
        idx = 0;
    }
    return results;
}

// Finding height of the B+ tree
int BPlusTree::treeHeight() {
    int h = 1;
    int32_t id = bufferManager.getRootPageID(); // root page 
    Page* p = bufferManager.fetchPage(id);
    while (p->isInternal()) {
        h++;
        id = p->internalChild(0);
        p = bufferManager.fetchPage(id);
    }
    return h;
}

BPlusTree::Stats BPlusTree::computeStats() {
    Stats s{0, 0, 0, 0};
    s.height = treeHeight();
    s.totalPages = static_cast<uint32_t>(bufferManager.getPageCount());

    std::vector<int32_t> stack;
    stack.push_back(bufferManager.getRootPageID());
    while (!stack.empty()) {
        int32_t id = stack.back();
        stack.pop_back();
        Page* p = bufferManager.fetchPage(id);
        if (p->isLeaf()) {
            s.leafPages++;
        } else {
            s.internalPages++;
            uint32_t n = p->header()->numKeys;
            for (uint32_t i = 0; i <= n; i++) stack.push_back(p->internalChild(i));
        }
    }
    return s;
}
