#include <Xylem/Xylem.hpp>
#include <Xylem/XBDiff.hpp>
#include <Xi/Random.hpp>
#include <Sec/Crypto.hpp>
#include <stdint.h>
#include <stdio.h>

namespace Xylem {

XylemEngine::XylemEngine() {
    for (u32 i = 0; i < 1024; ++i) pinnedRawBits[i] = 0;
}

XylemEngine::~XylemEngine() {
    destroy();
}

u32 getTriangularBlock(u32 i, u32 totalBlocks) {
    return (i * (i + 1) / 2) % totalBlocks;
}

static constexpr u32 BAM_DEFAULT_COUNT   = 16;
static constexpr u32 JNL_DEFAULT_COUNT   = 4;

static const char* SB_MAGIC = "XYLM_SUPERBLOCK_V1";
static constexpr u32 SB_MAGIC_LEN = 18;
// Magic(18) + Seq(8) + bamStart(4) + bamCount(4) + jnlStart(4) + jnlCount(4) + pinnedRawBits(1024) = 1066
static constexpr u32 SB_TOTAL_LEN = 1066; 

// Helper to write/read Superblock payload
static void writeSuperblock(String& sbuf, u64 seq, u32 bStart, u32 bCount, u32 jStart, u32 jCount, const u8* pinnedBits) {
    sbuf.allocate(SB_TOTAL_LEN);
    sbuf.fill(0);
    u8* ptr = (u8*)sbuf.data();
    for (u32 i = 0; i < SB_MAGIC_LEN; ++i) *ptr++ = SB_MAGIC[i];
    *(u64*)ptr = seq;    ptr += 8;
    *(u32*)ptr = bStart; ptr += 4;
    *(u32*)ptr = bCount; ptr += 4;
    *(u32*)ptr = jStart; ptr += 4;
    *(u32*)ptr = jCount; ptr += 4;
    for (u32 i = 0; i < 1024; ++i) *ptr++ = pinnedBits[i];
}

static bool readSuperblock(const String& sbuf, u64& seq, u32& bStart, u32& bCount, u32& jStart, u32& jCount, u8* pinnedBits) {
    if ((u32)sbuf.size() < SB_TOTAL_LEN) return false;
    const u8* ptr = (const u8*)sbuf.data();
    for (u32 i = 0; i < SB_MAGIC_LEN; ++i) {
        if (*ptr++ != SB_MAGIC[i]) return false;
    }
    seq    = *(const u64*)ptr; ptr += 8;
    bStart = *(const u32*)ptr; ptr += 4;
    bCount = *(const u32*)ptr; ptr += 4;
    jStart = *(const u32*)ptr; ptr += 4;
    jCount = *(const u32*)ptr; ptr += 4;
    if (pinnedBits) {
        for (u32 i = 0; i < 1024; ++i) pinnedBits[i] = *ptr++;
    }
    return true;
}

// ─── format() ────────────────────────────────────────────────────────────────

bool XylemEngine::format() {
    if (!device) {
        device = new BlockDevice();
        device->config = config;
    }

    u32 totalBlocks = config.deviceSize > 0 ? (config.deviceSize / config.blockSize) : 1024;

    // 1. Try to find existing Superblock to preserve RAW_FIXED blocks
    u32 oldBamStart = 0, oldBamCount = 0;
    bool foundOld = false;
    for (u32 i = 0; i < 32; ++i) {
        u32 bIdx = getTriangularBlock(i, totalBlocks);
        String data = device->readBlock(bIdx, 0);
        u64 seq; u32 bS, bC, jS, jC;
        if (readSuperblock(data, seq, bS, bC, jS, jC, nullptr)) {
            oldBamStart = bS; oldBamCount = bC;
            foundOld = true;
            break;
        }
    }

    Array<u32> rawFixedBlocks;
    if (foundOld) {
        Allocator tempAlloc(device);
        tempAlloc.bamStartBlock = oldBamStart;
        tempAlloc.bamBlockCount = oldBamCount;
        tempAlloc.initFromFormat(totalBlocks);
        if (tempAlloc.loadBam()) {
            for (u32 i = 0; i < totalBlocks; ++i) {
                if (tempAlloc.bam[i].getStatus() == BlockStatus::RESERVED &&
                    tempAlloc.bam[i].getType() == BlockType::RAW) {
                    rawFixedBlocks.push(i);
                }
            }
        }
    }

    // 1b. Check if there's an existing Superblock so we don't erase pinned RAW blocks
    for (u32 i = 0; i < 32; ++i) {
        u32 bIdx = getTriangularBlock(i, totalBlocks);
        String data = device->readBlock(bIdx, 0);
        u64 seq; u32 bS, bC, jS, jC;
        if (readSuperblock(data, seq, bS, bC, jS, jC, this->pinnedRawBits)) {
            break; 
        }
    }

    // Merge explicitly provided rawFixedBlocks into pinnedRawBits
    for (usz j = 0; j < rawFixedBlocks.size(); ++j) {
        u32 b = rawFixedBlocks[j];
        if (b < 8192) pinnedRawBits[b / 8] |= (1 << (b % 8));
    }

    auto isPinnedBlock = [&](u32 b) {
        if (b >= 8192) return false;
        return (pinnedRawBits[b / 8] & (1 << (b % 8))) != 0;
    };

    // 2. Erase all non-RAW blocks
    for (u32 i = 0; i < totalBlocks; ++i) {
        if (!isPinnedBlock(i)) {
            if (device->config.onDeviceErase) {
                u64 off = (u64)i * device->config.blockSize;
                device->config.onDeviceErase(off, off + device->config.blockSize);
            } else if (device->config.onDeviceWrite) {
                String empty; empty.allocate(device->config.blockSize);
                empty.fill(0xFF);
                device->config.onDeviceWrite((u64)i * device->config.blockSize, empty);
            }
        }
    }

    // 3. Find safe system block locations
    auto isSafe = [&](u32 b) {
        return !isPinnedBlock(b);
    };

    u32 superBlockIdx = 0xFFFFFFFFu;
    for (u32 i = 0; i < 32; ++i) {
        u32 bIdx = getTriangularBlock(i, totalBlocks);
        if (isSafe(bIdx) && isSafe((bIdx + 1) % totalBlocks)) {
            superBlockIdx = bIdx;
            break;
        }
    }

    u32 sysStart = (superBlockIdx + 2) % totalBlocks;
    while (true) {
        bool ok = true;
        for (u32 i = 0; i < BAM_DEFAULT_COUNT + JNL_DEFAULT_COUNT; ++i) {
            if (!isSafe((sysStart + i) % totalBlocks)) { ok = false; break; }
        }
        if (ok) break;
        sysStart = (sysStart + 1) % totalBlocks;
    }

    u32 newBamStart = sysStart;
    u32 newJnlStart = (sysStart + BAM_DEFAULT_COUNT) % totalBlocks;

    allocator = new Allocator(device);
    allocator->bamStartBlock = newBamStart;
    allocator->bamBlockCount = BAM_DEFAULT_COUNT;
    allocator->initFromFormat(totalBlocks);

    allocator->bam[superBlockIdx].setStatus(BlockStatus::RESERVED);
    allocator->bam[superBlockIdx].setType(BlockType::SUPER);
    allocator->bam[(superBlockIdx + 1) % totalBlocks].setStatus(BlockStatus::RESERVED);
    allocator->bam[(superBlockIdx + 1) % totalBlocks].setType(BlockType::SUPER);

    for (u32 i = 0; i < BAM_DEFAULT_COUNT; ++i) {
        allocator->bam[(newBamStart + i) % totalBlocks].setStatus(BlockStatus::RESERVED);
        allocator->bam[(newBamStart + i) % totalBlocks].setType(BlockType::BAM);
    }
    for (u32 i = 0; i < JNL_DEFAULT_COUNT; ++i) {
        allocator->bam[(newJnlStart + i) % totalBlocks].setStatus(BlockStatus::RESERVED);
        allocator->bam[(newJnlStart + i) % totalBlocks].setType(BlockType::JOURNAL);
    }
    for (usz i = 0; i < rawFixedBlocks.size(); ++i) {
        allocator->bam[rawFixedBlocks[i]].setStatus(BlockStatus::RESERVED);
        allocator->bam[rawFixedBlocks[i]].setType(BlockType::RAW);
    }

    allocator->buildHeap();
    allocator->saveBam();

    String sbuf;
    writeSuperblock(sbuf, 1, newBamStart, BAM_DEFAULT_COUNT, newJnlStart, JNL_DEFAULT_COUNT, this->pinnedRawBits);
    device->writeBlock(superBlockIdx, 0, sbuf);
    device->writeBlock((superBlockIdx + 1) % totalBlocks, 0, sbuf);

    this->currentSuperblockIdx = superBlockIdx;

    delete allocator; allocator = nullptr;
    delete device;    device    = nullptr;
    return true;
}

// ─── mount() ─────────────────────────────────────────────────────────────────

bool XylemEngine::mount() {
    if (!device) {
        device = new BlockDevice();
        device->config = config;
    }

    u32 totalBlocks = config.deviceSize > 0 ? (config.deviceSize / config.blockSize) : 1024;
    u32 superblockIdx = 0xFFFFFFFFu;
    u64 maxSeq = 0;
    u32 bamStart = BAM_DEFAULT_COUNT, bamCount = BAM_DEFAULT_COUNT;
    u32 jnlStart = BAM_DEFAULT_COUNT * 2, jnlCount = JNL_DEFAULT_COUNT;

    for (u32 i = 0; i < 32; ++i) {
        u32 bIdx = getTriangularBlock(i, totalBlocks);
        String data = device->readBlock(bIdx, 0);
        u64 seq; u32 bS, bC, jS, jC;
        if (readSuperblock(data, seq, bS, bC, jS, jC, this->pinnedRawBits)) {
            if (superblockIdx == 0xFFFFFFFFu || seq > maxSeq) {
                maxSeq = seq;
                superblockIdx = bIdx;
                bamStart = bS; bamCount = bC;
                jnlStart = jS; jnlCount = jC;
            }
        }
    }

    if (superblockIdx == 0xFFFFFFFFu) {
        return false;
    }

    this->currentSuperblockIdx = superblockIdx;
    allocator = new Allocator(device);
    allocator->bamStartBlock = bamStart;
    allocator->bamBlockCount = bamCount;
    allocator->initFromFormat(totalBlocks);
    bool ok = allocator->loadBam();
    allocator->buildHeap();

    journal = new Journal(device, allocator);
    journal->initFromFormat((u16)jnlStart, (u16)jnlCount, maxSeq);

    tableStore = new TableStore(device, allocator, &globalKeys);
    tableStore->maxMemoryBytes = maxCache > 0 ? maxCache / 2 : 4 * 1024 * 1024;

    blobStore = new BlobStore(device, allocator, &globalKeys);
    blobStore->scanFromDevice();
    blobStore->loadRefsAndUsage();
    blobStore->loadPendingFreezes();
    // Wire up BlobStore pointer in TableStore
    tableStore->blobStore = blobStore;
    blobStore->thawCallback = [this](u64 start, u64 end) {
        this->thaw(start, end);
    };

    auto writeVLU = [](u8*& ptr, u64 val) {
        while (val >= 0x80) { *ptr++ = (val & 0x7F) | 0x80; val >>= 7; }
        *ptr++ = val & 0x7F;
    };
    auto readVLU = [](const u8*& ptr, const u8* end) -> u64 {
        u64 val = 0; int shift = 0;
        while (ptr < end) {
            u8 b = *ptr++;
            val |= (u64)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift > 63) break; // Prevent infinite loop on corrupt data
        }
        return val;
    };

