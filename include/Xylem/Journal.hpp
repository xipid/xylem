#ifndef XYLEM_JOURNAL_HPP
#define XYLEM_JOURNAL_HPP

#include <Xylem/Format.hpp>
#include <Xylem/BlockDevice.hpp>
#include <Xylem/Allocator.hpp>
#include <Xylem/Query.hpp>
#include <Collection/Array.hpp>
#include <Collection/Map.hpp>

namespace Xylem {

using namespace Collection;

enum class JournalOpType : u8 {
    TABLE_WRITE = 0,
    TABLE_REMOVE = 1,
    BLOB_WRITE = 2,
    BLOB_REMOVE = 3,
    BLOCK_ALLOC = 4,
    BLOCK_FREE = 5,
    META_UPDATE = 6,
    LOCK_BEGIN = 7,
    LOCK_COMMIT = 8,
    LOCK_ROLLBACK = 9,
    HNSW_WRITE = 10,
    ROW_WRITE = 11
};

struct JournalEntry {
    JournalOpType opType;
    u16 targetBlock;
    String payload;
};

// A buffered write operation inside an active transaction.
struct PendingWrite {
    Array<Clause>   columns;
    Array<Clauses>  clauses;
    String          encryptionKey;
};

struct LockState {
    Array<JournalEntry> pendingEntries;
    Array<PendingWrite> pendingWrites; // Buffered writes (applied on commit)
    u64 snapshotSeq = 0;              // MVCC: journal sequence at lock time
    Array<u64> touchedRowIds;         // MVCC: row IDs read/written in this tx
};

class Journal {
public:
    BlockDevice* device;
    Allocator*   allocator;

    u64  currentSequence    = 0;
    u16  journalStartBlock  = 18;
    u16  journalBlockCount  = 4;
    u16  currentJournalBlock = 18;

    // In-memory locks
    Map<u64, LockState> activeLocks;
    u64 nextLockId = 1;

    Journal(BlockDevice* dev, Allocator* alloc);

    void initFromFormat(u16 startBlock, u16 count, u64 seq);
    void recover(class TableStore* ts); // Scans WAL on mount; replays pending writes into TableStore

    bool isNearingCapacity() const {
        return currentJournalBlock >= journalStartBlock + journalBlockCount - 1;
    }

    // Low-level atomic write-ahead
    void append(JournalOpType op, u16 targetBlock, const String& payload);
    void appendBatch(const Array<JournalEntry>& entries);

    // Transactional API
    u64  lockBegin();
    bool lockPendingWrite(u64 lockId, const PendingWrite& pw);
    bool lockAppend(u64 lockId, JournalOpType op, u16 targetBlock, const String& payload);
    bool lockCommit(u64 lockId);
    bool lockRollback(u64 lockId);

    // Track a row touched by a transaction (for MVCC conflict detection)
    void trackRow(u64 lockId, u64 rowId);

    // Get the oldest snapshot sequence across all active locks
    u64 oldestActiveSnapshot() const;

    // Serialization for WAL
    static String serializeTableWrite(const Array<Clause>& columns, const Array<Clauses>& clauses, const String& encryptionKey);
    static String serializeTableRemove(const Array<Clauses>& clauses, u64 length);
    static String serializeGraphWrite(const Array<GraphOp>& ops, const String& encryptionKey);
    static void deserializeAndApply(TableStore* ts, JournalOpType type, const String& payload);

private:
    void writeToFlash(const Array<JournalEntry>& entries);
    void replayEntry(const JournalEntry& entry);
};

} // namespace Xylem

#endif // XYLEM_JOURNAL_HPP
