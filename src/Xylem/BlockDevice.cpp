#include <Xylem/BlockDevice.hpp>

namespace Xylem {

u64 BlockDevice::makeNonce(u32 blockIdx, u16 eraseCount) const {
    // Unique nonce per block write.
    // Because Xylem is COW, a block is written exactly once per erase cycle.
    // Thus blockIdx + eraseCount is perfectly unique over the lifetime of the DB.
    return ((u64)blockIdx << 16) | (u64)eraseCount;
}

bool BlockDevice::eraseBlock(u32 blockIdx) {
    if (!config.blockErase || !config.onDeviceErase) {
        return true; // In archive mode or if no erase provided, assume success/no-op
    }
    u64 offset = (u64)blockIdx * config.blockSize;
    return config.onDeviceErase(offset, offset + config.blockSize);
}

bool BlockDevice::writeBlock(u32 blockIdx, u16 eraseCount, const String& data) {
    if (!config.onDeviceWrite) return false;
    
    u64 offset = (u64)blockIdx * config.blockSize;
    String finalData = data;



    // Write in chunks of writeSize
    for (u32 chunkOff = 0; chunkOff < finalData.size(); chunkOff += config.writeSize) {
        u32 chunkLen = (chunkOff + config.writeSize > finalData.size()) 
            ? (finalData.size() - chunkOff) 
            : config.writeSize;
            
        if (!config.onDeviceWrite(offset + chunkOff, finalData.slice(chunkOff, chunkOff + chunkLen))) {
            return false;
        }
    }
    return true;
}

String BlockDevice::readBlock(u32 blockIdx, u16 eraseCount) {
    if (!config.onDeviceRead) return String();

    u64 offset = (u64)blockIdx * config.blockSize;
    String rawData;
    rawData.allocate(config.blockSize);

    // Read in chunks of readSize
    for (u32 chunkOff = 0; chunkOff < config.blockSize; chunkOff += config.readSize) {
        u32 chunkLen = (chunkOff + config.readSize > config.blockSize) 
            ? (config.blockSize - chunkOff) 
            : config.readSize;
            
        String chunk = config.onDeviceRead(offset + chunkOff, offset + chunkOff + chunkLen);
        if (chunk.size() != chunkLen) {
            return String(); // Read failure
        }
        for (usz i = 0; i < chunk.size(); ++i) {
            rawData[chunkOff + i] = chunk[i];
        }
    }



    return rawData;
}

} // namespace Xylem
