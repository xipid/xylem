#include <Xylem/Cache.hpp>

namespace Xylem {

Cache::Cache(BlockDevice* dev, usz maxCache) 
    : device(dev), maxCacheSize(maxCache), currentUsedBytes(0), accessCounter(0) {}

InlineArray<u8> Cache::get(u32 blockIdx) {
    accessCounter++;
    if (auto* entry = entries.get(blockIdx)) {
        entry->accessSeq = accessCounter;
        return entry->decompressed;
    }

    // Cache miss: logic hooks up to decompress device block here.
    InlineArray<u8> data;
    
    CacheEntry entry = { blockIdx, data, false, accessCounter };
    entries.set(blockIdx, entry);
    currentUsedBytes += data.size();

    while (currentUsedBytes > maxCacheSize && entries.size() > 0) {
        evict();
    }

    return data;
}

void Cache::put(u32 blockIdx, const InlineArray<u8>& data) {
    accessCounter++;
    if (auto* pEntry = entries.get(blockIdx)) {
        CacheEntry& entry = *pEntry;
        currentUsedBytes -= entry.decompressed.size();
        entry.decompressed = data;
        entry.dirty = true;
        entry.accessSeq = accessCounter;
        currentUsedBytes += data.size();
    } else {
        CacheEntry entry = { blockIdx, data, true, accessCounter };
        entries.set(blockIdx, entry);
        currentUsedBytes += data.size();
    }

    while (currentUsedBytes > maxCacheSize && entries.size() > 0) {
        evict();
    }
}

void Cache::evict() {
    u32 oldestBlock = 0;
    u64 oldestSeq = (u64)-1;

    for (auto& pair : entries) {
        if (pair.value.accessSeq < oldestSeq) {
            oldestSeq = pair.value.accessSeq;
            oldestBlock = pair.key;
        }
    }

    if (oldestSeq != (u64)-1) {
        CacheEntry entry = *entries.get(oldestBlock);
        if (entry.dirty) {
            if (device) {
                device->writeBlock(entry.blockIdx, 0, String(entry.decompressed));
            }
        }
        currentUsedBytes -= entry.decompressed.size();
        entries.remove(oldestBlock);
    }
}

void Cache::flushAll() {
    for (auto& pair : entries) {
        if (pair.value.dirty) {
            if (device) {
                device->writeBlock(pair.key, 0, String(pair.value.decompressed));
            }
            pair.value.dirty = false;
        }
    }
}

} // namespace Xylem