    tableStore->saveToDisk = [this, writeVLU](u64 id, Map<String, String>* row) {
        if (!this->blobStore) return;
        String key = "ROW_" + String::from(id);
        this->blobStore->removeHash(key); 
        
        usz needed = 10; // VLU overhead for count
        for (auto it = row->begin(); it != row->end(); ++it) {
            needed += 10 + it->key.size() + 10 + it->value.size(); // VLU + data
        }
        String data; data.allocate(needed);
        u8* ptr = data.data();
        writeVLU(ptr, row->size());
        for (auto it = row->begin(); it != row->end(); ++it) {
            writeVLU(ptr, it->key.size());
            for (usz i = 0; i < it->key.size(); ++i) *ptr++ = it->key[i];
            writeVLU(ptr, it->value.size());
            for (usz i = 0; i < it->value.size(); ++i) *ptr++ = it->value[i];
        }
        this->blobStore->writeHash(key, 0, data.slice(0, ptr - data.data()), "");
    };

    tableStore->fetchFromDisk = [this, readVLU](u64 id) -> Map<String, String>* {
        if (!this->blobStore) return nullptr;
        String data = this->blobStore->readHash("ROW_" + String::from(id), 0, 0xFFFFFFFF);
        if (data.isEmpty()) return nullptr;

        Map<String, String>* row = new Map<String, String>();
        const u8* ptr = data.data();
        const u8* end = data.data() + data.size();
        u64 count = readVLU(ptr, end);
        for (u64 i = 0; i < count && ptr < end; ++i) {
            u64 kLen = readVLU(ptr, end);
            if (end - ptr < (ptrdiff_t)kLen) break;
            String key; key.allocate(kLen);
            for (u64 j = 0; j < kLen; ++j) key[j] = *ptr++;
            u64 vLen = readVLU(ptr, end);
            if (end - ptr < (ptrdiff_t)vLen) break;
            String val; val.allocate(vLen);
            for (u64 j = 0; j < vLen; ++j) val[j] = *ptr++;
            row->set(key, val);
        }
        return row;
    };

    tableStore->saveHNSWToDisk = [this, writeVLU](u64 id, HNSW::Node* n) {
        if (!this->blobStore) return;
        String key = "HNSW_" + String::from(id);
        this->blobStore->removeHash(key);
        
        usz needed = 30; // VLU overheads
        needed += n->vec.size() * sizeof(f32);
        for (usz l = 0; l < n->neighbors.size(); ++l) {
            needed += 10 + n->neighbors[l].size() * 10;
        }
        String data; data.allocate(needed);
        u8* ptr = data.data();
        writeVLU(ptr, n->id);
        writeVLU(ptr, n->vec.size());
        for (usz i = 0; i < n->vec.size(); ++i) { *(f32*)ptr = n->vec[i]; ptr += 4; }
        writeVLU(ptr, n->neighbors.size());
        for (usz l = 0; l < n->neighbors.size(); ++l) {
            writeVLU(ptr, n->neighbors[l].size());
            for (usz k = 0; k < n->neighbors[l].size(); ++k) writeVLU(ptr, n->neighbors[l][k]);
        }
        this->blobStore->writeHash(key, 0, data.slice(0, ptr - data.data()), "");
    };

