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
#include <functional>
#include <stdio.h>

namespace Xylem {

class XylemEngine {
public:
    DeviceConfig config;
    usz maxCache = 1024 * 1024;
    void addKey(const String& key) { globalKeys.push(key); }

    BlockDevice* device = nullptr;

    XylemEngine();
    ~XylemEngine();

    bool format();
    bool mount();
    void destroy();
    void flush();
    bool isMounted() const;

    // ─── Query Parser ────────────────────────────────────────────────────────
    QueryResult query(const String& queryString, const Array<String>& sanitized = Array<String>());

    // ─── Database ────────────────────────────────────────────────────────────
    // Returns: 0 = success, >0 = ASSERT clause index, -1 = locked/error, -2 = MVCC conflict

    Array<Map<String,String>> read(const Array<String>& columns, const Array<Clauses>& clauses,
                                    u64 length = 0, u64 page = 0, bool tombstones = false, u64 txId = 0,
                                    bool readAllColumns = false);
    int write(const Array<Clause>& columns, const Array<Clauses>& clauses = Array<Clauses>(),
              u64 txId = 0, const String& encryptionKey = "");
    int writeVolatile(const Array<Clause>& columns, const Array<Clauses>& clauses = Array<Clauses>(),
                      u64 txId = 0, const String& encryptionKey = "");
    bool rm(const Array<Clauses>& clauses, u64 length = 0, u64 as = 0, bool burn = false);
    bool burn(const Array<Clauses>& clauses, u64 length = 0, u64 as = 0) { return rm(clauses, length, as, true); }


    // Transactions (MVCC)
    // Returns: 0 = success, -1 = invalid lock, -2 = MVCC conflict (auto-rolled-back)
    u64 lock(const Array<Clauses>& clauses = Array<Clauses>(), u64 id = 0, bool requiresExplicitAs = true);
    u64 commit(const Array<Clauses>& clauses = Array<Clauses>(), u64 id = 0);
    bool rollback(u64 id);
    int  unlock(u64 id);

    String generateId(const String& column);

    // ─── Blob API (ref-based) ────────────────────────────────────────────────

    u32 getBlobRef(const String& hash);
    String getBlob(u32 ref);
    u32 getBlobSize(u32 ref);
    void writeBlob(u32 ref, const String& content, u64 start = 0);
    String readBlob(u32 ref, u64 start = 0, u64 end = 0);
    void setBlob(u32 ref, const String& hash);
    String setBlob(u32 ref);
    bool freezeBlob(u64 position, u32 blobRef);
    void thawBlob(u32 blobRef);

    // Internal hash-based blob (kept for engine internals)
    String writeHash(const String& content);
    String writeHash(const String& content, const String& hash);
    String writeHash(const String& content, u64 position);
    int fixHash(const String& hash, u64 position);
    bool removeHash(const String& hash);
    String readHash(const String& hash, u64 min = 0, u64 max = 0);

    // Pin raw data at block address (relocating system blocks if needed)
    bool fixRaw(u64 byteAddress, const String& rawData);

    // ─── Vacuum / Freeze / Thaw ─────────────────────────────────────────────

    void vaccum();                                  // Shrink from end
    bool vaccum(u64 startPos);                      // Vaccum from startPos to end
    bool vaccum(u64 startPos, u64 endPos);           // Vaccum region
    
    u64 getUnusedBlockSpace();                       // Count unused space from the end of the device

    bool freeze(u64 startPos, u64 endPos);           // Prevent Xylem from writing here
    bool thaw(u64 startPos, u64 endPos);             // Allow Xylem to write here again
    bool freeze(u64 startPos, const String& data);   // Freeze + write raw data (replaces fixRaw public use)

    // ─── File-like convenience API ──────────────────────────────────────────

    QueryResult cat(const String& path, u64 start = 0, u64 end = 0);
    QueryResult tee(const String& path, const String& content, u64 start = 0, u64 end = 0);
    QueryResult ls(const String& path = "");
    bool rm(const String& path);
    QueryResult cp(const String& src, const String& dst);
    QueryResult mv(const String& src, const String& dst);

