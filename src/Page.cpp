#include "Page.h"

Page::Page() {
    std::memset(data, 0, PAGE_SIZE);
}

void Page::init(PageType type, int32_t pageID) {
    std::memset(data, 0, PAGE_SIZE);
    PageHeader* h = header();
    h->pageType   = type;
    h->numKeys    = 0;
    h->pageID     = pageID;
    h->nextPageID = INVALID_PAGE_ID;
    h->prevPageID = INVALID_PAGE_ID;
}

PageHeader* Page::header() {
    // The header lives at byte offset 0 of the raw buffer.
    return reinterpret_cast<PageHeader*>(data);
}

const PageHeader* Page::header() const {
    return reinterpret_cast<const PageHeader*>(data);
}

char* Page::body() {
    return data + HEADER_SIZE;
}

const char* Page::body() const {
    return data + HEADER_SIZE;
}

bool Page::isLeaf() const {
    return header()->pageType == PageType::LEAF;
}

bool Page::isInternal() const {
    return header()->pageType == PageType::INTERNAL;
}

bool Page::isFull() const {
    if (isLeaf()) return header()->numKeys >= LEAF_MAX_KEYS;
    return header()->numKeys >= INTERNAL_MAX_KEYS;
}

// ---- Leaf body layout ----
// [ key_0 .. key_{LEAF_MAX_KEYS-1} ][ val_0 .. val_{LEAF_MAX_KEYS-1} ]
int64_t& Page::leafKey(uint32_t idx) {
    int64_t* keys = reinterpret_cast<int64_t*>(body());
    return keys[idx];
}

int64_t& Page::leafValue(uint32_t idx) {
    int64_t* keys = reinterpret_cast<int64_t*>(body());
    int64_t* vals = keys + LEAF_MAX_KEYS;
    return vals[idx];
}

// ---- Internal body layout ----
// [ key_0 .. key_{INTERNAL_MAX_KEYS-1} ][ child_0 .. child_{INTERNAL_MAX_KEYS} ]
int64_t& Page::internalKey(uint32_t idx) {
    int64_t* keys = reinterpret_cast<int64_t*>(body());
    return keys[idx];
}

int32_t& Page::internalChild(uint32_t idx) {
    int64_t* keys     = reinterpret_cast<int64_t*>(body());
    int32_t* children = reinterpret_cast<int32_t*>(keys + INTERNAL_MAX_KEYS);
    return children[idx];
}

// ---- Meta page layout ----
// [ rootPageID:int32 ][ pageCount:int32 ]
int32_t& Page::metaRootPageID() {
    return *reinterpret_cast<int32_t*>(body());
}

int32_t& Page::metaPageCount() {
    return *reinterpret_cast<int32_t*>(body() + sizeof(int32_t));
}
