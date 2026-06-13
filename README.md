# Xylem 🪵
> **The Multi-Model Embedded Database Engine designed for Flash Memory & AI Edge Devices.**

[![License](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![GitBook Docs](https://img.shields.io/badge/docs-GitBook-green.svg)](https://xylem.gitbook.io)
[![Discord](https://img.shields.io/badge/Discord-Join%20Us-purple.svg)](https://discord.gg/s7Rg4DHuej)

---

Xylem is a lightweight, zero-dependency, serverless database engine written in C++17. Designed to run on everything from constrained microcontrollers (like ESP32 using raw SPI flash) to full-scale Linux application backends, Xylem collapses the boundary between different storage paradigms.

Instead of combining **SQLite** (relational), **littlefs** (raw block/wear-leveling storage), **FAISS** (vector similarity), and **tar/zip** (file packaging), you can use Xylem to manage it all in one cohesive, multi-model format.

---

## ⚡ Super Features: Every Taste of Xylem

Xylem provides a complete suite of storage, query, and relational capabilities, giving you all the tools of an enterprise database in a micro-footprint.

### 💾 1. Raw Block Engine & Flash Optimizations
*   **Zero-Overhead Wear Leveling:** Operates directly on block devices or raw NOR flash without a filesystem partition table.
*   **Wandering Superblocks:** Prevents block burn-out by dynamically relocating critical metadata.
*   **Automatic Vacuuming:** Shrinks files dynamically and reclaims empty tail-end space efficiently.
*   **Memory Swapping:** Use `WRITEVOLATILE` to seamlessly drop ephemeral memory onto the block device like a swap file, which auto-clears on restart.

### 🧠 2. Embedded Vector DB (HNSW)
*   **On-Disk HNSW Graphs:** State-of-the-art Hierarchical Navigable Small World graphs stored directly on the block device.
*   **AI Edge Ready:** Perform instant Top-K cosine similarity searches on 128/256/512+ dimension embeddings locally.

### 🔗 3. Graph Traversal Engine
*   **Native Graph Pathing:** Relational matching using highly optimized graph pipelines (`MATCH`, `FOLLOW`, `REPEATFOLLOW`, `UNTIL`).
*   **Batch Graph Mutations:** Update or remove entire relational trees instantaneously using standard `WRITE` or `RM` with graph path predicates.
*   **VFS Macros:** Built-in Virtual File System syntax to `CAT`, `TEE`, `CP`, `MV`, and range-slice tree-like folder structures.

### 🛡️ 4. Multi-Version Concurrency Control (MVCC) & Predicate Locking
*   **ACID Compliance & Predicate Locks:** Clause-based locking protecting queries (including future inserts/updates) instead of just static row IDs.
*   **Snapshot Versioning (MVCC):** Keeps track of historical row versions and tombstones, allowing readers to view consistent snapshots without blocking concurrent writers.
*   **Auto rollback:** Automatically roll back transaction changes on write-write conflict or manually via `ROLLBACK <txId>`.

### 📦 5. CAS Blob Deduplication & Physical Shredding
*   **Blake2b-128 Hashing:** Content Addressable Storage ensures identical blobs (files/assets) are only stored once on the disk.
*   **Blob Freezing/Thawing:** Use `FREEZE <pos>` to bypass copy-on-write overheads and lock binaries directly to flash addresses (e.g. for ESP32 bootloaders!).
*   **Physical Shredding (BURN):** Irrevocably erase matching rows and physically shred/overwrite associated blobs, automatically merging and rewriting diff-dependencies of remaining records.

### 🔔 6. Reactive Pub/Sub & Watchers
*   **Query Watchers:** Set up native callbacks via `WATCH WHERE ...` to trigger real-time updates when matching data is mutated.
*   **Virtual Ephemeral Columns:** Pass zero-disk messages instantly through the database engine without invoking the block allocator.

### 🔌 8. Query Overlays & Hooks
*   **Operation Interception:** Intercept, redirect, or mock database reads, writes, and custom operators using flexible callbacks (`onOverlayRead`, `onOverlayWrite`, etc.).

### 🛠️ 7. Unified Query Language & CLI
*   **Interactive REPL:** Use the `xy` binary as a full-fledged SQL-like shell to manage your database.
*   **Unified Syntax:** Perform CRUD and Graph Traversals (`READ`, `WRITE`), Pub/Sub (`PULL`, `WATCH`), and Admin tasks (`VACUUM`, `DESTROY`) from a single language.

---

## 🚀 Getting Started

Xylem compiles into a single static library. If you are using PlatformIO (e.g. for ESP32 development), add it to your `platformio.ini`:

```ini
lib_deps =
    xipid/Xylem
```

### 1. Launching the CLI
Xylem provides a built-in shell for rapid prototyping.
```bash
./build/xy dev/db.xy
```
```sql
> WRITE name=Alice role=Engineer id:generate=0
> READ id MATCH name=Alice FOLLOW reports_to=parent.id
> VACUUM
```

### 2. Using Xylem as a Virtual File System
Xylem provides native macro commands to ingest, navigate, and export folders directly to/from your Linux filesystem.
```bash
> IO /home/user/project my_project
> CD my_project
> LS
> OI . /tmp/exported_project
> VACUUM
```

### 3. Simple C++ Row CRUD
```cpp
#include <Xylem/Xylem.hpp>

Xylem::XylemEngine xm;
xm.config.deviceSize = 10 * 1024 * 1024; // 10MB
xm.config.blockSize = 4096;

// Bind to your filesystem or raw flash read/write drivers:
xm.config.onDeviceRead = [](u64 offset, u64 maxOffset) { ... };
xm.config.onDeviceWrite = [](u64 offset, String data) { ... };

xm.mount();

// Write structured rows (tables auto-generate schema signatures)
xm.write({
    {"name", "=", "Alice"},
    {"role", "=", "Engineer"},
    {"age", "=", "30"}
});

// Query data using SQL-like where clauses
auto results = xm.read({"name", "role"}, {WHERE("role", "=", "Engineer")});
// Results contain: [{"name": "Alice", "role": "Engineer"}]
```

### 3. High-Performance Vector Similarity Search
```cpp
// Generate a 128-dimensional embedding
String queryVec;
queryVec.allocate(128 * sizeof(f32));
// ... fill queryVec with float values ...

// Read top 5 nearest neighbors using cosine similarity
auto topMatches = xm.read({"id"}, {WHERE("embedding", "cos", queryVec)}, 5);
```

---

## 📂 Project Structure

```
├── include/Xylem/      # Public headers
│   ├── Xylem.hpp       # Main database engine
│   ├── HNSW.hpp        # HNSW vector database logic
│   ├── TableStore.hpp  # Relational/document storage
│   ├── BlobStore.hpp   # CAS Blob store
│   └── QueryParser.hpp # Unified Query Language
├── src/Xylem/          # Implementation files
├── src/xy.cpp          # Interactive CLI Runner
├── tests/              # Comprehensive test suites
└── docs/               # GitBook documentation files
```

---

## 💬 Community & Support

*   **Documentation:** Detailed guides are hosted at [xylem.gitbook.io](https://xylem.gitbook.io).
*   **Discord:** Join our developer community on [Discord](https://discord.gg/s7Rg4DHuej).
*   **Repository:** Report issues or pull requests on [GitHub](https://github.com/xipid/xylem).

## 📄 License

Xylem is released under the **Apache 2.0 License**. See [LICENSE](LICENSE) for more information.
