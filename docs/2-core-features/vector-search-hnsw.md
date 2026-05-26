# HNSW Vector Search

For AI-driven applications, Xylem embeds a state-of-the-art **HNSW (Hierarchical Navigable Small World)** vector index. It allows searching millions of high-dimensional embeddings (e.g. from OpenAI, CLIP, or BERT models) using **Cosine Similarity** directly on-device.

## Vector Columns
Any column containing binary data matching vector floats (e.g., float arrays) can be queried as a vector. 

```cpp
// Insert a 128-dimensional embedding
usz dim = 128;
String vecStr;
vecStr.allocate(dim * sizeof(f32));
f32* vec = reinterpret_cast<f32*>(vecStr.data());
// ... populate vec ...

xm.write({
    {"v_id", "=", "vector_01"},
    {"embedding", "=", vecStr}
});
```

---

## Top-K Cosine Similarity Search
To search for similar vectors, use the `cos` operator in a standard `WHERE` clause:

```cpp
// Find top 5 items closest to targetVecStr
auto topRows = xm.read(
    {"v_id"}, 
    {WHERE("embedding", "cos", targetVecStr)}, 
    5 // Limit to Top-5 results
);

for (size_t i = 0; i < topRows.size(); ++i) {
    // Print matched IDs
    printf("Match %d: ID %s\n", (int)i, topRows[i]["v_id"].data());
}
```

---

## Key Performance Designs

### 1. Lazy (On-Demand) Ingestion
Writing data into a database should be fast. If Xylem built the multi-layered HNSW index on every single insert, writes would suffer significant latency. 

Instead, Xylem performs **lazy ingestion**:
*   Writes are appended to the table store instantly as flat rows (takes microseconds).
*   The HNSW index is only built or updated **on-demand** when a cosine search query (`cos`) is executed.
*   Once triggered, it consumes uningested vectors, builds the HNSW graph layers, and caches them.

### 2. Physical HNSW Graph Persistence
Unlike vector libraries that keep indices entirely in-memory, Xylem's HNSW implementation **persists the graph nodes directly to disk**. 
*   When `flush()` or `unmount()` is called, all index node linkages are saved.
*   Upon database reboot, Xylem does not need to rebuild the index. It dynamically loads the nodes from disk on-demand (`fetchNode`) during similarity queries.

### 3. Graceful Deletions & Collapse
When a vector row is deleted via `remove()`, it is pruned from the HNSW indexing linkages immediately. Xylem safely restructures the graph without memory leaks or pointers collapsing, guaranteeing that a subsequent query returns correct results.

### 4. Malformed Vector Defense
If a row is accidentally written with incorrect dimensions (e.g., a 127-dimensional array instead of a 128-dimensional one), Xylem's query pipeline is fully guarded. During a similarity search, it will skip dimension-mismatched rows gracefully rather than causing segmentation faults.