    tableStore->fetchHNSWFromDisk = [this, readVLU](u64 id) -> HNSW::Node* {
        if (!this->blobStore) return nullptr;
        String data = this->blobStore->readHash("HNSW_" + String::from(id), 0, 0xFFFFFFFF);
        if (data.isEmpty()) return nullptr;
        HNSW::Node* n = new HNSW::Node();
        const u8* ptr = data.data();
        const u8* end = data.data() + data.size();
        n->id = readVLU(ptr, end);
        u64 vecSize = readVLU(ptr, end);
        if (end - ptr < (ptrdiff_t)(vecSize * 4)) { delete n; return nullptr; }
        n->vec.allocate(vecSize);
        for (u64 i = 0; i < vecSize; ++i) { n->vec[i] = *(f32*)ptr; ptr += 4; }
        u64 numLevels = readVLU(ptr, end);
        n->neighbors.allocate(numLevels);
        for (u64 l = 0; l < numLevels && ptr < end; ++l) {
            u64 nCount = readVLU(ptr, end);
            n->neighbors[l].allocate(nCount);
            for (u64 k = 0; k < nCount && ptr < end; ++k) n->neighbors[l][k] = readVLU(ptr, end);
        }
        return n;
    };

    tableStore->removeHNSWFromDisk = [this](u64 id) {
        if (this->blobStore) this->blobStore->removeHash("HNSW_" + String::from(id));
    };

    tableStore->writeBlob = [this](const String& h, const String& d) {
        if (!this->blobStore) return;
        this->blobStore->removeHash(h); 
        this->blobStore->writeHash(h, 0, d, "");
    };

    tableStore->readBlob = [this](const String& h) -> String {
        return this->blobStore ? this->blobStore->readHash(h, 0, 0xFFFFFFFF) : String();
    };

    tableStore->loadSchemas();
    journal->recover(tableStore);

    // Purge Volatile SWAP blocks on boot
    Array<String> volatileBlobsToPurge;
    for (auto it = blobStore->index.begin(); it != blobStore->index.end(); ++it) {
        if (it->key.startsWith("VOLATILE_BLOCK_")) {
            volatileBlobsToPurge.push(it->key);
        }
    }
    for (usz i = 0; i < volatileBlobsToPurge.size(); ++i) {
        blobStore->removeHash(volatileBlobsToPurge[i]);
    }

    cache   = new Cache(device, maxCache);
    watcher = new Watcher(tableStore);

    return true;
}

// ─── destroy() / flush() ─────────────────────────────────────────────────────────

void XylemEngine::destroy() {
    // Save everything corruption-free
    flush();

    // Remove volatile items from memory
    if (tableStore) {
        Array<u64> toRemove;
        for (auto it = tableStore->volatileRows.begin(); it != tableStore->volatileRows.end(); ++it) {
            toRemove.push(it->key);
        }
        for (usz i = 0; i < toRemove.size(); ++i) {
            tableStore->allRows.remove(toRemove[i]);
            // Remove from allRowIds
            Array<u64> newIds;
            for (usz j = 0; j < tableStore->allRowIds.size(); ++j) {
                if (tableStore->allRowIds[j] != toRemove[i]) newIds.push(tableStore->allRowIds[j]);
            }
            tableStore->allRowIds = newIds;
        }
        tableStore->volatileRows.clear();
    }

    // Free all resources
    delete watcher;
    delete cache;
    delete blobStore;
    delete tableStore;
    delete journal;
    delete allocator;
    delete device;

    watcher    = nullptr;
    cache      = nullptr;
    blobStore  = nullptr;
    tableStore = nullptr;
    journal    = nullptr;
    allocator  = nullptr;
    device     = nullptr;
}

bool XylemEngine::isMounted() const {
    return device != nullptr && allocator != nullptr && tableStore != nullptr;
}

void XylemEngine::ensureMounted() {
    if (!isMounted()) mount();
}

void XylemEngine::flush() {
    if (tableStore) {
        tableStore->flushAllRows();   
        tableStore->flushHnsw();      
        if (blobStore) {
            blobStore->trainDictionary();
            blobStore->saveRefsAndUsage();
            blobStore->savePendingFreezes();
        }
        tableStore->saveSchemas();    
    }
    if (allocator) {
        allocator->saveBam();         
    }
    if (cache) cache->flushAll();

    // Update superblock to match current state
    if (device && allocator && journal && currentSuperblockIdx != 0xFFFFFFFFu) {
        u32 totalBlocks = allocator->bam.size();
        String sbuf;
        journal->currentSequence++;
        writeSuperblock(sbuf, journal->currentSequence,
                        allocator->bamStartBlock, allocator->bamBlockCount,
                        journal->journalStartBlock, journal->journalBlockCount,
                        this->pinnedRawBits);
        device->writeBlock(currentSuperblockIdx, 0, sbuf);
        device->writeBlock((currentSuperblockIdx + 1) % totalBlocks, 0, sbuf);
    }

    // Semi-aggressive GC: clean up old versions on flush
    if (journal && tableStore) {
        u64 oldest = journal->oldestActiveSnapshot();
        tableStore->gcVersions(oldest);
    }

    // Active wear leveling: relocate cold data from high-erase blocks to low-erase blocks
    if (allocator && blobStore && device) {
        u32 maxEraseIdx = 0xFFFFFFFF;
        u16 maxErase = 0;
        u32 totalBlocks = allocator->bam.size();
        for (u32 i = 0; i < totalBlocks; ++i) {
            if (allocator->bam[i].getStatus() == BlockStatus::USED &&
                allocator->bam[i].getType() == BlockType::BLOB) {
                if (allocator->bam[i].eraseCount > maxErase) {
                    maxErase = allocator->bam[i].eraseCount;
                    maxEraseIdx = i;
                }
            }
        }
        
        if (maxEraseIdx != 0xFFFFFFFF && allocator->freeHeap.size() > 0) {
            u16 minFreeErase = allocator->freeHeap[0].eraseCount;
            // Relocate if the spread is significant (>20% of max)
            if (maxErase > minFreeErase + (maxErase / 5)) {
                String targetHash;
                for (auto it = blobStore->index.begin(); it != blobStore->index.end(); ++it) {
                    u32 cur = it->value.blockIdx;
                    bool owns = false;
                    while (cur != 0) {
                        if (cur == maxEraseIdx) { owns = true; break; }
                        String blk = device->readBlock(cur, allocator->bam[cur].eraseCount);
                        if ((u32)blk.size() >= 19) {
                            const u8* ptr = (const u8*)blk.data();
                            ptr++; u8 isF = *ptr++; u8 kLen = *ptr++;
                            if (isF) ptr += kLen;
                            ptr += 8;
                            cur = *(const u32*)ptr;
                        } else { cur = 0; }
                    }
                    if (owns) {
                        targetHash = it->key;
                        break;
                    }
                }
                
                if (!targetHash.isEmpty()) {
                    String data = blobStore->readHash(targetHash, 0, 0xFFFFFFFF);
                    // Temporarily mark the hot block as RESERVED so writeHash doesn't pick it
                    allocator->bam[maxEraseIdx].setStatus(BlockStatus::RESERVED);
                    blobStore->removeHash(targetHash);
                    blobStore->writeHash(targetHash, 0, data, "");
                    allocator->freeBlock(maxEraseIdx);
                    allocator->saveBam();
                }
            }
        }
    }
}

// ─── fixRaw() & Relocation ───────────────────────────────────────────────────

