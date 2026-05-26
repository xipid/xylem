#include <Xylem/Allocator.hpp>
#include <algorithm>

namespace Xylem {

Allocator::Allocator(BlockDevice* dev) : device(dev) {}

void Allocator::initFromFormat(u32 blockCount) {
    bam.allocate(blockCount);
    for (u32 i = 0; i < blockCount; ++i) {
        bam[i].eraseCount = 0;
        bam[i].setStatus(BlockStatus::FREE);
        bam[i].setType(BlockType::FREE);
    }
    buildHeap();
}

void Allocator::buildHeap() {
    freeHeap = InlineArray<AllocHeapEntry>();
    for (u32 i = 0; i < bam.size(); ++i) {
        if (bam[i].getStatus() == BlockStatus::FREE) {
            pushHeap({bam[i].eraseCount, i});
        }
    }
}

// BAM block format:
//   [4B] count of entries in this block
//   [count * 3B] entries (u16 eraseCount LE + u8 packed)
//   [4B] CRC32 of all preceding bytes
void Allocator::saveBam() {
    if (!device || !device->config.onDeviceWrite) return;

    u32 blockSize = device->config.blockSize;
    // Overhead: 4B count + 4B CRC = 8B
    u32 entriesPerBlock = (blockSize > 8) ? (blockSize - 8) / 3 : 1;
    usz totalEntries = bam.size();

    for (u32 b = 0; b < bamBlockCount; ++b) {
        usz start = (usz)b * entriesPerBlock;
        if (start >= totalEntries) break;

        usz end = start + entriesPerBlock;
        if (end > totalEntries) end = totalEntries;
        u32 count = (u32)(end - start);

        String data; data.allocate(blockSize);
        data.fill(0xFF);
        u8* ptr = (u8*)data.data();
        u8* blockStart = ptr;

        *(u32*)ptr = count; ptr += 4;
        for (usz i = start; i < end; ++i) {
            *(u16*)ptr = bam[i].eraseCount; ptr += 2;
            *ptr++ = bam[i].packed;
        }

        u32 payloadLen = (u32)(ptr - blockStart);
        u32 c = crc32(blockStart, payloadLen);
        *(u32*)ptr = c; ptr += 4;

        u32 blockDataLen = (u32)(ptr - blockStart);
        device->writeBlock(bamStartBlock + b, 0, data.slice(0, blockDataLen));
    }
}

bool Allocator::loadBam() {
    if (!device || !device->config.onDeviceRead) return false;

    u32 blockSize = device->config.blockSize;
    u32 entriesPerBlock = (blockSize > 8) ? (blockSize - 8) / 3 : 1;
    usz totalEntries = bam.size();
    usz entryIdx = 0;

    for (u32 b = 0; b < bamBlockCount && entryIdx < totalEntries; ++b) {
        String data = device->readBlock(bamStartBlock + b, 0);
        if ((u32)data.size() < 8) { return false; }

        const u8* blockStart = (const u8*)data.data();
        const u8* ptr = blockStart;
        u32 count = *(u32*)ptr; ptr += 4;
        if (count == 0 || count > entriesPerBlock) { return false; }

        u32 payloadLen = 4 + count * 3;
        if (payloadLen + 4 > (u32)data.size()) { return false; }

        u32 storedCrc   = *(u32*)(blockStart + payloadLen);
        u32 computedCrc = crc32(blockStart, payloadLen);
        if (storedCrc != computedCrc) { return false; }

        for (u32 i = 0; i < count && entryIdx < totalEntries; ++i, ++entryIdx) {
            bam[entryIdx].eraseCount = *(u16*)ptr; ptr += 2;
            bam[entryIdx].packed     = *ptr++;
        }
    }

    return entryIdx > 0;
}

void Allocator::pushHeap(AllocHeapEntry entry) {
    freeHeap.push(entry);
    usz idx = freeHeap.size() - 1;
    while (idx > 0) {
        usz p = (idx - 1) / 2;
        if (freeHeap[idx] < freeHeap[p]) {
            auto tmp = freeHeap[idx];
            freeHeap[idx] = freeHeap[p];
            freeHeap[p] = tmp;
            idx = p;
        } else {
            break;
        }
    }
}

AllocHeapEntry Allocator::popHeap() {
    if (freeHeap.size() == 0) return {0, 0};
    
    AllocHeapEntry minEntry = freeHeap[0];
    freeHeap[0] = freeHeap[freeHeap.size() - 1];
    freeHeap.pop();

    usz idx = 0;
    while (true) {
        usz left  = 2 * idx + 1;
        usz right = 2 * idx + 2;
        usz smallest = idx;

        if (left  < freeHeap.size() && freeHeap[left]  < freeHeap[smallest]) smallest = left;
        if (right < freeHeap.size() && freeHeap[right] < freeHeap[smallest]) smallest = right;
        
        if (smallest != idx) {
            auto tmp = freeHeap[idx];
            freeHeap[idx] = freeHeap[smallest];
            freeHeap[smallest] = tmp;
            idx = smallest;
        } else {
            break;
        }
    }
    return minEntry;
}

u32 Allocator::allocBlock(BlockType type) {
    while (true) {
        if (freeHeap.size() == 0) return 0;
        
        AllocHeapEntry entry = popHeap();
        
        if (bam[entry.blockIdx].getStatus() != BlockStatus::FREE) {
            continue;
        }
        
        if (device->config.blockCycles > 0 && entry.eraseCount >= device->config.blockCycles) {
            bam[entry.blockIdx].setStatus(BlockStatus::BAD);
            continue;
        }
        
        bam[entry.blockIdx].setStatus(BlockStatus::USED);
        bam[entry.blockIdx].setType(type);
        return entry.blockIdx;
    }
}

bool Allocator::freeBlock(u32 blockIdx) {
    if (blockIdx >= bam.size()) return false;
    // Allow freeing USED or RESERVED (for cleanup during rollback)
    BlockStatus st = bam[blockIdx].getStatus();
    if (st != BlockStatus::USED && st != BlockStatus::RESERVED) return false;

    if (!device->eraseBlock(blockIdx)) {
        bam[blockIdx].setStatus(BlockStatus::BAD);
        return false;
    }
    
    if (device->config.blockCycles > 0 || device->config.blockErase) {
        bam[blockIdx].eraseCount++;
    }

    bam[blockIdx].setStatus(BlockStatus::FREE);
    bam[blockIdx].setType(BlockType::FREE);
    pushHeap({bam[blockIdx].eraseCount, blockIdx});
    return true;
}

void Allocator::wearLevel() {
    // Min-heap already provides passive wear leveling.
    // Active relocation of cold data is deferred to a future GC pass.
}

} // namespace Xylem
