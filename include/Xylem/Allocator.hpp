#ifndef XYLEM_ALLOCATOR_HPP
#define XYLEM_ALLOCATOR_HPP

#include <Xylem/Format.hpp>
#include <Xylem/BlockDevice.hpp>
#include <Collection/InlineArray.hpp>

namespace Xylem {

using namespace Collection;

struct BlockMeta {
    u16 eraseCount;
    u8 packed; // bits 7-6: status, bits 5-2: blockType, bits 1-0: reserved

    BlockStatus getStatus() const { return (BlockStatus)(packed >> 6); }
    void setStatus(BlockStatus s) { packed = (packed & 0x3F) | (((u8)s & 0x03) << 6); }
    
    BlockType getType() const { return (BlockType)((packed >> 2) & 0x0F); }
    void setType(BlockType t) { packed = (packed & 0xC3) | (((u8)t & 0x0F) << 2); }
};

struct AllocHeapEntry {
    u16 eraseCount;
    u32 blockIdx;
    
    bool operator<(const AllocHeapEntry& o) const {
        return eraseCount < o.eraseCount;
    }
};

class Allocator {
public:
    BlockDevice* device;
    InlineArray<BlockMeta> bam;
    InlineArray<AllocHeapEntry> freeHeap;
    u32 bamStartBlock = 2;
    u32 bamBlockCount = 16;

    Allocator(BlockDevice* dev);

    void initFromFormat(u32 blockCount);
    void buildHeap();

    // Allocates a block, automatically wear-leveling if necessary.
    // Returns 0 on failure (no space). Block 0 is typically reserved for Superblock.
    u32 allocBlock(BlockType type);

    // Frees a block, erases it, and puts it back in the heap
    bool freeBlock(u32 blockIdx);

    // Reserved for full wear leveling moving hot data
    void wearLevel();

    // Persist BAM to device (call on flush/unmount)
    void saveBam();
    // Load BAM from device (call on mount). Returns true if valid BAM was found.
    bool loadBam();
    
private:
    void pushHeap(AllocHeapEntry entry);
    AllocHeapEntry popHeap();
};

} // namespace Xylem

#endif // XYLEM_ALLOCATOR_HPP
