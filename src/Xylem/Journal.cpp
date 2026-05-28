#include <Xylem/Journal.hpp>
#include <Xylem/TableStore.hpp>

namespace Xylem {

Journal::Journal(BlockDevice* dev, Allocator* alloc)
    : device(dev), allocator(alloc) {}

void Journal::initFromFormat(u16 startBlock, u16 count, u64 seq) {
    journalStartBlock    = startBlock;
    journalBlockCount    = count;
    currentSequence      = seq;
    currentJournalBlock  = startBlock;
}

// Journal block format:
//   [8B] sequence (u64 LE, monotonically increasing)
//   [4B] entryCount (u32 LE)
//   Per entry:
//     [1B] opType
//     [2B] targetBlock (u16 LE)
//     [4B] payloadLen (u32 LE)
//     [payloadLen B] payload
//   [4B] CRC32 of all preceding bytes
void Journal::writeToFlash(const Array<JournalEntry>& entries) {
    if (!device || !device->config.onDeviceWrite) return;
    if (entries.size() == 0) return;

    u32 blockSize = device->config.blockSize;

    String buf; buf.allocate(blockSize);
    buf.fill(0xFF);
    u8* blockStart = (u8*)buf.data();
    u8* ptr = blockStart;

    u64 seq = ++currentSequence;
    *(u64*)ptr = seq; ptr += 8;

    // We'll write the count after we know how many fit
    u8* countPtr = ptr;
    *(u32*)ptr = 0; ptr += 4;

    u32 written = 0;
    for (usz i = 0; i < entries.size(); ++i) {
        const JournalEntry& e = entries[i];
        u32 payloadLen = (u32)e.payload.size();
        u32 entryBytes = 1 + 2 + 4 + payloadLen;

        // Reserve 4 bytes at end for CRC
        if ((u32)(ptr - blockStart) + entryBytes + 4 > blockSize) break;

        *ptr++ = (u8)e.opType;
        *(u16*)ptr = e.targetBlock; ptr += 2;
        *(u32*)ptr = payloadLen;    ptr += 4;
        for (u32 j = 0; j < payloadLen; ++j) *ptr++ = e.payload[j];
        ++written;
    }

    *(u32*)countPtr = written;

    u32 payloadLen = (u32)(ptr - blockStart);
    u32 c = crc32(blockStart, payloadLen);
    *(u32*)ptr = c; ptr += 4;

    u32 blockDataLen = (u32)(ptr - blockStart);
    device->writeBlock(currentJournalBlock, 0, buf.slice(0, blockDataLen));

    // Advance ring pointer
    ++currentJournalBlock;
    if (currentJournalBlock >= journalStartBlock + journalBlockCount) {
        currentJournalBlock = journalStartBlock;
    }
}

void Journal::recover(TableStore* ts) {
    if (!device || !device->config.onDeviceRead) return;

    u32 blockSize = device->config.blockSize;

    // Scan every journal block; collect LOCK_BEGIN / LOCK_COMMIT / LOCK_ROLLBACK records.
    Map<u64, bool> committed;
    Map<u64, bool> rolledBack;

    for (u16 b = 0; b < journalBlockCount; ++b) {
        u16 blockIdx = journalStartBlock + b;
        String data = device->readBlock(blockIdx, 0);
        if ((u32)data.size() < 16) continue;

        const u8* blockStart = (const u8*)data.data();
        const u8* ptr = blockStart;

        u64 seq = *(u64*)ptr; ptr += 8;
        if (seq == 0 || seq == 0xFFFFFFFFFFFFFFFFULL) continue; // Uninitialized block

        u32 entryCount = *(u32*)ptr; ptr += 4;
        if (entryCount > 10000) continue; // Sanity check

        const u8* endPtr = blockStart + (u32)data.size() - 4; // 4 bytes for CRC at end

        for (u32 i = 0; i < entryCount && ptr < endPtr; ++i) {
            if (ptr + 7 > endPtr) break;

            JournalOpType opType = (JournalOpType)*ptr++;
            u16 targetBlock = *(u16*)ptr; ptr += 2;
            (void)targetBlock;
            u32 pLen = *(u32*)ptr; ptr += 4;
            if (ptr + pLen > endPtr) break;

            if (opType == JournalOpType::LOCK_BEGIN && pLen >= 8) {
                u64 lockId = *(u64*)ptr;
                committed.set(lockId, false);
            } else if (opType == JournalOpType::LOCK_COMMIT && pLen >= 8) {
                u64 lockId = *(u64*)ptr;
                committed.set(lockId, true);
            } else if (opType == JournalOpType::LOCK_ROLLBACK && pLen >= 8) {
                u64 lockId = *(u64*)ptr;
                rolledBack.set(lockId, true);
            } else if (opType == JournalOpType::TABLE_WRITE || 
                       opType == JournalOpType::TABLE_REMOVE || 
                       opType == JournalOpType::ROW_WRITE) {
                if (ts) deserializeAndApply(ts, opType, String((const u8*)ptr, pLen));
            }
            ptr += pLen;
        }
    }
    // Incomplete transactions (started but never committed/rolled back) are simply
    // discarded — their in-memory LockState was never committed to TableStore,
    // so no data inconsistency exists. The block allocations they made will be
    // reclaimed once a full GC pass runs.
}

void Journal::append(JournalOpType op, u16 targetBlock, const String& payload) {
    JournalEntry e;
    e.opType      = op;
    e.targetBlock = targetBlock;
    e.payload     = payload;
    Array<JournalEntry> arr;
    arr.push(e);
    writeToFlash(arr);
}

void Journal::appendBatch(const Array<JournalEntry>& entries) {
    writeToFlash(entries);
}

u64 Journal::lockBegin(bool requiresExplicitAs) {
    u64 id = nextLockId++;
    LockState ls;
    ls.snapshotSeq = currentSequence; // MVCC: capture snapshot at lock time
    ls.requiresExplicitAs = requiresExplicitAs;
    activeLocks.set(id, ls);

    String payload; payload.allocate(8);
    *(u64*)payload.data() = id;
    append(JournalOpType::LOCK_BEGIN, 0, payload);
    return id;
}

bool Journal::lockPendingWrite(u64 lockId, const PendingWrite& pw) {
    auto* ls = activeLocks.get(lockId);
    if (!ls) return false;
    ls->pendingWrites.push(pw);
    return true;
}

bool Journal::lockAppend(u64 lockId, JournalOpType op, u16 targetBlock, const String& payload) {
    auto* ls = activeLocks.get(lockId);
    if (!ls) return false;
    JournalEntry e;
    e.opType      = op;
    e.targetBlock = targetBlock;
    e.payload     = payload;
    ls->pendingEntries.push(e);
    return true;
}

bool Journal::lockCommit(u64 lockId) {
    auto* ls = activeLocks.get(lockId);
    if (!ls) return false;

    // Write all pending journal entries + LOCK_COMMIT atomically
    Array<JournalEntry> batch = ls->pendingEntries;

    JournalEntry commitEntry;
    commitEntry.opType      = JournalOpType::LOCK_COMMIT;
    commitEntry.targetBlock = 0;
    commitEntry.payload.allocate(8);
    *(u64*)commitEntry.payload.data() = lockId;
    batch.push(commitEntry);

    writeToFlash(batch);
    activeLocks.remove(lockId);
    return true;
}

bool Journal::lockRollback(u64 lockId) {
    if (!activeLocks.has(lockId)) return false;

    String payload; payload.allocate(8);
    *(u64*)payload.data() = lockId;
    append(JournalOpType::LOCK_ROLLBACK, 0, payload);

    activeLocks.remove(lockId);
    return true;
}

void Journal::replayEntry(const JournalEntry& /*entry*/) {
    // Full entry replay is done by BlobStore::scanFromDevice() on mount —
    // it rebuilds the blob index from all physically present BLOB blocks,
    // so committed writes are automatically recovered without explicit replay.
}

void Journal::trackRow(u64 lockId, u64 rowId) {
    auto* ls = activeLocks.get(lockId);
    if (!ls) return;
    // Avoid duplicates
    for (usz i = 0; i < ls->touchedRowIds.size(); ++i) {
        if (ls->touchedRowIds[i] == rowId) return;
    }
    ls->touchedRowIds.push(rowId);
}

u64 Journal::oldestActiveSnapshot() const {
    u64 oldest = currentSequence;
    for (auto it = activeLocks.begin(); it != activeLocks.end(); ++it) {
        if (it->value.snapshotSeq < oldest) oldest = it->value.snapshotSeq;
    }
    return oldest;
}

static void writeU64(String& buf, u64 v) {
    usz old = buf.size();
    buf.allocate(old + 8);
    *(u64*)(buf.data() + old) = v;
}

static u64 readU64(const u8*& ptr) {
    u64 v = *(u64*)ptr; ptr += 8;
    return v;
}

static void writeStr(String& buf, const String& s) {
    writeU64(buf, s.size());
    if (s.size() > 0) {
        usz old = buf.size();
        buf.allocate(old + s.size());
        for (usz i = 0; i < s.size(); ++i) buf[old + i] = s[i];
    }
}

static String readStr(const u8*& ptr) {
    u64 len = readU64(ptr);
    if (len == 0) return String();
    String s; s.allocate(len);
    for (u64 i = 0; i < len; ++i) s[i] = *ptr++;
    return s;
}

static void writeClauses(String& buf, const Clauses& c) {
    usz old = buf.size();
    buf.allocate(old + 1);
    buf[old] = c.isAssert ? 1 : 0;
    writeU64(buf, c.size());
    for (usz i = 0; i < c.size(); ++i) {
        writeStr(buf, c[i].col);
        writeStr(buf, c[i].op);
        writeStr(buf, c[i].val);
    }
}

static Clauses readClauses(const u8*& ptr) {
    Clauses c;
    c.isAssert = *ptr++ != 0;
    u64 len = readU64(ptr);
    for (u64 i = 0; i < len; ++i) {
        Clause cl;
        cl.col = readStr(ptr);
        cl.op = readStr(ptr);
        cl.val = readStr(ptr);
        c.push(cl);
    }
    return c;
}

String Journal::serializeTableWrite(const Array<Clause>& columns, const Array<Clauses>& clauses, const String& encryptionKey) {
    String buf;
    writeU64(buf, columns.size());
    for (usz i = 0; i < columns.size(); ++i) {
        writeStr(buf, columns[i].col);
        writeStr(buf, columns[i].val);
    }
    writeU64(buf, clauses.size());
    for (usz i = 0; i < clauses.size(); ++i) writeClauses(buf, clauses[i]);
    writeStr(buf, encryptionKey);
    return buf;
}

String Journal::serializeTableRemove(const Array<Clauses>& clauses, u64 length) {
    String buf;
    writeU64(buf, length);
    writeU64(buf, clauses.size());
    for (usz i = 0; i < clauses.size(); ++i) writeClauses(buf, clauses[i]);
    return buf;
}

static void writeGraphOp(String& buf, const GraphOp& op) {
    usz old = buf.size();
    buf.allocate(old + 1);
    buf[old] = (u8)op.type;
    writeU64(buf, op.query.size());
    for (usz i = 0; i < op.query.size(); ++i) writeClauses(buf, op.query[i]);
    writeU64(buf, op.untilQuery.size());
    for (usz i = 0; i < op.untilQuery.size(); ++i) writeClauses(buf, op.untilQuery[i]);
    writeU64(buf, op.writeSet.size());
    for (usz i = 0; i < op.writeSet.size(); ++i) {
        writeStr(buf, op.writeSet[i].col);
        writeStr(buf, op.writeSet[i].op);
        writeStr(buf, op.writeSet[i].val);
    }
}

static GraphOp readGraphOp(const u8*& ptr) {
    GraphOp op;
    op.type = (GraphOpType)*ptr++;
    u64 qLen = readU64(ptr);
    for (u64 i = 0; i < qLen; ++i) op.query.push(readClauses(ptr));
    u64 uqLen = readU64(ptr);
    for (u64 i = 0; i < uqLen; ++i) op.untilQuery.push(readClauses(ptr));
    u64 wLen = readU64(ptr);
    for (u64 i = 0; i < wLen; ++i) {
        Clause cl;
        cl.col = readStr(ptr);
        cl.op = readStr(ptr);
        cl.val = readStr(ptr);
        op.writeSet.push(cl);
    }
    return op;
}

String Journal::serializeGraphWrite(const Array<GraphOp>& ops, const String& encryptionKey) {
    String buf;
    writeU64(buf, ops.size());
    for (usz i = 0; i < ops.size(); ++i) writeGraphOp(buf, ops[i]);
    writeStr(buf, encryptionKey);
    return buf;
}

void Journal::deserializeAndApply(TableStore* ts, JournalOpType type, const String& payload) {
    if (!ts || payload.isEmpty()) return;
    const u8* ptr = (const u8*)payload.data();
    
    if (type == JournalOpType::TABLE_WRITE) {
        u64 colLen = readU64(ptr);
        Array<Clause> columns;
        for (u64 i = 0; i < colLen; ++i) {
            Clause c; c.col = readStr(ptr); c.op = "="; c.val = readStr(ptr);
            columns.push(c);
        }
        u64 clLen = readU64(ptr);
        Array<Clauses> clauses;
        for (u64 i = 0; i < clLen; ++i) clauses.push(readClauses(ptr));
        String key = readStr(ptr);
        ts->write(columns, clauses, key);
    } 
    else if (type == JournalOpType::TABLE_REMOVE) {
        u64 length = readU64(ptr);
        u64 clLen = readU64(ptr);
        Array<Clauses> clauses;
        for (u64 i = 0; i < clLen; ++i) clauses.push(readClauses(ptr));
        ts->remove(clauses, length);
    }
    else if (type == JournalOpType::ROW_WRITE) { 
        u64 opsLen = readU64(ptr);
        Array<GraphOp> ops;
        for (u64 i = 0; i < opsLen; ++i) ops.push(readGraphOp(ptr));
        String key = readStr(ptr);
        ts->graphWrite(ops, key);
    }
}

} // namespace Xylem
