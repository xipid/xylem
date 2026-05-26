#ifndef XYLEM_XYLEM_HPP
#define XYLEM_XYLEM_HPP

#include <Xylem/Format.hpp>
#include <Xylem/BlockDevice.hpp>
#include <Xylem/Allocator.hpp>
#include <Xylem/Journal.hpp>
#include <Xylem/TableStore.hpp>
#include <Xylem/BlobStore.hpp>
#include <Xylem/Cache.hpp>
#include <Xylem/Watcher.hpp>
#include <Xylem/QueryParser.hpp>

namespace Xylem {

class XylemEngine {
public:
    DeviceConfig config;
    usz maxCache = 1024 * 1024;
    Array<String> globalKeys;
    void addKey(const String& key) { globalKeys.push(key); }

    u8 pinnedRawBits[1024];
    u32 currentSuperblockIdx = 0xFFFFFFFFu;

    BlockDevice* device = nullptr;
    Allocator* allocator = nullptr;
    Journal* journal = nullptr;
    TableStore* tableStore = nullptr;
    BlobStore* blobStore = nullptr;
    Cache* cache = nullptr;
    Watcher* watcher = nullptr;

    XylemEngine();
    ~XylemEngine();

    bool format();
    bool mount();
    void unmount();
    void flush();

    // ─── Query Parser ────────────────────────────────────────────────────────
    QueryResult query(const String& queryString, const Array<String>& sanitized = Array<String>());

    // ─── Database ────────────────────────────────────────────────────────────
    // Returns: 0 = success, >0 = ASSERT clause index, -1 = locked/error, -2 = MVCC conflict

    Array<Map<String,String>> read(const Array<String>& columns, const Array<Clauses>& clauses,
                                    u64 length = 0, bool tombstones = false, u64 txId = 0);
    int write(const Array<Clause>& columns, const Array<Clauses>& clauses = Array<Clauses>(),
              u64 txId = 0, const String& encryptionKey = "");
    int writeVolatile(const Array<Clause>& columns, const Array<Clauses>& clauses = Array<Clauses>(),
                      const String& encryptionKey = "");
    bool remove(const Array<Clauses>& clauses, u64 length = 0, u64 as = 0);
    
    // Advanced Graph Traversal
    Collection::TreeBranch* graphRead(const Array<String>& columns, const Array<GraphOp>& ops,
                                       u64 limit = 0, u64 txId = 0);
    int graphWrite(const Array<GraphOp>& ops, u64 txId = 0, const String& encryptionKey = "");
    int graphWriteVolatile(const Array<GraphOp>& ops, const String& encryptionKey = "");

    // Transactions (MVCC)
    // Returns: 0 = success, -1 = invalid lock, -2 = MVCC conflict (auto-rolled-back)
    u64 lock(const Array<Clauses>& clauses = Array<Clauses>(), u64 id = 0);
    bool rollback(u64 id);
    int  unlock(u64 id);

    // ─── Blob API ────────────────────────────────────────────────────────────

    // Auto-hash content, CAS dedup. Returns the 16-byte hash.
    String writeHash(const String& content);
    // Store with user-provided hash. Returns the hash.
    String writeHash(const String& content, const String& hash);
    // Write content at a fixed position. Returns hash. Rejects if overlap.
    String writeHash(const String& content, u64 position);

    // Pin existing blob at byte address. Returns 0=ok, -1=overlap/error.
    int fixHash(const String& hash, u64 position);
    // Force remove blob.
    bool removeHash(const String& hash);
    // Read blob content.
    String readHash(const String& hash, u64 min = 0, u64 max = 0);
    // Stat blob (returns original size as string).
    String statHash(const String& hash);

    // Pin raw data at block address (relocating system blocks if needed)
    bool fixRaw(u64 byteAddress, const String& rawData);

    // ─── Reactivity ─────────────────────────────────────────────────────────

    u64 watch(const Array<Clauses>& clauses);
    u64 watch(const Array<Clauses>& clauses, Func<void(Map<String,String>)> cb);
    bool unwatch(u64 id);
    Array<Map<String,String>> pull(u64 id);
};

} // namespace Xylem

#endif // XYLEM_XYLEM_HPP
