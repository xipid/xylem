# Introduction to Xylem 🪵

Xylem is a revolutionary **multi-model embedded database** engine built from the ground up in modern C++17. It is designed to combine several core engineering concepts that traditionally require separate libraries:

*   **Transactional MVCC Row Store** (Alternative to SQLite)
*   **Hierarchical Graph Database** (Alternative to Neo4j/custom graph stores)
*   **Vector Search Similarity Index** (Alternative to FAISS/Pinecone)
*   **Content-Addressable Blob Storage** (Alternative to custom file containers)
*   **Raw Flash Wear-Leveling Layer** (Alternative to littlefs/SPIFFS)

By packing all these components into a single format, Xylem provides a unified and highly robust data container.

## Why Xylem?

When developing software for edge devices, embedded systems (like ESP32/ARM), or local desktop tools, you often need to store different types of data:
1.  **System Logs and Configuration:** Best stored in relational tables.
2.  **Files, Images, Assets:** Best stored in a blob store with deduplication.
3.  **AI Embeddings:** Best stored in a vector index for fast similarity search.
4.  **Complex Directories or Networks:** Best stored in a graph.

Normally, developers are forced to layer multiple database libraries and virtual file systems on top of each other. This increases firmware size, duplicates memory budgets, and complicates crash consistency.

**Xylem solves this by merging all storage backends into a single raw block manager.**

## Core Links

*   **GitHub Repository:** [xipid/xylem](https://github.com/xipid/xylem)
*   **Discord Server:** [Join our Discord](https://discord.gg/s7Rg4DHuej)
*   **PlatformIO Registry:** `xipid/Xylem`
*   **License:** Apache 2.0
