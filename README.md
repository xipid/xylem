# Xylem 🪵
> **The Multi-Model Embedded Database Engine designed for Flash Memory & AI Edge Devices.**

[![License](https://img.shields.io/badge/License-Apache--2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![GitBook Docs](https://img.shields.io/badge/docs-GitBook-green.svg)](https://xylem.gitbook.io)
[![Discord](https://img.shields.io/badge/Discord-Join%20Us-purple.svg)](https://discord.gg/s7Rg4DHuej)

---

Xylem is a lightweight, zero-dependency, serverless database engine written in C++17. Designed to run on everything from constrained microcontrollers (like ESP32 using raw SPI flash) to full-scale Linux application backends, Xylem collapses the boundary between different storage paradigms. 

Instead of combining **SQLite** (relational), **littlefs** (raw block/wear-leveling storage), **FAISS** (vector similarity), and **tar/zip** (file packaging), you can use Xylem to manage it all in one cohesive format.

---

## ⚡ Super Features

*   **💾 Raw Block Device Engine:** Wear-leveling and dynamic, wandering superblocks mean you can run Xylem on raw NOR flash without a filesystem partition table.
*   **🧠 Embedded Vector DB (HNSW):** Hierarchical Navigable Small World graphs persisted to disk, offering Top-K cosine similarity searches on-demand.
*   **🔗 Graph Traversal Engine:** Native graph pathing using custom query pipelines (`MATCH`, `FOLLOW`, `REPEATFOLLOW`, `UNTIL`).
*   **🛡️ Multi-Version Concurrency Control (MVCC):** Complete transactional isolation with atomic locks, rollbacks, and write-write conflict detection.
*   **📦 CAS Blob Deduplication:** Zero-overhead storage using Blake2b-128 Content Addressable Storage, supporting partial updates (`blob[+]` or `blob[offset:len]`).
*   **🔔 Reactive Pub/Sub:** Watch queries to trigger real-time callbacks on writes, or use ephemeral `VIRTUAL` columns for zero-disk message passing.
*   **🔑 Multi-Key Security:** On-the-fly encryption and decryption utilizing multiple global secure keys.

---

## 🚀 Getting Started

Xylem compiles into a single static library. If you are using PlatformIO (e.g. for ESP32 development), add it to your `platformio.ini`:

```ini
lib_deps =
    xipid/Xylem
```

### Simple Row CRUD
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

### High-Performance Vector Similarity Search
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
│   └── BlobStore.hpp   # CAS Blob store
├── src/Xylem/          # Implementation files
├── tests/              # Comprehensive test suites
├── dev/                # Development plans & specifications
└── docs/               # GitBook documentation files
```

---

## 💬 Community & Support

*   **Documentation:** Detailed guides are hosted at [xylem.gitbook.io](https://xylem.gitbook.io).
*   **Discord:** Join our developer community on [Discord](https://discord.gg/s7Rg4DHuej).
*   **Repository:** Report issues or pull requests on [GitHub](https://github.com/xipid/xylem).

## 📄 License

Xylem is released under the **Apache 2.0 License**. See [LICENSE](LICENSE) for more information.