bool XylemEngine::fixRaw(u64 byteAddress, const String& rawData) {
    if (!device || !allocator) return false;
    
    u32 blockSize = device->config.blockSize;
    u32 startBlock = (u32)(byteAddress / blockSize);
    u32 endBlock = (u32)((byteAddress + rawData.size() + blockSize - 1) / blockSize);
    u32 totalBlocks = allocator->bam.size();

    // Set pinnedBits for the RAW block so we don't erase it later!
    for (u32 b = startBlock; b < endBlock && b < totalBlocks; ++b) {
        if (b < 8192) this->pinnedRawBits[b / 8] |= (1 << (b % 8));
    }

    // 1. Check for conflicts
    bool hitSystem = false;
    Array<String> blobsToMove;
    for (u32 b = startBlock; b < endBlock && b < totalBlocks; ++b) {
        BlockType t = allocator->bam[b].getType();
        if (t == BlockType::SUPER || t == BlockType::BAM || t == BlockType::JOURNAL) {
            hitSystem = true;
        } else if (t == BlockType::BLOB) {
            if (blobStore) {
                for (auto it = blobStore->index.begin(); it != blobStore->index.end(); ++it) {
                    bool owns = false;
                    u32 cur = it->value.blockIdx;
                    while (cur != 0) {
                        if (cur == b) { owns = true; break; }
                        String blk = device->readBlock(cur, 0);
                        if ((u32)blk.size() >= 19) {
                            const u8* ptr = (const u8*)blk.data();
                            ptr++; u8 isF = *ptr++; u8 kLen = *ptr++;
                            if (isF) ptr += kLen;
                            ptr += 8;
                            cur = *(u32*)ptr;
                        } else { cur = 0; }
                    }
                    if (owns) {
                        bool alreadyAdded = false;
                        for (usz k = 0; k < blobsToMove.size(); ++k) if (blobsToMove[k] == it->key) alreadyAdded = true;
                        if (!alreadyAdded) blobsToMove.push(it->key);
                    }
                }
            }
        }
    }

    // 2. Relocate Blobs
    if (blobStore) {
        Array<String> blobData;
        for (usz i = 0; i < blobsToMove.size(); ++i) {
            String hash = blobsToMove[i];
            blobData.push(blobStore->readHash(hash, 0, 0xFFFFFFFF));
            blobStore->removeHash(hash);
        }
        
        // Prevent allocBlock from returning the target blocks
        for (u32 b = startBlock; b < endBlock && b < totalBlocks; ++b) {
            allocator->bam[b].setStatus(BlockStatus::RESERVED);
        }

        for (usz i = 0; i < blobsToMove.size(); ++i) {
            String hash = blobsToMove[i];
            blobStore->writeHash(hash, 0, blobData[i], "");
        }
    }

    // 3. Relocate System Blocks
    if (hitSystem) {
        auto isSafe = [&](u32 b) {
            if (b >= startBlock && b < endBlock) return false;
            if (allocator->bam[b].getType() == BlockType::RAW) return false;
            if (allocator->bam[b].getStatus() == BlockStatus::USED) return false;
            return true;
        };

        u32 newSuperBlockIdx = 0xFFFFFFFFu;
        for (u32 i = 0; i < 32; ++i) {
            u32 bIdx = getTriangularBlock(i, totalBlocks);
            if (isSafe(bIdx) && isSafe((bIdx + 1) % totalBlocks)) {
                newSuperBlockIdx = bIdx; break;
            }
        }
        if (newSuperBlockIdx == 0xFFFFFFFFu) return false;

        u32 sysStart = (newSuperBlockIdx + 2) % totalBlocks;
        u32 startSysTry = sysStart;
        bool foundSys = false;
        while (true) {
            bool ok = true;
            for (u32 i = 0; i < allocator->bamBlockCount + journal->journalBlockCount; ++i) {
                u32 b = (sysStart + i) % totalBlocks;
                if (b == newSuperBlockIdx || b == (newSuperBlockIdx + 1) % totalBlocks) { ok = false; break; }
                if (!isSafe(b)) { ok = false; break; }
            }
            if (ok) { foundSys = true; break; }
            sysStart = (sysStart + 1) % totalBlocks;
            if (sysStart == startSysTry) break;
        }
        if (!foundSys) return false;

        u32 newBamStart = sysStart;
        u32 newJnlStart = (sysStart + allocator->bamBlockCount) % totalBlocks;

        for (u32 i = 0; i < totalBlocks; ++i) {
            BlockType t = allocator->bam[i].getType();
            if (t == BlockType::SUPER || 
                t == BlockType::BAM || 
                t == BlockType::JOURNAL) {
                allocator->bam[i].setStatus(BlockStatus::FREE);
                allocator->bam[i].setType(BlockType::FREE);
                
                bool isNewSystem = false;
                if (i == newSuperBlockIdx || i == (newSuperBlockIdx + 1) % totalBlocks) isNewSystem = true;
                for (u32 j = 0; j < allocator->bamBlockCount; ++j) {
                    if (i == (newBamStart + j) % totalBlocks) isNewSystem = true;
                }
                for (u32 j = 0; j < journal->journalBlockCount; ++j) {
                    if (i == (newJnlStart + j) % totalBlocks) isNewSystem = true;
                }
                if (!isNewSystem) {
                    device->eraseBlock(i);
                    String emptyBlock; emptyBlock.allocate(device->config.blockSize);
                    emptyBlock.fill(0xFF);
                    device->writeBlock(i, 0, emptyBlock);
                }
            }
        }

        allocator->bam[newSuperBlockIdx].setStatus(BlockStatus::RESERVED);
        allocator->bam[newSuperBlockIdx].setType(BlockType::SUPER);
        allocator->bam[(newSuperBlockIdx + 1) % totalBlocks].setStatus(BlockStatus::RESERVED);
        allocator->bam[(newSuperBlockIdx + 1) % totalBlocks].setType(BlockType::SUPER);

        for (u32 i = 0; i < allocator->bamBlockCount; ++i) {
            allocator->bam[(newBamStart + i) % totalBlocks].setStatus(BlockStatus::RESERVED);
            allocator->bam[(newBamStart + i) % totalBlocks].setType(BlockType::BAM);
        }
        for (u32 i = 0; i < journal->journalBlockCount; ++i) {
            allocator->bam[(newJnlStart + i) % totalBlocks].setStatus(BlockStatus::RESERVED);
            allocator->bam[(newJnlStart + i) % totalBlocks].setType(BlockType::JOURNAL);
        }

        allocator->bamStartBlock = newBamStart;
        journal->journalStartBlock = newJnlStart;
        journal->currentJournalBlock = newJnlStart;

        allocator->buildHeap();
        if (isSafe(newBamStart) && isSafe(newJnlStart)) {
            String sbuf;
            journal->currentSequence++;
            writeSuperblock(sbuf, journal->currentSequence, newBamStart, allocator->bamBlockCount, newJnlStart, journal->journalBlockCount, this->pinnedRawBits);
            device->writeBlock(newSuperBlockIdx, 0, sbuf);
            device->writeBlock((newSuperBlockIdx + 1) % totalBlocks, 0, sbuf);
            this->currentSuperblockIdx = newSuperBlockIdx;
        }
        allocator->saveBam();
    } else {
        if (this->currentSuperblockIdx != 0xFFFFFFFFu) {
            String sbuf;
            journal->currentSequence++;
            writeSuperblock(sbuf, journal->currentSequence, allocator->bamStartBlock, allocator->bamBlockCount, journal->journalStartBlock, journal->journalBlockCount, this->pinnedRawBits);
            device->writeBlock(this->currentSuperblockIdx, 0, sbuf);
            device->writeBlock((this->currentSuperblockIdx + 1) % totalBlocks, 0, sbuf);
        }
    }

    // 4. Finally write the RAW FIXED blocks
    u32 offset = 0;
    for (u32 b = startBlock; b < endBlock && b < totalBlocks; ++b) {
        allocator->bam[b].setStatus(BlockStatus::RESERVED);
        allocator->bam[b].setType(BlockType::RAW);
        
        u32 writeLen = blockSize;
        if (offset + writeLen > rawData.size()) writeLen = rawData.size() - offset;
        
        String chunk = rawData.slice(offset, offset + writeLen);
        if (chunk.size() < blockSize) {
            String pad; pad.allocate(blockSize); pad.fill(0xFF);
            for (usz i = 0; i < chunk.size(); ++i) pad[i] = chunk[i];
            chunk = pad;
        }
        
        device->writeBlock(b, allocator->bam[b].eraseCount, chunk);
        offset += writeLen;
    }

    allocator->buildHeap();
    allocator->saveBam();

    return true;
}

