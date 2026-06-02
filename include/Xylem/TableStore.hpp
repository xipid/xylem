#ifndef XYLEM_TABLESTORE_HPP
#define XYLEM_TABLESTORE_HPP

#include <Xylem/Format.hpp>
#include <Xylem/Query.hpp>
#include <Xylem/BlockDevice.hpp>
#include <Xylem/Allocator.hpp>
#include <Collection/Map.hpp>
#include <Collection/Tree.hpp>
#include <Xylem/HNSW.hpp>
#include <Xi/Func.hpp>

namespace Xylem {

class BlobStore; // Forward declaration

struct ColumnSchema {
    String name;
    TypeTag type;
    EncodingTag encoding;
    u16 fixedSize = 0;
};

struct TableSchema {
    u16 tableId;
    Array<ColumnSchema> columns;
};

class RowNode : public Collection::TaggedTreeBranch {
public:
    Map<String, String> row;
    u64 rId;
    RowNode(u64 id, const Map<String, String>& r) : rId(id), row(r) {
        this->name = "Row";
    }
    virtual Collection::TreeItem* clone() const override {
        return new RowNode(rId, row);
    }
};

class BloomFilter {
public:
    Array<u8> bits;
    usz numBits;
    
    BloomFilter(usz numBits = 1024) : numBits(numBits) {
        bits.allocate((numBits + 7) / 8);
        for (usz i = 0; i < bits.size(); ++i) bits[i] = 0;
    }
    
    void add(const String& item) {
        u32 h1 = crc32(item);
        u32 h2 = h1 ^ 0x5bd1e995;
        bits[(h1 % numBits) / 8] |= (1 << ((h1 % numBits) % 8));
        bits[(h2 % numBits) / 8] |= (1 << ((h2 % numBits) % 8));
    }
    
    bool contains(const String& item) const {
        u32 h1 = crc32(item);
        u32 h2 = h1 ^ 0x5bd1e995;
        if ((bits[(h1 % numBits) / 8] & (1 << ((h1 % numBits) % 8))) == 0) return false;
        if ((bits[(h2 % numBits) / 8] & (1 << ((h2 % numBits) % 8))) == 0) return false;
        return true;
    }
};

class TableStore {
public:
    BlockDevice* device;
    Allocator* allocator;
    BlobStore* blobStore = nullptr; // Set by XylemEngine::mount()

    Array<TableSchema> schemas;
    
    usz currentMemoryBytes = 0;
    usz maxMemoryBytes = 2 * 1024 * 1024; // 2MB default for tests
    
    // In-memory representation for current operations. 
    u64 nextRowId = 1;
    Map<u64, Map<String, String>> allRows;
    Array<u64> allRowIds;
    
    struct LruNode {
        u64 id;
        u64 prev = 0xFFFFFFFFFFFFFFFFULL;
        u64 next = 0xFFFFFFFFFFFFFFFFULL;
    };
    Map<u64, LruNode> lruMap;
    u64 lruHead = 0xFFFFFFFFFFFFFFFFULL;
    u64 lruTail = 0xFFFFFFFFFFFFFFFFULL;

    Map<u16, Array<u64>> tableToRows;
    Map<u64, bool> volatileRows;
    
    Xi::Func<Map<String, String>*(u64)> fetchFromDisk;
    Xi::Func<void(u64, Map<String, String>*)> saveToDisk;
    
    Xi::Func<HNSW::Node*(u64)> fetchHNSWFromDisk;
    Xi::Func<void(u64, HNSW::Node*)> saveHNSWToDisk;
    Xi::Func<void(u64)> removeHNSWFromDisk;

    Array<u64> uningestedVectors;

    Xi::Func<String(const String&)> readBlob;
    Xi::Func<void(const String&, const String&)> writeBlob;
    
    HNSW* hnsw = nullptr; 

    Array<String>* globalKeys;

    // ─── MVCC ────────────────────────────────────────────────────────────────
    // Per-row modification sequence for conflict detection.
    // Updated on every write (transactional or not).
    Map<u64, u64> rowModSeq;
    u64 currentSeq = 0; // Mirrors journal sequence

