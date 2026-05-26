#ifndef XYLEM_CACHE_HPP
#define XYLEM_CACHE_HPP

#include <Xylem/Format.hpp>
#include <Xylem/BlockDevice.hpp>
#include <Collection/Map.hpp>
#include <Collection/InlineArray.hpp>

namespace Xylem {

using namespace Collection;

struct CacheEntry {
    u32 blockIdx;
    InlineArray<u8> decompressed;
    bool dirty;
    u64 accessSeq;
};

class Cache {
public:
    BlockDevice* device;
    usz maxCacheSize;
    usz currentUsedBytes;
    u64 accessCounter;
    
    Map<u32, CacheEntry> entries;

    Cache(BlockDevice* dev, usz maxCache);

    InlineArray<u8> get(u32 blockIdx);
    void put(u32 blockIdx, const InlineArray<u8>& data);
    void flushAll();

private:
    void evict();
};

} // namespace Xylem

#endif // XYLEM_CACHE_HPP