// ─── Query Parser ────────────────────────────────────────────────────────────

String XylemEngine::generateId(const String& column) {
    if (!Xi::_randomInitialized) {
        Xi::randomSeed(true);
    }
    
    while (true) {
        u64 part1 = Xi::randomNext();
        u64 part2 = Xi::randomNext();
        u64 rId = (part1 << 32) | part2;
        String idStr = String::from(rId);
        
        Array<String> cols; cols.push("id");
        Array<Clauses> query; query.push(WHERE(column, "=", idStr));
        auto result = read(cols, query, 1);
        if (result.size() == 0) {
            return idStr;
        }
    }
}

QueryResult XylemEngine::query(const String& queryString, const Array<String>& sanitized) {
    ensureMounted();
    return QueryParser::execute(this, queryString, sanitized);
}

// ─── CRUD operations ─────────────────────────────────────────────────────────

Array<Map<String,String>> XylemEngine::read(const Array<String>& columns, const Array<Clauses>& clauses,
                                             u64 length, bool tombstones, u64 txId, bool readAllColumns) {
    ensureMounted();
    if (!tableStore) return {};
    u64 snapshotSeq = 0;
    if (txId != 0 && journal) {
        auto* ls = journal->activeLocks.get(txId);
        if (ls) snapshotSeq = ls->snapshotSeq;
    }
    return tableStore->read(columns, clauses, length, tombstones, snapshotSeq, txId, readAllColumns);
}

bool XylemEngine::isWriteBlocked(u64 txId) {
    if (!journal) return false;
    for (auto it = journal->activeLocks.begin(); it != journal->activeLocks.end(); ++it) {
        if (it->value.requiresExplicitAs && it->key != txId) {
            return true;
        }
    }
    return false;
}

int XylemEngine::write(const Array<Clause>& columns, const Array<Clauses>& clauses, u64 txId, const String& encryptionKey) {
    ensureMounted();
    if (isWriteBlocked(txId)) return -1;
    if (txId != 0) {
        if (!journal) return -1;
        auto* ls = journal->activeLocks.get(txId);
        if (!ls) return -1;
        
        if (tableStore && clauses.size() > 0) {
            Array<u64> matchedIds = tableStore->getMatchingRowIds(clauses, ls->snapshotSeq, txId);
            for (usz i = 0; i < matchedIds.size(); ++i) {
                journal->trackRow(txId, matchedIds[i]);
            }
        }
        
        PendingWrite pw;
        pw.columns       = columns;
        pw.clauses       = clauses;
        pw.encryptionKey = encryptionKey;
        journal->lockPendingWrite(txId, pw);
        
        String payload = Journal::serializeTableWrite(columns, clauses, encryptionKey);
        journal->lockAppend(txId, JournalOpType::TABLE_WRITE, 0, payload);
        
        return 0;
    }

    if (!tableStore) return -1;
    int result = tableStore->write(columns, clauses, encryptionKey);
    if (result == 0) {
        if (watcher) {
            Map<String, String> mockRow;
            for (const auto& col : columns) {
                ParsedCol pc = parseCol(col.col);
                mockRow.set(pc.name, col.val);
            }
            watcher->notify(mockRow);
        }
        
        String payload = Journal::serializeTableWrite(columns, clauses, encryptionKey);
        journal->append(JournalOpType::TABLE_WRITE, 0, payload);
        if (payload.size() > 4000 || journal->isNearingCapacity()) flush();
    }
    return result;
}

int XylemEngine::writeVolatile(const Array<Clause>& columns, const Array<Clauses>& clauses, u64 txId, const String& encryptionKey) {
    ensureMounted();
    if (isWriteBlocked(txId)) return -1;
    if (!tableStore) return -1;
    // Bypass journal entirely and flag as volatile
    int result = tableStore->write(columns, clauses, encryptionKey, 0, true);
    if (result == 0 && watcher) {
        Map<String, String> mockRow;
        for (const auto& col : columns) {
            ParsedCol pc = parseCol(col.col);
            mockRow.set(pc.name, col.val);
        }
        watcher->notify(mockRow);
    }
    flush();
    return result;
}


bool XylemEngine::remove(const Array<Clauses>& clauses, u64 length, u64 as) {
    ensureMounted();
    if (isWriteBlocked(as)) return false;
    bool ok = tableStore->remove(clauses, length);
    if (ok) {
        String payload = Journal::serializeTableRemove(clauses, length);
        if (as != 0) {
            journal->lockAppend(as, JournalOpType::TABLE_REMOVE, 0, payload);
        } else {
            journal->append(JournalOpType::TABLE_REMOVE, 0, payload);
            if (payload.size() > 4000 || journal->isNearingCapacity()) flush();
        }
    }
    return ok;
}

// ─── Transactions (MVCC) ─────────────────────────────────────────────────────

u64 XylemEngine::lock(const Array<Clauses>& clauses, u64 /*id*/, bool requiresExplicitAs) {
    ensureMounted();
    if (!journal) return 0;
    u64 txId = journal->lockBegin(requiresExplicitAs);
    if (tableStore) {
        auto* ls = journal->activeLocks.get(txId);
        if (ls) ls->snapshotSeq = tableStore->currentSeq;
        if (clauses.size() > 0) {
            for (u64 rId : tableStore->allRowIds) {
                Map<String, String>* row = tableStore->fetchRow(rId);
                if (row && tableStore->evaluateClauses(*row, clauses) >= 0.0f) {
                    journal->trackRow(txId, rId);
                }
            }
        }
    }
    return txId;
}

u64 XylemEngine::commit(const Array<Clauses>& clauses, u64 id) {
    return lock(clauses, id, false);
}

bool XylemEngine::rollback(u64 id) {
    return journal ? journal->lockRollback(id) : false;
}

int XylemEngine::unlock(u64 id) {
    if (!journal) return -1;
    auto* ls = journal->activeLocks.get(id);
    if (!ls) return -1;

    // MVCC: Check for write-write conflicts before applying
    if (tableStore && tableStore->hasConflicts(ls->touchedRowIds, ls->snapshotSeq)) {
        journal->lockRollback(id);
        return -2; // Conflict — auto rolled back
    }

    // No conflicts — apply all buffered writes
    if (ls && tableStore) {
        for (const auto& pw : ls->pendingWrites) {
            int r = tableStore->write(pw.columns, pw.clauses, pw.encryptionKey);
            if (r == 0 && watcher) {
                Map<String, String> mockRow;
                for (const auto& col : pw.columns) {
                    ParsedCol pc = parseCol(col.col);
                    mockRow.set(pc.name, col.val);
                }
                watcher->notify(mockRow);
            }
        }
    }

    journal->lockCommit(id);

    // Semi-aggressive GC on commit
    if (tableStore) {
        u64 oldest = journal->oldestActiveSnapshot();
        tableStore->gcVersions(oldest);
        mergeUnusedDiffs();
    }
    flush();

    return 0;
}

