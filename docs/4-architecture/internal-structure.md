# Internal Structure

Xylem is organized into distinct functional layers that separate memory allocation, hardware devices, database logic, and query compilation.

```
xylem/
├── CMakeLists.txt        # Build system configuration
├── library.json          # PlatformIO library configuration
├── include/
│   └── Xylem/            # Public Headers
└── src/
    └── Xylem/            # Implementation Files
```

---

## Module Layout

### 1. Hardware Abstraction Layer
*   `BlockDevice.hpp` / `BlockDevice.cpp`
    *   Defines the `BlockDevice` interface and custom read/write hooks. Simulated as POSIX files or memory blocks.
*   `Allocator.hpp` / `Allocator.cpp`
    *   Handles low-level physical block allocation and reclaiming. Implements wear-leveling algorithms.
*   `Format.hpp`
    *   Sets structural layouts, block headers, and geometry boundaries.

### 2. Relational & Schema Core
*   `TableStore.hpp` / `TableStore.cpp`
    *   Creates tables dynamically from signatures. Handles page management, row insertions, edits, deletions (tombstones), and MVCC snapshots.
*   `Cache.hpp` / `Cache.cpp`
    *   Maintains a dirty-page cache using an LRU eviction strategy. Flushes modified blocks back to the physical layer.

### 3. BLAKE2b CAS Storage
*   `BlobStore.hpp` / `BlobStore.cpp`
    *   Manages the Content-Addressable Blob database. Stores files based on their BLAKE2b hashes, keeps reference tracking, and handles garbage collection.

### 4. Search & Vector Core
*   `HNSW.hpp` (header-only)
    *   Calculates vector graphs and performs Hierarchical Navigable Small World clustering. Executes fast Cosine Similarity lookups.
*   `CryptItem.hpp`
    *   Handles row-level encryption pipelines using AES/symmetric encryption tools.

### 5. Queries & CLI
*   `Query.hpp` / `QueryParser.hpp` / `QueryParser.cpp`
    *   Parses incoming query strings into operational pipelines (like `MATCH`, `FOLLOW`, `ASSERT`). Supports comment sanitization (`#` and `//`).
*   `Watcher.hpp` / `Watcher.cpp`
    *   Tracks query conditional channels, registering pull/push callbacks.
*   `Xylem.hpp` / `Xylem.cpp`
    *   The top-level orchestrator. Houses `XylemEngine` and integrates all stores and drivers.
*   `xy.cpp` (CLI Binary)
    *   Interactive terminal application for managing databases, transferring files via `IO`/`OI`, and running queries.
