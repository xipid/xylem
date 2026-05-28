#include <Xylem/BlobStore.hpp>
#include <Xylem/CryptItem.hpp>

namespace Xylem {

BlobStore::BlobStore(BlockDevice* dev, Allocator* alloc, Array<String>* keys)
    : device(dev), allocator(alloc), globalKeys(keys) {}

// ─── helpers ────────────────────────────────────────────────────────────────

// Header overhead for first block: 3 bytes fixed + keyLen + 4+4+4 = 15 + keyLen
static inline u32 firstBlockOverhead(u32 keyLen) { return 15 + keyLen + 4; /* +4 CRC */ }
// Header overhead for continuation blocks: 3 + 0 + 4+4+4 = 15 + 4 CRC
static constexpr u32 CONT_OVERHEAD = 19;

static u32 firstBlockCap(u32 blockSize, u32 keyLen) {
    u32 oh = firstBlockOverhead(keyLen);
    return (blockSize > oh) ? blockSize - oh : 0;
}
static u32 contBlockCap(u32 blockSize) {
    return (blockSize > CONT_OVERHEAD) ? blockSize - CONT_OVERHEAD : 0;
}

// ─── public API ─────────────────────────────────────────────────────────────



String BlobStore::readHash(const String& hash, u64 minOffset, u64 maxOffset) {
    if (!bloomMayExist(hash)) return String();
    auto* m = index.get(hash);
    if (!m) return String();

    String data;
    if (device && device->config.onDeviceRead) {
        if (m->frozen && m->fixedPosition != 0) {
            u32 blockSize = device->config.blockSize;
            u32 startBlock = (u32)(m->fixedPosition / blockSize);
            u32 endBlock = (u32)((m->fixedPosition + m->originalSize + blockSize - 1) / blockSize);
            data.allocate(m->originalSize);
            u32 written = 0;
            for (u32 b = startBlock; b < endBlock && written < m->originalSize; ++b) {
                u16 ec = (allocator && b < allocator->bam.size()) ? allocator->bam[b].eraseCount : 0u;
                String blk = device->readBlock(b, ec);
                
                u32 blockOffset = 0;
                if (b == startBlock) blockOffset = (u32)(m->fixedPosition % blockSize);
                
                u32 toCopy = blockSize - blockOffset;
                if (written + toCopy > m->originalSize) toCopy = m->originalSize - written;
                
                for (u32 i = 0; i < toCopy && blockOffset + i < blk.size(); ++i) {
                    data[written++] = blk[blockOffset + i];
                }
            }
        } else if (m->blockIdx != 0) {
            data = readChain(m->blockIdx);
        } else {
            return String();
        }
    } else {
        return String(); // No device or invalid block
    }

    String savedDict;
    bool isTableRoot = (hash == "XYLM_TABLE_ROOT");
#ifdef XI_ZSTD_ENABLED
    if (isTableRoot && zstd.dictionary.size() > 0) {
        savedDict = zstd.dictionary[0];
        zstd.setDictionary(String());
    }
#endif

#ifdef XI_ZSTD_ENABLED
    data = zstd.decompress(data);
#endif

#ifdef XI_ZSTD_ENABLED
    if (isTableRoot && !savedDict.isEmpty()) {
        zstd.setDictionary(savedDict);
    }
#endif

    // Attempt decryption using global keys
    if (globalKeys && globalKeys->size() > 0) {
        String decrypted = CryptItem::decrypt(data, *globalKeys);
        if (!decrypted.isEmpty()) data = decrypted;
    }

    if (maxOffset > 0 && maxOffset > minOffset) {
        u64 len = maxOffset - minOffset;
        if (len > (u64)data.size() - minOffset) len = (u64)data.size() - minOffset;
        return data.slice((usz)minOffset, (usz)len);
    }
    if (minOffset > 0) {
        return data.slice((usz)minOffset);
    }
    return data;
}

bool BlobStore::writeHash(const String& hash, u64 /*minOffset*/, const String& data,
                          const String& encryptionKey) {
    // Zero-cost dedup for true content-addressed blobs
    if (index.has(hash)) return true;

    if (!device || !device->config.onDeviceWrite || !allocator) {
        // No device available — store a placeholder so the key registers
        BlobMeta meta = { 0, 0, 0, (u32)data.size() };
        index.set(hash, meta);
        return false;
    }

    u32 origSize = (u32)data.size();

    // Encrypt if key provided
    String payload = data;
    if (!encryptionKey.isEmpty()) {
        payload = CryptItem::encrypt(data, encryptionKey);
    }

    String savedDict;
    bool isTableRoot = (hash == "XYLM_TABLE_ROOT");
#ifdef XI_ZSTD_ENABLED
    if (isTableRoot && zstd.dictionary.size() > 0) {
        savedDict = zstd.dictionary[0];
        zstd.setDictionary(String());
    }
#endif

#ifdef XI_ZSTD_ENABLED
    payload = zstd.compress(payload);
#endif

#ifdef XI_ZSTD_ENABLED
    if (isTableRoot && !savedDict.isEmpty()) {
        zstd.setDictionary(savedDict);
    }
#endif

    u32 blockSize = device->config.blockSize;
    u32 keyLen    = (u32)hash.size();
    u32 fCap      = firstBlockCap(blockSize, keyLen);
    u32 cCap      = contBlockCap(blockSize);
    if (fCap == 0 || cCap == 0) return false;

    u32 totalLen = (u32)payload.size();

    // Calculate number of blocks required
    u32 numBlocks = 1;
    if (totalLen > fCap) {
        u32 remaining = totalLen - fCap;
        numBlocks += (remaining + cCap - 1) / cCap;
    }

    // Allocate all blocks upfront
    InlineArray<u32> blockIdxs;
    blockIdxs.allocate(numBlocks);
    for (u32 i = 0; i < numBlocks; ++i) {
        u32 bIdx = allocator->allocBlock(BlockType::BLOB);
        if (bIdx == 0) {
            // Rollback already-allocated blocks
            for (u32 j = 0; j < i; ++j) {
                allocator->bam[blockIdxs[j]].setStatus(BlockStatus::FREE);
                allocator->bam[blockIdxs[j]].setType(BlockType::FREE);
                allocator->freeHeap.push({allocator->bam[blockIdxs[j]].eraseCount, blockIdxs[j]});
            }
            return false;
        }
        blockIdxs[i] = bIdx;
    }

    // Write each block
    u32 dataOffset = 0;
    for (u32 b = 0; b < numBlocks; ++b) {
        bool isFirst  = (b == 0);
        u32  cap      = isFirst ? fCap : cCap;
        u32  chunkLen = (totalLen - dataOffset < cap) ? (totalLen - dataOffset) : cap;
        u32  nextBlk  = (b + 1 < numBlocks) ? blockIdxs[b + 1] : 0u;

        String buf; buf.allocate(blockSize);
        buf.fill(0xFF);
        u8* ptr       = (u8*)buf.data();
        u8* blkStart  = ptr;

        *ptr++ = (u8)BlockType::BLOB;
        *ptr++ = isFirst ? 1u : 0u;
        *ptr++ = isFirst ? (u8)keyLen : 0u;
        if (isFirst) {
            for (u32 k = 0; k < keyLen; ++k) *ptr++ = hash[k];
        }
        *(u32*)ptr = isFirst ? totalLen : 0u; ptr += 4;
        *(u32*)ptr = chunkLen;                ptr += 4;
        *(u32*)ptr = nextBlk;                 ptr += 4;
        for (u32 k = 0; k < chunkLen; ++k) *ptr++ = payload[dataOffset + k];

        u32 payloadLen = (u32)(ptr - blkStart);
        u32 c = crc32(blkStart, payloadLen);
        *(u32*)ptr = c; ptr += 4;

        u32 blkDataLen = (u32)(ptr - blkStart);
        u16 eraseCount = (blockIdxs[b] < allocator->bam.size())
                         ? allocator->bam[blockIdxs[b]].eraseCount : 0u;
        device->writeBlock(blockIdxs[b], eraseCount, buf.slice(0, blkDataLen));
        dataOffset += chunkLen;
    }

    BlobMeta meta = { blockIdxs[0], 0, totalLen, origSize, 0 };
    index.set(hash, meta);
    bloomInsert(hash);
    return true;
}

bool BlobStore::removeHash(const String& hash) {
    auto* m = index.get(hash);
    if (!m) return false;

    // Free the entire block chain
    if (m->frozen && m->fixedPosition != 0) {
        if (thawCallback) thawCallback(m->fixedPosition, m->fixedPosition + m->originalSize);
    } else if (device && allocator && m->blockIdx != 0) {
        u32 blockIdx = m->blockIdx;
        while (blockIdx != 0) {
            String blockData = device->readBlock(
                blockIdx,
                (blockIdx < allocator->bam.size()) ? allocator->bam[blockIdx].eraseCount : 0u);

            u32 nextBlock = 0;
            if ((u32)blockData.size() >= CONT_OVERHEAD) {
                const u8* ptr = (const u8*)blockData.data();
                ptr++;               // type
                u8 isFirst = *ptr++;
                u8 kLen    = *ptr++;
                if (isFirst) ptr += kLen;
                ptr += 4; // totalDataLen
                ptr += 4; // thisChunkLen
                nextBlock = *(u32*)ptr;
            }

            allocator->freeBlock(blockIdx);
            blockIdx = nextBlock;
        }
    }

    index.remove(hash);
    bloomRebuild();
    return true;
}

bool BlobStore::fixBlob(const String& hash, u64 byteAddress) {
    auto* m = index.get(hash);
    if (!m || !device || !allocator) return false;

    u32 blockSize = device->config.blockSize;
    u32 startBlock = (u32)(byteAddress / blockSize);
    u32 requiredBlocks = (m->originalSize + blockSize - 1) / blockSize;

    if (startBlock + requiredBlocks > allocator->bam.size()) return false;

    // Read the entire blob contents first
    String data = readHash(hash, 0, 0xFFFFFFFF);
    if (data.isEmpty() && m->originalSize > 0) return false;

    // Save old meta before removing
    u32 ref = m->ref;
    bool used = m->used;

    // Free the old blocks
    removeHash(hash);

    // Write the raw bytes directly to the fixed position
    u32 written = 0;
    for (u32 b = startBlock; b < startBlock + requiredBlocks; ++b) {
        u16 ec = allocator->bam[b].eraseCount;
        u32 blockOffset = 0;
        if (b == startBlock) blockOffset = (u32)(byteAddress % blockSize);
        
        String blk; blk.allocate(blockSize); blk.fill(0xFF);
        if (blockOffset > 0 || (written + blockSize - blockOffset > data.size())) {
            blk = device->readBlock(b, ec);
        }
        
        u32 toCopy = blockSize - blockOffset;
        if (written + toCopy > data.size()) toCopy = data.size() - written;
        
        for (u32 i = 0; i < toCopy; ++i) {
            blk[blockOffset + i] = data[written++];
        }
        
        device->writeBlock(b, ec, blk);
        allocator->bam[b].setStatus(BlockStatus::USED);
        allocator->bam[b].setType(BlockType::RAW);
    }

    BlobMeta meta = { 0, 0, (u32)data.size(), (u32)data.size(), byteAddress, ref, true, used };
    index.set(hash, meta);
    bloomInsert(hash);
    return true;
}

void BlobStore::scanFromDevice() {
    if (!device || !device->config.onDeviceRead || !allocator) return;
    index.clear();

    for (u32 b = 0; b < allocator->bam.size(); ++b) {
        if (allocator->bam[b].getType() != BlockType::BLOB) continue;
        if (allocator->bam[b].getStatus() != BlockStatus::USED) continue;

        u16 eraseCount = allocator->bam[b].eraseCount;
        String blockData = device->readBlock(b, eraseCount);
        if ((u32)blockData.size() < 20) continue;

        const u8* ptr = (const u8*)blockData.data();
        if (*ptr++ != (u8)BlockType::BLOB) continue;
        u8 isFirst = *ptr++;
        if (!isFirst) continue; // Only index first-block entries

        u8 keyLen = *ptr++;
        if (keyLen == 0 || keyLen > 200) continue;

        String key; key.allocate(keyLen);
        for (u8 i = 0; i < keyLen; ++i) key[i] = *ptr++;

        u32 totalDataLen = *(u32*)ptr; ptr += 4;
        /* u32 thisChunkLen = *(u32*)ptr; */ ptr += 4;
        /* u32 nextBlock    = *(u32*)ptr; */ ptr += 4;

        BlobMeta meta = { b, 0, totalDataLen, totalDataLen, 0 };
        index.set(key, meta);
    }
    bloomRebuild();
}

void BlobStore::trainDictionary(u32 maxSamples) {
#ifdef XI_ZSTD_ENABLED
    if (index.size() < 10) return;
    if (zstd.dictionary.size() > 0) return;
    
    Array<String> samples;
    u32 count = 0;
    for (auto it = index.begin(); it != index.end() && count < maxSamples; ++it, ++count) {
        if (it->value.blockIdx != 0) {
            String data = readChain(it->value.blockIdx);
            if (!data.isEmpty()) {
                samples.push(data);
            }
        }
    }
    if (samples.size() >= 10) {
        zstd.train(samples);
    }
#endif
}

// ─── private ────────────────────────────────────────────────────────────────

void BlobStore::bloomInsert(const String& hash) {
    if (hash.size() < 16) return;
    const u8* h = (const u8*)hash.data();
    u64 h1 = *(u64*)&h[0];           
    u64 h2 = *(u64*)&h[8];           
    u64 h3 = h1 ^ h2;               
    bloom[(h1 % BLOOM_SIZE_BITS) / 8] |= (1 << (h1 % BLOOM_SIZE_BITS % 8));
    bloom[(h2 % BLOOM_SIZE_BITS) / 8] |= (1 << (h2 % BLOOM_SIZE_BITS % 8));
    bloom[(h3 % BLOOM_SIZE_BITS) / 8] |= (1 << (h3 % BLOOM_SIZE_BITS % 8));
}

bool BlobStore::bloomMayExist(const String& hash) const {
    if (hash.size() < 16) return true;
    const u8* h = (const u8*)hash.data();
    u64 h1 = *(u64*)&h[0], h2 = *(u64*)&h[8], h3 = h1 ^ h2;
    if (!(bloom[(h1 % BLOOM_SIZE_BITS) / 8] & (1 << (h1 % BLOOM_SIZE_BITS % 8)))) return false;
    if (!(bloom[(h2 % BLOOM_SIZE_BITS) / 8] & (1 << (h2 % BLOOM_SIZE_BITS % 8)))) return false;
    if (!(bloom[(h3 % BLOOM_SIZE_BITS) / 8] & (1 << (h3 % BLOOM_SIZE_BITS % 8)))) return false;
    return true;
}

void BlobStore::bloomRebuild() {
    for (u32 i = 0; i < BLOOM_SIZE_BYTES; ++i) bloom[i] = 0;
    for (auto it = index.begin(); it != index.end(); ++it) {
        bloomInsert(it->key);
    }
}

String BlobStore::readChain(u32 blockIdx) const {
    if (!device || !allocator) return String();

    u16 firstEc = (blockIdx < allocator->bam.size()) ? allocator->bam[blockIdx].eraseCount : 0u;
    String firstBlk = device->readBlock(blockIdx, firstEc);
    if ((u32)firstBlk.size() < CONT_OVERHEAD) return String();

    const u8* ptr = (const u8*)firstBlk.data();
    ptr++;                     // type
    u8 isF = *ptr++;
    u8 kLen = *ptr++;
    if (!isF) return String(); // Must start with a first block
    ptr += kLen;               // key
    u32 totalLen = *(u32*)ptr; ptr += 4;
    u32 cLen = *(u32*)ptr; ptr += 4;
    u32 next = *(u32*)ptr; ptr += 4;

    if (totalLen == 0) return String();

    String result; result.allocate(totalLen);
    u32 written = 0;

    // Copy first block payload
    for (u32 i = 0; i < cLen && written < totalLen; ++i) {
        result[written++] = *ptr++;
    }

    u32 cur = next;
    while (cur != 0 && written < totalLen) {
        u16 ec = (cur < allocator->bam.size()) ? allocator->bam[cur].eraseCount : 0u;
        String blk = device->readBlock(cur, ec);
        if ((u32)blk.size() < CONT_OVERHEAD) break;

        const u8* cPtr = (const u8*)blk.data();
        cPtr++;             // type
        cPtr++;             // isFirst
        cPtr++;             // keyLen
        cPtr += 4;          // totalDataLen
        u32 chunkLen = *(u32*)cPtr; cPtr += 4;
        u32 nextBlock = *(u32*)cPtr; cPtr += 4;

        for (u32 i = 0; i < chunkLen && written < totalLen; ++i) {
            result[written++] = *cPtr++;
        }

        cur = nextBlock;
    }

    return result.slice(0, written);
}

bool BlobStore::wouldOverlap(u64 position, u32 size) const {
    if (!device) return false;
    u32 blockSize = device->config.blockSize;
    u64 nStart = position;
    u64 nEnd = position + size;
    for (auto it = index.begin(); it != index.end(); ++it) {
        if (it->value.fixedPosition == 0) continue;
        u64 fStart = it->value.fixedPosition;
        u64 fEnd = fStart + (u64)it->value.originalSize;
        // Round up to block boundaries
        fEnd = ((fEnd + blockSize - 1) / blockSize) * blockSize;
        u64 nEndAligned = ((nEnd + blockSize - 1) / blockSize) * blockSize;
        if (nStart < fEnd && nEndAligned > fStart) return true;
    }
    return false;
}

bool BlobStore::isFixed(const String& hash) const {
    auto* m = index.get(hash);
    return m && m->fixedPosition != 0;
}

// ─── Ref-based public API ───────────────────────────────────────────────────

u32 BlobStore::allocRef(const String& hash) {
    auto* m = index.get(hash);
    if (!m) return 0;
    if (m->ref != 0) return m->ref; // Already has a ref
    
    // Check hashToRef
    auto* existingRef = hashToRef.get(hash);
    if (existingRef) {
        m->ref = *existingRef;
        return m->ref;
    }
    
    u32 ref = nextRef++;
    m->ref = ref;
    refToHash.set(ref, hash);
    hashToRef.set(hash, ref);
    return ref;
}

u32 BlobStore::getBlobRef(const String& hash) {
    auto* r = hashToRef.get(hash);
    if (r) return *r;
    if (!index.has(hash)) return 0;
    return allocRef(hash);
}

String BlobStore::getBlob(u32 ref) {
    auto* hash = refToHash.get(ref);
    if (!hash) return String();
    return readHash(*hash, 0, 0xFFFFFFFF);
}

u32 BlobStore::getBlobSize(u32 ref) {
    auto* hash = refToHash.get(ref);
    if (!hash) return 0;
    auto* m = index.get(*hash);
    if (!m) return 0;
    return m->originalSize;
}

void BlobStore::writeBlob(u32 ref, const String& content, u64 start) {
    auto* hash = refToHash.get(ref);
    if (!hash) return;
    
    // Note: To support partial updates, we read existing, modify, and re-write.
    String data = readHash(*hash, 0, 0xFFFFFFFF);
    if (start + content.size() > data.size()) {
        String newData; newData.allocate(start + content.size());
        newData.fill(0);
        for (usz i = 0; i < data.size(); ++i) newData[i] = data[i];
        for (usz i = 0; i < content.size(); ++i) newData[start + i] = content[i];
        data = newData;
    } else {
        for (usz i = 0; i < content.size(); ++i) data[start + i] = content[i];
    }
    
    // We rewrite using the same hash mapping, which updates the block device content
    removeHash(*hash);
    writeHash(*hash, 0, data, "");
    allocRef(*hash); // re-assign ref mapping to new metadata
    resolvePendingFreezes();
}

String BlobStore::readBlob(u32 ref, u64 start, u64 end) {
    auto* hash = refToHash.get(ref);
    if (!hash) return String();
    if (end == 0) end = 0xFFFFFFFF;
    return readHash(*hash, start, end);
}

void BlobStore::setBlob(u32 ref, const String& hash) {
    refToHash.set(ref, hash);
    hashToRef.set(hash, ref);
    if (auto* m = index.get(hash)) {
        m->ref = ref;
    } else {
        // Create an empty placeholder if it doesn't exist?
        // Usually it should exist or be written shortly.
    }
}

String BlobStore::setBlob(u32 ref) {
    // This expects to auto-hash? 
    // Wait, the API `String setBlob(u32 ref);` says "causes Xylem to hash it then write the hash."
    // Actually, if we just have the ref, we only have its content if it's already mapped.
    // If it's already mapped, we can compute the hash of its content.
    auto* oldHash = refToHash.get(ref);
    if (!oldHash) return String();
    String data = readHash(*oldHash, 0, 0xFFFFFFFF);
    String newHash = Sec::hash(data, 16);
    if (newHash != *oldHash) {
        removeHash(*oldHash);
        writeHash(newHash, 0, data, "");
        refToHash.set(ref, newHash);
        hashToRef.set(newHash, ref);
        hashToRef.remove(*oldHash);
        if (auto* m = index.get(newHash)) m->ref = ref;
    }
    return newHash;
}

bool BlobStore::freezeBlob(u64 position, u32 blobRef) {
    auto* hash = refToHash.get(blobRef);
    if (!hash || !index.has(*hash)) {
        PendingFreeze pf;
        pf.position = position;
        pf.ref = blobRef;
        pendingFreezes.push(pf);
        savePendingFreezes();
        return true;
    }
    auto* m = index.get(*hash);
    if (m) {
        m->frozen = true;
        bool ok = fixBlob(*hash, position);
        if (ok) {
            m = index.get(*hash); // Might have been reallocated
            if (m) m->frozen = true;
        }
        return ok;
    }
    return false;
}

void BlobStore::thawBlob(u32 blobRef) {
    auto* hash = refToHash.get(blobRef);
    if (hash) {
        auto* m = index.get(*hash);
        if (m) m->frozen = false;
    }
}

void BlobStore::resolvePendingFreezes() {
    bool changed = false;
    Array<PendingFreeze> remaining;
    for (usz i = 0; i < pendingFreezes.size(); ++i) {
        auto* hash = refToHash.get(pendingFreezes[i].ref);
        if (hash && index.has(*hash)) {
            freezeBlob(pendingFreezes[i].position, pendingFreezes[i].ref);
            changed = true;
        } else {
            remaining.push(pendingFreezes[i]);
        }
    }
    if (changed) {
        pendingFreezes = remaining;
        savePendingFreezes();
    }
}

void BlobStore::savePendingFreezes() {
    String data;
    // Format: [4B count] then array of [8B pos, 4B ref]
    u32 count = (u32)pendingFreezes.size();
    data.allocate(4 + count * 12);
    u8* ptr = (u8*)data.data();
    *(u32*)ptr = count; ptr += 4;
    for (u32 i = 0; i < count; ++i) {
        *(u64*)ptr = pendingFreezes[i].position; ptr += 8;
        *(u32*)ptr = pendingFreezes[i].ref; ptr += 4;
    }
    removeHash("XYLM_PENDING_FREEZES");
    writeHash("XYLM_PENDING_FREEZES", 0, data, "");
}

void BlobStore::loadPendingFreezes() {
    pendingFreezes.clear();
    String data = readHash("XYLM_PENDING_FREEZES", 0, 0xFFFFFFFF);
    if (data.size() >= 4) {
        const u8* ptr = (const u8*)data.data();
        u32 count = *(u32*)ptr; ptr += 4;
        if (data.size() >= 4 + count * 12) {
            for (u32 i = 0; i < count; ++i) {
                PendingFreeze pf;
                pf.position = *(u64*)ptr; ptr += 8;
                pf.ref = *(u32*)ptr; ptr += 4;
                pendingFreezes.push(pf);
            }
        }
    }
}

void BlobStore::saveRefsAndUsage() {
    // Collect all refs and usage
    // Format: [4B nextRef] [4B count] [count * (4B ref, 1B used, 1B hashLen, hashBytes)]
    String data;
    u32 count = 0;
    usz needed = 8;
    for (auto it = refToHash.begin(); it != refToHash.end(); ++it) {
        count++;
        needed += 4 + 1 + 1 + it->value.size();
    }
    data.allocate(needed);
    u8* ptr = (u8*)data.data();
    *(u32*)ptr = nextRef; ptr += 4;
    *(u32*)ptr = count; ptr += 4;
    for (auto it = refToHash.begin(); it != refToHash.end(); ++it) {
        *(u32*)ptr = it->key; ptr += 4;
        auto* m = index.get(it->value);
        *ptr++ = (m && m->used) ? 1 : 0;
        *ptr++ = (u8)it->value.size();
        for (usz i = 0; i < it->value.size(); ++i) *ptr++ = it->value[i];
    }
    removeHash("XYLM_REFS_USAGE");
    writeHash("XYLM_REFS_USAGE", 0, data, "");
}

void BlobStore::loadRefsAndUsage() {
    refToHash.clear();
    hashToRef.clear();
    String data = readHash("XYLM_REFS_USAGE", 0, 0xFFFFFFFF);
    if (data.size() >= 8) {
        const u8* ptr = (const u8*)data.data();
        nextRef = *(u32*)ptr; ptr += 4;
        u32 count = *(u32*)ptr; ptr += 4;
        const u8* end = (const u8*)data.data() + data.size();
        for (u32 i = 0; i < count && ptr < end; ++i) {
            if (end - ptr < 6) break;
            u32 ref = *(u32*)ptr; ptr += 4;
            bool used = (*ptr++ != 0);
            u8 hLen = *ptr++;
            if (end - ptr < hLen) break;
            String hash; hash.allocate(hLen);
            for (u8 j = 0; j < hLen; ++j) hash[j] = *ptr++;
            refToHash.set(ref, hash);
            hashToRef.set(hash, ref);
            if (auto* m = index.get(hash)) {
                m->ref = ref;
                m->used = used;
            }
        }
    }
}


void BlobStore::setBlobUsed(u32 ref, bool used) {
    auto* hash = refToHash.get(ref);
    if (!hash) return;
    auto* m = index.get(*hash);
    if (m) {
        m->used = used;
        saveRefsAndUsage();
    }
}

} // namespace Xylem