void XylemEngine::mergeUnusedDiffs() {
    if (!blobStore || !tableStore || !journal) return;
    u64 oldest = journal->oldestActiveSnapshot();

    auto isDirectlyNeeded = [&](const String& hash) -> bool {
        if (hash == "XYLM_TABLE_ROOT" || hash == "XYLM_PENDING_FREEZES" || hash == "XYLM_REFS_USAGE") {
            return true;
        }
        auto* m = blobStore->index.get(hash);
        if (!m) return false;
        if (m->frozen) return true;
        bool isReferenced = tableStore->blobRefCounts.has(hash) && (*tableStore->blobRefCounts.get(hash) > 0);
        if (isReferenced) return true;
        if (m->isDiff && m->seq >= oldest) return true;
        return false;
    };

    bool progress = true;
    while (progress) {
        progress = false;
        
        for (auto it = blobStore->index.begin(); it != blobStore->index.end(); ++it) {
            String P = it->key;
            if (isDirectlyNeeded(P)) {
                continue;
            }
            
            // Find all children of P
            Array<String> children;
            for (auto it2 = blobStore->index.begin(); it2 != blobStore->index.end(); ++it2) {
                if (it2->value.isDiff && it2->value.baseHash == P) {
                    children.push(it2->key);
                }
            }
            
            if (children.size() == 0) {
                continue;
            }
            
            if (it->value.isDiff) {
                String GP = it->value.baseHash;
                String GP_data = blobStore->readHash(GP, 0, 0xFFFFFFFF);
                
                for (usz i = 0; i < children.size(); ++i) {
                    String C = children[i];
                    String C_data = blobStore->readHash(C, 0, 0xFFFFFFFF);
                    
                    XBDiff diff = XBDiff::create(GP_data, C_data);
                    String diffBin = diff.toBinary();
                    
                    auto* cMeta = blobStore->index.get(C);
                    u32 cRef = cMeta ? cMeta->ref : 0;
                    bool cUsed = cMeta ? cMeta->used : false;
                    u64 cSeq = cMeta ? cMeta->seq : 0;
                    
                    blobStore->removeHash(C);
                    blobStore->writeDiffHash(C, GP, diffBin, "");
                    
                    auto* newCMeta = blobStore->index.get(C);
                    if (newCMeta) {
                        newCMeta->ref = cRef;
                        newCMeta->used = cUsed;
                        newCMeta->seq = cSeq;
                    }
                }
            } else {
                for (usz i = 0; i < children.size(); ++i) {
                    String C = children[i];
                    String C_data = blobStore->readHash(C, 0, 0xFFFFFFFF);
                    
                    auto* cMeta = blobStore->index.get(C);
                    u32 cRef = cMeta ? cMeta->ref : 0;
                    bool cUsed = cMeta ? cMeta->used : false;
                    u64 cSeq = cMeta ? cMeta->seq : 0;
                    
                    blobStore->removeHash(C);
                    blobStore->writeHash(C, 0, C_data, "");
                    
                    auto* newCMeta = blobStore->index.get(C);
                    if (newCMeta) {
                        newCMeta->ref = cRef;
                        newCMeta->used = cUsed;
                        newCMeta->seq = cSeq;
                    }
                }
            }
            
            progress = true;
            break; // Iterator invalidated, restart
        }
    }
    
    Array<String> toRemove;
    for (auto it = blobStore->index.begin(); it != blobStore->index.end(); ++it) {
        String H = it->key;
        if (!isDirectlyNeeded(H)) {
            bool hasChildren = false;
            for (auto it2 = blobStore->index.begin(); it2 != blobStore->index.end(); ++it2) {
                if (it2->value.isDiff && it2->value.baseHash == H) {
                    hasChildren = true;
                    break;
                }
            }
            if (!hasChildren) {
                toRemove.push(H);
            }
        }
    }
    
    for (usz i = 0; i < toRemove.size(); ++i) {
        blobStore->removeHash(toRemove[i]);
    }
}

// ─── Blob API ─────────────────────────────────────────────────────────────────

String XylemEngine::writeHash(const String& content) {
    String hash = Sec::hash(content, 16);
    if (blobStore) blobStore->writeHash(hash, 0, content, "");
    return hash;
}

String XylemEngine::writeHash(const String& content, const String& hash) {
    if (blobStore) blobStore->writeHash(hash, 0, content, "");
    return hash;
}

String XylemEngine::writeHash(const String& content, u64 position) {
    String hash = Sec::hash(content, 16);
    if (blobStore) {
        if (blobStore->wouldOverlap(position, (u32)content.size())) return hash;
        blobStore->writeHash(hash, 0, content, "");
        fixHash(hash, position);
    }
    return hash;
}

int XylemEngine::fixHash(const String& hash, u64 position) {
    if (!blobStore) return -1;
    auto* meta = blobStore->index.get(hash);
    if (!meta) return -1;
    if (blobStore->wouldOverlap(position, meta->originalSize)) return -1;
    meta->fixedPosition = position;
    String data = blobStore->readHash(hash, 0, 0xFFFFFFFF);
    if (data.isEmpty()) return -1;
    return fixRaw(position, data) ? 0 : -1;
}



String XylemEngine::readHash(const String& hash, u64 min, u64 max) {
    return blobStore ? blobStore->readHash(hash, min, max) : String();
}

bool XylemEngine::removeHash(const String& hash) {
    return blobStore ? blobStore->removeHash(hash) : false;
}

// ─── Blob ref API ─────────────────────────────────────────────────────────────

u32 XylemEngine::getBlobRef(const String& hash) {
    ensureMounted();
    return blobStore ? blobStore->getBlobRef(hash) : 0;
}

String XylemEngine::getBlob(u32 ref) {
    ensureMounted();
    return blobStore ? blobStore->getBlob(ref) : String();
}

u32 XylemEngine::getBlobSize(u32 ref) {
    ensureMounted();
    return blobStore ? blobStore->getBlobSize(ref) : 0;
}

void XylemEngine::writeBlob(u32 ref, const String& content, u64 start) {
    ensureMounted();
    if (!blobStore) return;
    auto* hashPtr = blobStore->refToHash.get(ref);
    if (!hashPtr) return;
    String hash = *hashPtr;
    auto* m = blobStore->index.get(hash);
    
    if (m && m->frozen && m->fixedPosition != 0) {
        u64 oldSize = m->originalSize;
        u64 newSize = start + content.size();
        if (oldSize > newSize) newSize = oldSize;
        
        u32 blockSize = device->config.blockSize;
        u32 oldBlocks = (u32)((oldSize + blockSize - 1) / blockSize);
        u32 newBlocks = (u32)((newSize + blockSize - 1) / blockSize);
        
        if (newBlocks > oldBlocks) {
            u64 startPos = m->fixedPosition + oldBlocks * blockSize;
            u64 endPos = m->fixedPosition + newBlocks * blockSize;
            vaccum(startPos, endPos);
            freeze(startPos, endPos);
        }
        
        // Update size in metadata
        m->originalSize = newSize;
        m->length = newSize; // Ensure we keep track
        
        u32 startBlock = (u32)(m->fixedPosition / blockSize);
        u32 endBlock = (u32)((m->fixedPosition + newSize + blockSize - 1) / blockSize);
        u32 written = 0;
        
        String data = blobStore->readHash(hash, 0, 0xFFFFFFFF);
        if (start + content.size() > data.size()) {
            String newData; newData.allocate(newSize);
            newData.fill(0);
            for (usz i = 0; i < data.size(); ++i) newData[i] = data[i];
            for (usz i = 0; i < content.size(); ++i) newData[start + i] = content[i];
            data = newData;
        } else {
            for (usz i = 0; i < content.size(); ++i) data[start + i] = content[i];
        }
        
        for (u32 b = startBlock; b < endBlock; ++b) {
            u16 ec = (allocator && b < allocator->bam.size()) ? allocator->bam[b].eraseCount : 0u;
            u32 blockOffset = 0;
            if (b == startBlock) blockOffset = (u32)(m->fixedPosition % blockSize);
            
            String blk; blk.allocate(blockSize); blk.fill(0xFF);
            if (blockOffset > 0 || (written + blockSize - blockOffset > newSize)) {
                blk = device->readBlock(b, ec); // Read-modify-write for partial blocks
            }
            
            u32 toCopy = blockSize - blockOffset;
            if (written + toCopy > newSize) toCopy = newSize - written;
            
            for (u32 i = 0; i < toCopy; ++i) {
                blk[blockOffset + i] = data[written++];
            }
            
            device->writeBlock(b, ec, blk);
            if (allocator && b < allocator->bam.size()) {
                allocator->bam[b].setStatus(BlockStatus::USED);
                allocator->bam[b].setType(BlockType::RAW);
            }
        }
    } else {
        blobStore->writeBlob(ref, content, start);
    }
}

