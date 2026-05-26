#ifndef XYLEM_BLOBSTORE_HPP
#define XYLEM_BLOBSTORE_HPP

#include <Xylem/Format.hpp>
#include <Xylem/BlockDevice.hpp>
#include <Xylem/Allocator.hpp>
#include <LLT/Compression.hpp>
#include <Collection/Map.hpp>

namespace Xylem {

// Blob block format (first block of a chain):
//   [1B] blockType = BLOB (6)
//   [1B] isFirst   = 1
//   [1B] keyLen    (number of bytes of key stored immediately after)
//   [keyLen B] key
//   [4B] totalDataLen (u32 LE, total payload across all chained blocks)
//   [4B] thisChunkLen (u32 LE, payload bytes in THIS block)
//   [4B] nextBlock    (u32 LE, 0 = no next block)
//   [thisChunkLen B] payload
//   [4B] CRC32 of all preceding bytes
//
// Continuation block:
//   [1B] blockType = BLOB (6)
//   [1B] isFirst   = 0
//   [1B] keyLen    = 0
//   [4B] totalDataLen = 0
//   [4B] thisChunkLen
//   [4B] nextBlock
//   [thisChunkLen B] payload
//   [4B] CRC32

struct BlobMeta {
    u32 blockIdx;       // First block of blob chain (0 = invalid / in-memory only)
    u32 offset;
    u32 length;         // Total payload length (after compression / encryption)
    u32 originalSize;   // Original unencrypted, uncompressed size
    u64 fixedPosition;  // Byte address if pinned (0 = not fixed)
};

class BlobStore {
public:
    BlockDevice* device;
    Allocator*   allocator;

    // Key → blob location index
    Map<String, BlobMeta> index;

#ifdef XI_ZSTD_ENABLED
    LLT::ZSTD zstd;
#endif

    Array<String>* globalKeys;

    // Bloom filter: 3 hashes derived from BLAKE2b-128 (bytes 0-7, 8-15, XOR)
    // Size: 10 bits per entry, false-positive rate < 1%
    static constexpr u32 BLOOM_SIZE_BITS = 65536; // 8KB
    static constexpr u32 BLOOM_SIZE_BYTES = BLOOM_SIZE_BITS / 8;
    u8 bloom[BLOOM_SIZE_BYTES] = {};

    void bloomInsert(const String& hash);
    bool bloomMayExist(const String& hash) const;
    void bloomRebuild();

    void trainDictionary(u32 maxSamples = 100);

    BlobStore(BlockDevice* dev, Allocator* alloc, Array<String>* keys = nullptr);

    // Returns original size as string if exists, empty otherwise
    String statHash(const String& hash);

    // Reads content; respects [minOffset, maxOffset) range (0,0 = all)
    String readHash(const String& hash, u64 minOffset = 0, u64 maxOffset = 0);

    // Stores content keyed by hash; no-op if hash already present (content dedup)
    bool writeHash(const String& hash, u64 minOffset, const String& data,
                   const String& encryptionKey = "");

    // Moves a blob to a fixed byte address (relocating anything in the way)
    bool fixBlob(const String& hash, u64 byteAddress);

    // Removes the blob and frees its blocks
    bool removeHash(const String& hash);

    // On mount: scan BAM for BLOB blocks and rebuild index from first-block headers
    void scanFromDevice();

    // Check if placing a blob of `size` bytes at `position` would overlap any fixed blob
    bool wouldOverlap(u64 position, u32 size) const;

    // Check if a hash is fixed at a position
    bool isFixed(const String& hash) const;

private:
    // Read raw payload chain starting at blockIdx, returns assembled bytes
    String readChain(u32 blockIdx) const;
};

} // namespace Xylem

#endif // XYLEM_BLOBSTORE_HPP
