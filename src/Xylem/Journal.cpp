#include <Xylem/Journal.hpp>

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

void Journal::recover() {
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

u64 Journal::lockBegin() {
    u64 id = nextLockId++;
    LockState ls;
    ls.snapshotSeq = currentSequence; // MVCC: capture snapshot at lock time
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

} // namespace Xylem