String XylemEngine::readBlob(u32 ref, u64 start, u64 end) {
    ensureMounted();
    return blobStore ? blobStore->readBlob(ref, start, end) : String();
}

void XylemEngine::setBlob(u32 ref, const String& hash) {
    ensureMounted();
    if (blobStore) blobStore->setBlob(ref, hash);
}

String XylemEngine::setBlob(u32 ref) {
    ensureMounted();
    return blobStore ? blobStore->setBlob(ref) : String();
}

bool XylemEngine::freezeBlob(u64 position, u32 blobRef) {
    ensureMounted();
    if (!blobStore) return false;
    
    u32 size = blobStore->getBlobSize(blobRef);
    if (size > 0) {
        vaccum(position, position + size);
    }
    
    return blobStore->freezeBlob(position, blobRef);
}

void XylemEngine::thawBlob(u32 blobRef) {
    ensureMounted();
    if (blobStore) blobStore->thawBlob(blobRef);
}

// ─── Vacuum / Freeze / Thaw ───────────────────────────────────────────────────

u64 XylemEngine::getUnusedBlockSpace() {
    if (!allocator || !device) return 0;
    u32 totalBlocks = allocator->bam.size();
    u32 emptyCount = 0;
    for (usz i = totalBlocks; i > 0; --i) {
        if (allocator->bam[i - 1].getStatus() == BlockStatus::FREE) {
            emptyCount++;
        } else {
            break;
        }
    }
    return (u64)emptyCount * config.blockSize;
}

void XylemEngine::vaccum() {
    ensureMounted();
    if (!allocator || !device) return;
    u32 totalBlocks = allocator->bam.size();
    // Shrink from the end: find last used block, free everything after
    u32 lastUsed = 0;
    for (u32 i = 0; i < totalBlocks; ++i) {
        if (allocator->bam[i].getStatus() == BlockStatus::USED ||
            allocator->bam[i].getStatus() == BlockStatus::RESERVED) {
            lastUsed = i;
        }
    }
    for (u32 i = lastUsed + 1; i < totalBlocks; ++i) {
        if (allocator->bam[i].getStatus() == BlockStatus::FREE) {
            // Already free, nothing to do
        } else if (allocator->bam[i].getStatus() != BlockStatus::RESERVED) {
            allocator->freeBlock(i);
        }
    }
    // Run GC
    if (journal && tableStore) {
        u64 oldest = journal->oldestActiveSnapshot();
        tableStore->gcVersions(oldest);
        mergeUnusedDiffs();
    }
    allocator->saveBam();
}

bool XylemEngine::vaccum(u64 startPos) {
    ensureMounted();
    if (!allocator || !device) return false;
    u32 blockSize = device->config.blockSize;
    u32 totalBlocks = allocator->bam.size();
    u32 startBlock = (u32)(startPos / blockSize);
    return vaccum(startPos, (u64)totalBlocks * blockSize);
}

bool XylemEngine::vaccum(u64 startPos, u64 endPos) {
    ensureMounted();
    if (!allocator || !device) return false;
    u32 blockSize = device->config.blockSize;
    u32 totalBlocks = allocator->bam.size();
    u32 startBlock = (u32)(startPos / blockSize);
    u32 endBlock = (u32)((endPos + blockSize - 1) / blockSize);
    if (endBlock > totalBlocks) endBlock = totalBlocks;

    // Check if any blocks in this range are RESERVED (frozen/pinned) — if so, can't vaccum
    for (u32 b = startBlock; b < endBlock; ++b) {
        if (allocator->bam[b].getStatus() == BlockStatus::RESERVED) {
            return false;
        }
    }

    // Move any blobs out of this range
    if (blobStore) {
        Array<String> blobsToMove;
        for (auto it = blobStore->index.begin(); it != blobStore->index.end(); ++it) {
            if (it->value.frozen) continue; // Don't move frozen blobs
            u32 cur = it->value.blockIdx;
            bool inRange = false;
            while (cur != 0) {
                if (cur >= startBlock && cur < endBlock) { inRange = true; break; }
                String blk = device->readBlock(cur, 0);
                if ((u32)blk.size() >= 19) {
                    const u8* ptr = (const u8*)blk.data();
                    ptr++; u8 isF = *ptr++; u8 kLen = *ptr++;
                    if (isF) ptr += kLen;
                    ptr += 8;
                    cur = *(const u32*)ptr;
                } else { cur = 0; }
            }
            if (inRange) blobsToMove.push(it->key);
        }
        for (usz i = 0; i < blobsToMove.size(); ++i) {
            String data = blobStore->readHash(blobsToMove[i], 0, 0);
            // Mark target blocks reserved so allocator doesn't use them
            for (u32 b = startBlock; b < endBlock; ++b) {
                if (allocator->bam[b].getStatus() == BlockStatus::USED) {
                    allocator->bam[b].setStatus(BlockStatus::RESERVED);
                }
            }
            blobStore->removeHash(blobsToMove[i]);
            blobStore->writeHash(blobsToMove[i], 0, data, "");
            // Unmark reserved blocks
            for (u32 b = startBlock; b < endBlock; ++b) {
                if (allocator->bam[b].getStatus() == BlockStatus::RESERVED &&
                    allocator->bam[b].getType() != BlockType::RAW) {
                    allocator->freeBlock(b);
                }
            }
        }
    }

    // Free remaining used blocks in range (tombstones, old data)
    for (u32 b = startBlock; b < endBlock; ++b) {
        if (allocator->bam[b].getStatus() == BlockStatus::USED) {
            allocator->freeBlock(b);
        }
    }

    // Run GC
    if (journal && tableStore) {
        u64 oldest = journal->oldestActiveSnapshot();
        tableStore->gcVersions(oldest);
        mergeUnusedDiffs();
    }
    allocator->buildHeap();
    allocator->saveBam();
    return true;
}

bool XylemEngine::freeze(u64 startPos, u64 endPos) {
    ensureMounted();
    if (!allocator || !device) return false;
    // First vacuum the region
    if (!vaccum(startPos, endPos)) return false;
    // Then mark all blocks as RESERVED
    u32 blockSize = device->config.blockSize;
    u32 startBlock = (u32)(startPos / blockSize);
    u32 endBlock = (u32)((endPos + blockSize - 1) / blockSize);
    u32 totalBlocks = allocator->bam.size();
    if (endBlock > totalBlocks) endBlock = totalBlocks;
    for (u32 b = startBlock; b < endBlock; ++b) {
        allocator->bam[b].setStatus(BlockStatus::RESERVED);
        allocator->bam[b].setType(BlockType::RAW);
    }
    allocator->buildHeap();
    allocator->saveBam();
    return true;
}