    // ─── Reactivity ─────────────────────────────────────────────────────────

    u64 watch(const Array<Clauses>& clauses);
    u64 watch(const Array<Clauses>& clauses, Func<void(Map<String,String>)> cb);
    bool unwatch(u64 id);
    Array<Map<String,String>> pull(u64 id);

    TableStore* getTableStore() { return tableStore; }
    usz getActiveLocksCount() { return journal ? journal->activeLocks.size() : 0; }
    void printActiveLocks() {
        if (!journal) { printf("No journal\n"); return; }
        printf("Active locks (%d):\n", (int)journal->activeLocks.size());
        for (auto it = journal->activeLocks.begin(); it != journal->activeLocks.end(); ++it) {
            printf("  ID: %llu, requiresExplicitAs: %d, clauses: %d\n", 
                   (unsigned long long)it->key, 
                   it->value.requiresExplicitAs ? 1 : 0,
                   (int)it->value.lockClauses.size());
        }
    }

    // ─── Query Overlay Registrations ──────────────────────────────────────────
    struct OverlayReadRegistration {
        Array<Clauses> clauses;
        bool supportsPath;
        std::function<Array<Map<String, String>>(const Array<Clauses>& clauses, const Array<Clauses>& asserts)> callback;
    };
    struct OverlayWriteRegistration {
        Array<Clauses> clauses;
        std::function<bool(const Array<Clause>& columns, const Array<Clauses>& clauses)> callback;
    };
    struct ClauseOperationRegistration {
        String op;
        std::function<bool(const Clause& clause, const Map<String, String>& row, bool& result)> callback;
    };
    struct QueryRegistration {
        String op;
        std::function<QueryResult(const String& queryString, XylemEngine* engine)> callback;
    };

    Array<OverlayReadRegistration> overlayReads;
    Array<OverlayWriteRegistration> overlayWrites;
    Array<ClauseOperationRegistration> clauseOperations;
    Array<QueryRegistration> queryCallbacks;

    void onOverlayRead(const Array<Clauses>& clauses, bool supportsPath, std::function<Array<Map<String, String>>(const Array<Clauses>&, const Array<Clauses>&)> cb) {
        OverlayReadRegistration reg;
        reg.clauses = clauses;
        reg.supportsPath = supportsPath;
        reg.callback = cb;
        overlayReads.push(reg);
    }
    void onOverlayWrite(const Array<Clauses>& clauses, std::function<bool(const Array<Clause>&, const Array<Clauses>&)> cb) {
        OverlayWriteRegistration reg;
        reg.clauses = clauses;
        reg.callback = cb;
        overlayWrites.push(reg);
    }
    void onClauseOperation(const String& op, std::function<bool(const Clause&, const Map<String, String>&, bool&)> cb) {
        ClauseOperationRegistration reg;
        reg.op = op;
        reg.callback = cb;
        clauseOperations.push(reg);
    }
    void onQuery(const String& op, std::function<QueryResult(const String&, XylemEngine*)> cb) {
        QueryRegistration reg;
        reg.op = op;
        reg.callback = cb;
        queryCallbacks.push(reg);
    }

private:
    friend class QueryParser;
    friend class XylemServer;
    friend class TableStore;

    u8 pinnedRawBits[1024];
    u32 currentSuperblockIdx = 0xFFFFFFFFu;

    Allocator* allocator = nullptr;
    Journal* journal = nullptr;
    TableStore* tableStore = nullptr;
    BlobStore* blobStore = nullptr;
    Cache* cache = nullptr;
    Watcher* watcher = nullptr;

    Array<String> globalKeys;

    void ensureMounted();
    bool isWriteBlocked(u64 txId);
    void mergeUnusedDiffs();
};

} // namespace Xylem

#endif // XYLEM_XYLEM_HPP
