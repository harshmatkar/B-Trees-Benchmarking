// =============================================================================
// main.cpp
//
// CLI + benchmark harness for the disk-backed B+ tree engine.
//
// Usage:
//   ./bptree_cli                         interactive menu
//   ./bptree_cli build   [N]              build a fresh DB with N random records (default 100000)
//   ./bptree_cli search  <key>            point lookup
//   ./bptree_cli range   <start> <end>    inclusive range query
//   ./bptree_cli bench   [N] [lookups]    build N records, then benchmark `lookups`
//                                         random B+Tree point lookups vs a full
//                                         unindexed sequential scan of a flat file
//                                         holding the same records.
// =============================================================================

#include "BufferManager.h"
#include "BPlusTree.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <random>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <optional>
#include <string>
#include <cstdlib>

using Clock = std::chrono::high_resolution_clock;

static double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static const std::string DB_FILE      = "database.db";
static const std::string RECORDS_FILE = "records.dat"; // flat, unindexed copy of the same data

// -----------------------------------------------------------------------------
// The "unindexed baseline": a plain flat file of (int64 key, int64 value)
// pairs in insertion order, with NO index structure at all. Looking up a key
// means reading the file from the front until we find it (or hit EOF) -
// exactly what a naive "grep through a CSV" approach costs.
// -----------------------------------------------------------------------------
static void appendRecordToFlatFile(std::ofstream& out, int64_t key, int64_t value) {
    out.write(reinterpret_cast<const char*>(&key), sizeof(key));
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

static std::optional<int64_t> sequentialScanSearch(const std::string& path, int64_t key) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    int64_t k, v;
    while (in.read(reinterpret_cast<char*>(&k), sizeof(k))) {
        in.read(reinterpret_cast<char*>(&v), sizeof(v));
        if (k == key) return v;
    }
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// Builds a fresh database.db + records.dat pair with `count` synthetic,
// shuffled records so insertion order isn't already sorted (a much more
// realistic/adversarial test of the split logic than sequential keys).
// -----------------------------------------------------------------------------
static void buildDatabase(int64_t count) {
    std::remove(DB_FILE.c_str());
    std::remove(RECORDS_FILE.c_str());

    std::vector<int64_t> keys(count);
    for (int64_t i = 0; i < count; i++) keys[i] = i + 1;

    std::mt19937_64 rng(42); // fixed seed -> reproducible benchmark runs
    std::shuffle(keys.begin(), keys.end(), rng);

    BufferManager bm(DB_FILE, /*poolSizeInPages=*/512);
    BPlusTree tree(bm);
    std::ofstream flat(RECORDS_FILE, std::ios::binary);

    auto start = Clock::now();
    for (int64_t i = 0; i < count; i++) {
        int64_t key   = keys[i];
        int64_t value = key * 10; // synthetic "record offset"
        tree.insert(key, value);
        appendRecordToFlatFile(flat, key, value);

        if (count >= 10 && (i + 1) % (count / 10) == 0) {
            std::cout << "  inserted " << (i + 1) << " / " << count << "\r" << std::flush;
        }
    }
    flat.close();
    bm.flushAll();
    auto end = Clock::now();

    std::cout << "\nBuilt database with " << count << " records in "
              << std::fixed << std::setprecision(2) << elapsedMs(start, end) << " ms\n";

    auto stats = tree.computeStats();
    std::cout << "Tree height: " << stats.height
              << " | internal pages: " << stats.internalPages
              << " | leaf pages: " << stats.leafPages
              << " | total pages: " << stats.totalPages
              << " (" << (stats.totalPages * PAGE_SIZE) / (1024 * 1024) << " MB on disk)\n";
    std::cout << "Buffer pool: " << bm.getResidentPageCount() << " / " << bm.getPoolSize()
              << " pages resident | disk reads: " << bm.diskReads
              << " | disk writes: " << bm.diskWrites << "\n";
}

// -----------------------------------------------------------------------------
// Runs `lookups` random point queries through BOTH the B+ tree and the
// unindexed flat-file scan, and prints a side-by-side comparison.
// -----------------------------------------------------------------------------
static void runBenchmark(int64_t recordCount, int64_t lookups) {
    BufferManager bm(DB_FILE, 512);
    BPlusTree tree(bm);

    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<int64_t> dist(1, recordCount);
    std::vector<int64_t> queryKeys(lookups);
    for (auto& k : queryKeys) k = dist(rng);

    // ---- B+ Tree lookups ----
    size_t found = 0;
    auto t0 = Clock::now();
    for (int64_t k : queryKeys) {
        if (tree.search(k).has_value()) found++;
    }
    auto t1 = Clock::now();
    double treeMs = elapsedMs(t0, t1);

    // ---- Unindexed sequential scan lookups ----
    // A full 100k-record x however-many-lookups scan is slow by design;
    // if the caller asked for a lot of lookups against a huge file this
    // could take a while, which is precisely the point being demonstrated.
    size_t foundScan = 0;
    auto t2 = Clock::now();
    for (int64_t k : queryKeys) {
        if (sequentialScanSearch(RECORDS_FILE, k).has_value()) foundScan++;
    }
    auto t3 = Clock::now();
    double scanMs = elapsedMs(t2, t3);

    std::cout << "\n===================== BENCHMARK RESULTS =====================\n";
    std::cout << "Dataset size:            " << recordCount << " records\n";
    std::cout << "Random lookups executed: " << lookups << "\n\n";

    std::cout << std::left << std::setw(28) << "Method"
              << std::setw(16) << "Total time"
              << std::setw(18) << "Avg per lookup"
              << "Found\n";
    std::cout << std::string(66, '-') << "\n";

    std::cout << std::left << std::setw(28) << "B+ Tree (indexed)"
              << std::setw(16) << (std::to_string(treeMs) + " ms")
              << std::setw(18) << (std::to_string(treeMs / lookups * 1000.0) + " us")
              << found << " / " << lookups << "\n";

    std::cout << std::left << std::setw(28) << "Sequential scan (no index)"
              << std::setw(16) << (std::to_string(scanMs) + " ms")
              << std::setw(18) << (std::to_string(scanMs / lookups * 1000.0) + " us")
              << foundScan << " / " << lookups << "\n";

    std::cout << std::string(66, '-') << "\n";
    if (treeMs > 0) {
        std::cout << "Speedup: B+ Tree is " << std::fixed << std::setprecision(1)
                  << (scanMs / treeMs) << "x faster than a full sequential scan\n";
    }
    auto stats = tree.computeStats();
    std::cout << "(B+ tree height = " << stats.height << ", so each lookup touches at most "
              << stats.height << " pages instead of scanning all " << recordCount << " records)\n";
    std::cout << "===============================================================\n";
}

static void runRangeDemo(int64_t startKey, int64_t endKey) {
    BufferManager bm(DB_FILE, 512);
    BPlusTree tree(bm);

    auto t0 = Clock::now();
    auto results = tree.rangeQuery(startKey, endKey);
    auto t1 = Clock::now();

    std::cout << "Range [" << startKey << ", " << endKey << "] -> "
              << results.size() << " records in " << elapsedMs(t0, t1) << " ms\n";

    size_t previewCount = std::min<size_t>(10, results.size());
    for (size_t i = 0; i < previewCount; i++) {
        std::cout << "  " << results[i].first << " -> " << results[i].second << "\n";
    }
    if (results.size() > previewCount) std::cout << "  ... (" << results.size() - previewCount << " more)\n";
}

static void runSearch(int64_t key) {
    BufferManager bm(DB_FILE, 512);
    BPlusTree tree(bm);

    auto t0 = Clock::now();
    auto val = tree.search(key);
    auto t1 = Clock::now();

    if (val.has_value()) {
        std::cout << "FOUND key=" << key << " -> value=" << *val
                  << "  (" << elapsedMs(t0, t1) << " ms, "
                  << bm.diskReads << " disk reads)\n";
    } else {
        std::cout << "key=" << key << " NOT FOUND ("
                  << elapsedMs(t0, t1) << " ms)\n";
    }
}

static void printMenu() {
    std::cout << "\n=== Disk-Backed B+ Tree Indexing Engine ===\n"
              << "1) Build fresh database (100,000 records)\n"
              << "2) Point lookup\n"
              << "3) Range query\n"
              << "4) Benchmark: B+ Tree vs sequential scan\n"
              << "5) Exit\n"
              << "Choose an option: ";
}

static void interactiveMenu() {
    while (true) {
        printMenu();
        int choice;
        if (!(std::cin >> choice)) return;

        if (choice == 1) {
            std::cout << "How many records? ";
            int64_t n; std::cin >> n;
            buildDatabase(n);
        } else if (choice == 2) {
            std::cout << "Key to search: ";
            int64_t k; std::cin >> k;
            runSearch(k);
        } else if (choice == 3) {
            std::cout << "Start key: "; int64_t s; std::cin >> s;
            std::cout << "End key: ";   int64_t e; std::cin >> e;
            runRangeDemo(s, e);
        } else if (choice == 4) {
            std::cout << "Dataset size (records already built, for reporting): ";
            int64_t n; std::cin >> n;
            std::cout << "Number of random lookups: ";
            int64_t l; std::cin >> l;
            runBenchmark(n, l);
        } else if (choice == 5) {
            std::cout << "Goodbye.\n";
            return;
        } else {
            std::cout << "Invalid option.\n";
        }
    }
}

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);

    if (argc == 1) {
        interactiveMenu();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "build") {
        int64_t n = (argc > 2) ? std::atoll(argv[2]) : 100000;
        buildDatabase(n);
    } else if (cmd == "search") {
        if (argc < 3) { std::cerr << "usage: bptree_cli search <key>\n"; return 1; }
        runSearch(std::atoll(argv[2]));
    } else if (cmd == "range") {
        if (argc < 4) { std::cerr << "usage: bptree_cli range <start> <end>\n"; return 1; }
        runRangeDemo(std::atoll(argv[2]), std::atoll(argv[3]));
    } else if (cmd == "bench") {
        int64_t n = (argc > 2) ? std::atoll(argv[2]) : 100000;
        int64_t l = (argc > 3) ? std::atoll(argv[3]) : 1000;
        std::cout << "Building database with " << n << " records...\n";
        buildDatabase(n);
        runBenchmark(n, l);
    } else {
        std::cerr << "Unknown command: " << cmd << "\n"
                  << "Usage: bptree_cli [build [N] | search <key> | range <a> <b> | bench [N] [lookups]]\n";
        return 1;
    }
    return 0;
}