bool XylemEngine::thaw(u64 startPos, u64 endPos) {
    ensureMounted();
    if (!allocator || !device) return false;
    u32 blockSize = device->config.blockSize;
    u32 startBlock = (u32)(startPos / blockSize);
    u32 endBlock = (u32)((endPos + blockSize - 1) / blockSize);
    u32 totalBlocks = allocator->bam.size();
    if (endBlock > totalBlocks) endBlock = totalBlocks;
    for (u32 b = startBlock; b < endBlock; ++b) {
        if (allocator->bam[b].getStatus() == BlockStatus::RESERVED &&
            allocator->bam[b].getType() == BlockType::RAW) {
            allocator->bam[b].setStatus(BlockStatus::FREE);
            allocator->bam[b].setType(BlockType::FREE);
        }
    }
    allocator->buildHeap();
    allocator->saveBam();
    return true;
}

bool XylemEngine::freeze(u64 startPos, const String& data) {
    ensureMounted();
    return fixRaw(startPos, data);
}

// ─── File-like convenience API ────────────────────────────────────────────────

QueryResult XylemEngine::cat(const String& path, u64 start, u64 end) {
    ensureMounted();
    QueryResult res;
    
    String normPath = path;
    if (normPath.endsWith("/") && normPath.size() > 1) {
        normPath = normPath.slice(0, normPath.size() - 1);
    }
    
    Array<String> cols;
    cols.push("content");
    
    Array<Clauses> queryClauses;
    queryClauses.push(WHERE("name", "path", normPath));
    
    auto rows = read(cols, queryClauses);
    if (rows.size() > 0 && rows[0].has("content")) {
        String content = *rows[0].get("content");
        if (start > 0 || end > 0) {
            u64 s = start;
            u64 e = (end > 0) ? end : (u64)content.size();
            if (e > (u64)content.size()) e = (u64)content.size();
            if (s < e) content = content.slice((usz)s, (usz)e);
            else content = "";
        }
        Map<String, String> row;
        row.set("content", content);
        res.readRows.push(row);
        res.code = 0;
    } else {
        res.code = -1;
    }
    return res;
}

QueryResult XylemEngine::tee(const String& path, const String& content, u64 start, u64 end) {
    ensureMounted();
    QueryResult res;
    
    String normPath = path;
    if (normPath.endsWith("/") && normPath.size() > 1) {
        normPath = normPath.slice(0, normPath.size() - 1);
    }
    
    // 1. Check if the path already exists
    Array<String> readCols;
    readCols.push("id");
    readCols.push("type");
    Array<Clauses> readClauses;
    readClauses.push(WHERE("name", "path", normPath));
    auto rows = read(readCols, readClauses);
    
    if (rows.size() > 0 && rows[0].has("id")) {
        // Path exists. Update it.
        String existingId = *rows[0].get("id");
        
        Array<Clause> writeCols;
        String colName = (content.size() > 512) ? "content:blob" : "content";
        if (start > 0 || end > 0) {
            colName += "[" + String::from((long long)start) + ":" + String::from((long long)end) + "]";
        }
        writeCols.push({colName, "=", content});
        
        Array<Clauses> writeClauses;
        writeClauses.push(WHERE("id", "=", existingId));
        
        res.code = write(writeCols, writeClauses);
    } else {
        // Path does not exist. Create directories and file.
        Array<String> parts = normPath.split("/");
        Array<String> cleanParts;
        for (usz i = 0; i < parts.size(); ++i) {
            if (!parts[i].isEmpty()) cleanParts.push(parts[i]);
        }
        if (cleanParts.size() == 0) return QueryResult();
        
        String currentParentId = "0";
        String currentPathStr = "";
        
        // Traverse down the directories and create them if missing
        for (usz i = 0; i < cleanParts.size() - 1; ++i) {
            currentPathStr += "/" + cleanParts[i];
            
            // Check if this dir path exists
            Array<Clauses> dirClauses;
            dirClauses.push(WHERE("name", "path", currentPathStr));
            auto dirRows = read(readCols, dirClauses);
            
            if (dirRows.size() > 0 && dirRows[0].has("id")) {
                currentParentId = *dirRows[0].get("id");
            } else {
                u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
                String newId(rnd);
                
                Array<Clause> dirCols;
                dirCols.push({"name", "=", cleanParts[i]});
                dirCols.push({"parent_id", "=", currentParentId});
                dirCols.push({"id", "=", newId});
                dirCols.push({"type", "=", "dir"});
                dirCols.push({"perms", "=", "755"});
                
                write(dirCols);
                currentParentId = newId;
            }
        }
        
        // Create the file itself
        u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
        String fileId(rnd);
        
        Array<Clause> fileCols;
        fileCols.push({"name", "=", cleanParts[cleanParts.size() - 1]});
        fileCols.push({"parent_id", "=", currentParentId});
        fileCols.push({"id", "=", fileId});
        fileCols.push({"type", "=", "file"});
        fileCols.push({"perms", "=", "644"});
        
        String colName = (content.size() > 512) ? "content:blob" : "content";
        fileCols.push({colName, "=", content});
        
        res.code = write(fileCols);
    }
    
    flush();
    return res;
}

QueryResult XylemEngine::ls(const String& path) {
    ensureMounted();
    QueryResult res;
    
    String normPath = path;
    if (normPath.endsWith("/") && normPath.size() > 1) {
        normPath = normPath.slice(0, normPath.size() - 1);
    }
    
    String queryPath = normPath;
    if (queryPath.isEmpty() || queryPath == "/") {
        queryPath = "/*";
    } else {
        if (!queryPath.startsWith("/")) {
            queryPath = "/" + queryPath;
        }
        queryPath = queryPath + "/*";
    }
    
    Array<String> cols;
    cols.push("id");
    cols.push("name");
    cols.push("parent_id");
    cols.push("type");
    cols.push("perms");
    
    Array<Clauses> queryClauses;
    queryClauses.push(WHERE("name", "path", queryPath));
    
    res.readRows = read(cols, queryClauses);
    res.code = (int)res.readRows.size();
    return res;
}

bool XylemEngine::unlink(const String& path) {
    ensureMounted();
    if (!tableStore) return false;
    
    String normPath = path;
    if (normPath.endsWith("/") && normPath.size() > 1) {
        normPath = normPath.slice(0, normPath.size() - 1);
    }
    
    Array<Clauses> queryClauses;
    queryClauses.push(WHERE("name", "path", normPath + "/**"));
    
    bool r = remove(queryClauses);
    flush();
    return r;
}

// ─── Watch / Pull ─────────────────────────────────────────────────────────────

u64 XylemEngine::watch(const Array<Clauses>& clauses) {
    ensureMounted();
    return watcher ? watcher->watch(clauses) : 0;
}

u64 XylemEngine::watch(const Array<Clauses>& clauses, Func<void(Map<String,String>)> cb) {
    ensureMounted();
    return watcher ? watcher->watch(clauses, cb) : 0;
}

bool XylemEngine::unwatch(u64 id) {
    return watcher ? watcher->unwatch(id) : false;
}

Array<Map<String,String>> XylemEngine::pull(u64 id) {
    if (!watcher) return {};
    return watcher->pull(id);
}

} // namespace Xylem
