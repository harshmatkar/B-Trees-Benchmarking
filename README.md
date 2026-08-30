# Disk-Backed B+ Tree Indexing Engine

A from-scratch, disk-backed B+ tree storage engine in C++17. No external
database dependencies — only the standard library. Built as a systems-level
portfolio project demonstrating page-based storage, buffer pool management,
and index structures the way real databases (SQLite, InnoDB, Postgres) build
them internally.

## What this actually implements

- **Fixed 4KB page format** (`Page.h`) — a byte-exact 17-byte header +
  fixed-array body, holding either leaf entries (key, value) or internal
  routing entries (key, child pageID). Capacities (254 leaf keys / 339
  internal keys) are computed at compile time from `PAGE_SIZE`.
- **LRU buffer pool** (`BufferManager.h/.cpp`) — bounded in-memory page
  cache backed by a single flat file `database.db`. Dirty pages are only
  flushed on eviction or shutdown; a reserved page 0 stores the root
  pointer + page count so the database survives process restarts.
- **B+ Tree core** (`BPlusTree.h/.cpp`) — top-down insertion with
  path-tracked bottom-up splitting (no stored parent pointers needed),
  leaf-to-leaf doubly linked list for range scans, O(log N) search.
- **CLI + benchmark** (`main.cpp`) — builds N synthetic records, then
  times random point lookups through the B+ tree against a full linear
  scan of an equivalent flat file, to make the indexing win concrete.

## Directory structure

```
bptree-index-engine/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── Page.h
│   ├── BufferManager.h
│   └── BPlusTree.h
└── src/
    ├── Page.cpp
    ├── BufferManager.cpp
    ├── BPlusTree.cpp
    └── main.cpp
```

## Build (WSL / any Linux, zero dependencies beyond g++ and cmake)

```bash
# one-time, inside your WSL Ubuntu shell:
sudo apt update && sudo apt install -y build-essential cmake

cd bptree-index-engine
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j

# binary lands at bptree-index-engine/bin/bptree_cli
```

If you don't want CMake at all, a single direct g++ invocation also works:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude \
    src/Page.cpp src/BufferManager.cpp src/BPlusTree.cpp src/main.cpp \
    -o bptree_cli
```

## Run

```bash
cd bin   # or wherever your binary landed

# One-shot: build a 100,000-record DB and benchmark 2,000 random lookups
./bptree_cli bench 100000 2000

# Or drive it piece by piece:
./bptree_cli build 100000        # creates database.db + records.dat
./bptree_cli search 54321        # point lookup
./bptree_cli range 100 200       # inclusive range scan

# Or just run with no args for an interactive menu
./bptree_cli
```

Sample benchmark output (100K records, 2K random lookups, measured in this
project's own test run):

```
Method                      Total time      Avg per lookup    Found
------------------------------------------------------------------
B+ Tree (indexed)           2.53 ms         1.27 us           2000 / 2000
Sequential scan (no index)  3388.05 ms      1694.03 us        2000 / 2000
------------------------------------------------------------------
Speedup: B+ Tree is 1337.7x faster than a full sequential scan
```

## Design notes worth calling out in an interview / README write-up

- **Fixed-array vs slotted pages**: since keys/values are fixed-width
  (`int64_t`), a slotted-page directory would add indirection for no
  benefit — offsets are computed directly. A natural "next step" extension
  is variable-length keys, which *would* justify a slot directory.
- **No stored parent pointers**: instead, `insert()` records the root-to-leaf
  path of internal page IDs during descent, then walks it backwards to
  propagate splits upward. This avoids the classic bug class of parent
  pointers going stale after a page is reused, at the cost of one
  `std::vector<int32_t>` per insert.
- **Eviction-safe pointer handling**: `BufferManager::allocatePage()` can
  trigger an LRU eviction, which invalidates any `Page*` obtained before
  the call. Every split routine allocates first, then **re-fetches** every
  page pointer it still needs — this is exercised directly by an 8-page
  buffer pool stress test (20,000 inserts, constant eviction) during
  development.
- **Durability**: `database.db` is a real flat binary file; killing the
  process and reopening it reloads the root pointer from page 0 and all
  data is intact (open the same file with `build`/`search`/`range` again
  to see this).

## Possible extensions (good "future work" talking points)

- WAL-based crash recovery (currently: no crash safety mid-write, only
  flush-on-shutdown durability).
- Deletion with leaf/internal merging & rebalancing (currently insert +
  search + range only, which covers the read/write path interviewers
  usually probe).
- Concurrent access (currently single-threaded; no latching).
- Variable-length keys via slotted pages.
- Generic key/value types via templates instead of `int64_t`.
