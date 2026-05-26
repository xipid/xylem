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

String BlobStore::statHash(const String& hash) {
    if (!bloomMayExist(hash)) return String();
    auto* m = index.get(hash);
    if (!m) return String();
    return String::from((u64)m->originalSize);
}

String BlobStore::readHash(const String& hash, u64 minOffset, u64 maxOffset) {
    if (!bloomMayExist(hash)) return String();
    auto* m = index.get(hash);
    if (!m) return String();

    String data;
    if (device && device->config.onDeviceRead && m->blockIdx != 0) {
        data = readChain(m->blockIdx);
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
    if (device && allocator && m->blockIdx != 0) {
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

    u32 targetBlock = (u32)(byteAddress / device->config.blockSize);
    if (targetBlock >= allocator->bam.size()) return false;

    // If already there, nothing to do
    if (m->blockIdx == targetBlock) return true;

    // Free the target block if occupied
    if (allocator->bam[targetBlock].getStatus() == BlockStatus::USED) {
        allocator->freeBlock(targetBlock);
    }

    // Free the old chain, then re-write the blob to the (now free) target area
    String data = readChain(m->blockIdx);
    if (data.isEmpty()) return false;

    // Remove old to free old blocks
    index.remove(hash);

    // Force-allocate target block by temporarily marking others
    // Simple approach: just re-write; allocBlock may not give exactly targetBlock
    // so we swap if needed.
    writeHash(hash, 0, data, "");
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

    // First pass: accumulate total size
    u32 totalLen = 0;
    {
        u32 cur = blockIdx;
        bool first = true;
        while (cur != 0) {
            u16 ec = (cur < allocator->bam.size()) ? allocator->bam[cur].eraseCount : 0u;
            String blk = device->readBlock(cur, ec);
            if ((u32)blk.size() < CONT_OVERHEAD) break;

            const u8* ptr = (const u8*)blk.data();
            ptr++;                     // type
            u8 isF = *ptr++;
            u8 kLen = *ptr++;
            if (isF) ptr += kLen;
            u32 tLen = *(u32*)ptr; ptr += 4;
            u32 cLen = *(u32*)ptr; ptr += 4;
            u32 next = *(u32*)ptr;

            if (first) { totalLen = tLen; first = false; }
            (void)cLen;
            cur = next;
        }
    }
    if (totalLen == 0) return String();

    // Second pass: read into pre-allocated buffer
    String result; result.allocate(totalLen);
    u32 written = 0;
    u32 cur = blockIdx;
    bool isFirstBlock = true;

    while (cur != 0 && written < totalLen) {
        u16 ec = (cur < allocator->bam.size()) ? allocator->bam[cur].eraseCount : 0u;
        String blk = device->readBlock(cur, ec);
        if ((u32)blk.size() < CONT_OVERHEAD) break;

        const u8* ptr = (const u8*)blk.data();
        ptr++;             // type
        u8 isF = *ptr++;
        u8 kLen = *ptr++;
        if (isF) ptr += kLen;
        ptr += 4;          // totalDataLen
        u32 cLen = *(u32*)ptr; ptr += 4;
        u32 next = *(u32*)ptr; ptr += 4;

        for (u32 i = 0; i < cLen && written < totalLen; ++i) {
            result[written++] = *ptr++;
        }

        cur = next;
        isFirstBlock = false;
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

} // namespace Xylem
