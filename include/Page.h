#pragma once
// =============================================================================
// Page.h
//
// Defines the on-disk / in-memory representation of a single database page.
//
// MEMORY LAYOUT (exactly 4096 bytes, byte-for-byte identical on disk and in RAM):
//
//   +-----------------------------------------------------------+
//   |                     PageHeader (17 bytes)                 |
//   |  pageType | numKeys | pageID | nextPageID | prevPageID    |
//   +-----------------------------------------------------------+
//   |                        Body (4079 bytes)                  |
//   |                                                           |
//   |  LEAF page body:                                          |
//   |     [ key_0 .. key_{N-1} ]  (int64_t each)                |
//   |     [ val_0 .. val_{N-1} ]  (int64_t each = record offset)|
//   |                                                           |
//   |  INTERNAL page body:                                      |
//   |     [ key_0 .. key_{M-1} ]     (int64_t each)             |
//   |     [ child_0 .. child_M ]     (int32_t page ids, M+1 of) |
//   +-----------------------------------------------------------+
//
// We deliberately use a FIXED ARRAY layout (not slotted-page) because our
// keys/values are fixed-width (int64_t). This keeps offset arithmetic O(1)
// and avoids the extra indirection a slot directory would add — appropriate
// for a fixed-schema index. The whole struct is exactly PAGE_SIZE bytes,
// so a single read()/write() syscall moves one page to/from disk.
// =============================================================================

#include <cstdint>
#include <cstring>
#include <stdexcept>

constexpr uint32_t PAGE_SIZE = 4096;           
constexpr int32_t  INVALID_PAGE_ID = -1;

enum class PageType : uint8_t {
    LEAF     = 0,
    INTERNAL = 1,
    META     = 2,   // page 0 only: stores root pointer + page count
    INVALID  = 3
};

// #pragma pack ensures NO compiler padding is inserted between header fields,
// so the header's on-disk size exactly matches sizeof(PageHeader) and is
// stable across compilers/platforms (critical for a binary file format).
#pragma pack(push, 1)
struct PageHeader {
    PageType  pageType;      // 1 byte
    uint32_t  numKeys;       // 4 bytes - number of live keys in this page
    int32_t   pageID;        // 4 bytes - this page's own id (== its offset/PAGE_SIZE)
    int32_t   nextPageID;    // 4 bytes - leaf: right sibling page id (doubly linked leaf list)
    int32_t   prevPageID;    // 4 bytes - leaf: left sibling page id
};
#pragma pack(pop)

constexpr uint32_t HEADER_SIZE = sizeof(PageHeader);          // 17 bytes
constexpr uint32_t BODY_SIZE   = PAGE_SIZE - HEADER_SIZE;      // 4079 bytes

// ---- Capacity math (computed once, at compile time) ------------------------
//
// Leaf page holds N (key,value) pairs, each 8+8 = 16 bytes:
//     N * 16 <= BODY_SIZE  =>  N = BODY_SIZE / 16
constexpr uint32_t LEAF_ENTRY_SIZE = sizeof(int64_t) + sizeof(int64_t);
constexpr uint32_t LEAF_MAX_KEYS   = BODY_SIZE / LEAF_ENTRY_SIZE;

// Internal page holds M keys and M+1 child pointers:
//     M*8 + (M+1)*4 <= BODY_SIZE  =>  M <= (BODY_SIZE - 4) / 12
constexpr uint32_t INTERNAL_MAX_KEYS = (BODY_SIZE - sizeof(int32_t)) /
                                        (sizeof(int64_t) + sizeof(int32_t));

// ---- Page ------------------------------------------------------------------
//
// A Page object IS the 4KB buffer. There is no separate serialize/deserialize
// step: BufferManager reads/writes `data` directly with a single read()/write()
// syscall. All accessor methods below are just pointer arithmetic into `data`.
class Page {
public:
    char data[PAGE_SIZE];   // The raw page buffer - exactly what's on disk.

    Page();

    // Initializes an empty page of the given type with the given id.
    void init(PageType type, int32_t pageID);

    PageHeader*       header();
    const PageHeader* header() const;

    bool isLeaf() const;
    bool isInternal() const;
    bool isFull() const;   // true if this page cannot accept one more key

    // ---- Leaf body accessors ----
    // Returns a reference directly into the page buffer (zero-copy read/write).
    int64_t& leafKey(uint32_t idx);
    int64_t& leafValue(uint32_t idx);

    // ---- Internal body accessors ----
    int64_t& internalKey(uint32_t idx);
    int32_t& internalChild(uint32_t idx);

    // ---- Meta page (page 0) accessors ----
    // Body layout for META page: [rootPageID:int32][pageCount:int32]
    int32_t& metaRootPageID();
    int32_t& metaPageCount();

private:
    char* body();               // pointer to first byte after the header
    const char* body() const;
};