    TableStore(BlockDevice* dev, Allocator* alloc, Array<String>* keys = nullptr);
    ~TableStore();

    void loadSchemas();
    void saveSchemas();

    u16 findOrCreateTable(const Array<Clause>& columns);

    // ─── CRUD (returns 0=ok, >0=ASSERT index, -1=error) ─────────────────────

    Array<Map<String, String>> read(const Array<String>& columns, const Array<Clauses>& clauses,
                                     u64 length = 0, bool tombstones = false,
                                     u64 snapshotSeq = 0, u64 txId = 0);
    
    int write(const Array<Clause>& columns, const Array<Clauses>& clauses = Array<Clauses>(),
              const String& encryptionKey = "", u64 txId = 0, bool isVolatile = false);
    bool remove(const Array<Clauses>& clauses, u64 length = 0);
    
    f32 evaluateClauses(const Map<String, String>& row, const Array<Clauses>& clausesGroups, const Map<String, String>* parentRow = nullptr, u64 rId = 0);
    f32 evaluateClause(const Map<String, String>& row, const Clause& clause, const Map<String, String>* parentRow = nullptr, u64 rId = 0);

    Array<u64> resolvePathPattern(const String& colName, const String& pathPattern, u64 snapshotSeq, u64 txId);
    void resolvePathsInClauses(const Array<Clauses>& clauses, u64 snapshotSeq, u64 txId);
    void rebuildBloomFilters();

    void updateLru(u64 id);
    void evictIfNeeded();
    Map<String, String>* fetchRow(u64 id);

    // Persist all in-memory rows to BlobStore (call on flush/unmount)
    void flushAllRows();
    // Persist HNSW nodes to disk
    void flushHnsw();

    // ─── MVCC helpers ────────────────────────────────────────────────────────

    // Returns true if rowId is visible under the given snapshot, false otherwise.
    // snapshotSeq=0, txId=0 means non-transactional (see all committed rows).
    bool isVisibleToSnapshot(u64 rowId, u64 snapshotSeq, u64 txId);

    // MVCC conflict check: returns true if any rowId in touchedIds was modified after snapshotSeq
    bool hasConflicts(const Array<u64>& touchedIds, u64 snapshotSeq);

    // GC: remove old versions no active transaction can see
    void gcVersions(u64 oldestActiveSnapshot);

    // ─── Blob helpers ────────────────────────────────────────────────────────

    // Resolve a column value: if it's a blob ref, fetch the blob content
    String resolveValue(const String& val);

    // Check if any row (except excludeRowId) still references this blob hash
    bool isBlobReferenced(const String& hash, u64 excludeRowId = 0xFFFFFFFFFFFFFFFFULL);

    // Collect blob hashes from a row's values and clean up unreferenced ones
    void cleanupBlobRefs(const Map<String, String>& row, u64 excludeRowId = 0xFFFFFFFFFFFFFFFFULL);

    Map<String, u32> blobRefCounts;
    void incrementBlobRef(const String& hash);
    void decrementBlobRef(const String& hash);

    // Apply a partial blob operation (range write)
    String applyBlobRange(const String& existingContent, const String& newData, const BlobRange& range);

    // Lazy per-column hash index for '=' queries
    // Key: column name → Map of (value → Array of row IDs)
    Map<String, Map<String, Array<u64>>> colHashIndex;
    bool colHashIndexDirty = true; // Mark dirty on any write/remove
    bool colBloomFiltersDirty = true;
    Map<String, BloomFilter*> colBloomFilters;
    Map<String, Array<u64>> precomputedPaths;
    bool disableIndex = false;

    void rebuildColHashIndex();
    void invalidateColHashIndex();

    Map<u64, bool> dirtyBlocks;
    bool loadBlock(u64 blockId);
    void serializeBlock(u64 blockId, String& out, bool volatileOnly = false);
    void deserializeBlock(u64 blockId, const String& in, bool isVolatile = false);

private:
};

} // namespace Xylem

#endif // XYLEM_TABLESTORE_HPP
