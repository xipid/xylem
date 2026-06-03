#ifndef XYLEM_BLOBSTORE_HPP
#define XYLEM_BLOBSTORE_HPP

#include <Collection/Map.hpp>
#include <Resource/Compression.hpp>
#include <Xi/Func.hpp>
#include <Xylem/Allocator.hpp>
#include <Xylem/BlockDevice.hpp>
#include <Xylem/Format.hpp>

namespace Xylem {

using namespace ::Xi;
using namespace ::Collection;

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
  u32 blockIdx; // First block of blob chain (0 = invalid / in-memory only)
  u32 offset;
  u32 length;          // Total payload length (after compression / encryption)
  u32 originalSize;    // Original unencrypted, uncompressed size
  u64 fixedPosition;   // Byte address if pinned (0 = not fixed)
  u32 ref;             // Reference handle (1-based, 0 = unassigned)
  bool frozen;         // Whether this blob is frozen at fixedPosition
  bool used;           // Whether this blob is actively referenced by items
  bool isDiff = false; // Whether this blob is stored as a diff
  String baseHash;     // The base hash this diff is relative to
  u64 seq = 0;         // Sequence when this blob became a diff
};

struct PendingFreeze {
  u64 position; // Byte address to freeze at
  u32 ref;      // Blob ref to freeze
};

class BlobStore {
public:
  friend class TableStore;
  BlockDevice *device;
  Allocator *allocator;

  // Key → blob location index
  Map<String, BlobMeta> index;

  // Ref-based mapping
  Map<u32, String> refToHash; // ref → hash
  Map<String, u32> hashToRef; // hash → ref
  u32 nextRef = 1;

  // Pending freezes (for blobs not yet written) — persisted to block device
  Array<PendingFreeze> pendingFreezes;

#ifdef XI_ZSTD_ENABLED
  Resource::ZSTD zstd;
#endif

  Array<String> *globalKeys;

  Xi::Func<void(u64, u64)> thawCallback;

  // Bloom filter: 3 hashes derived from BLAKE2b-128 (bytes 0-7, 8-15, XOR)
  // Size: 10 bits per entry, false-positive rate < 1%
  static constexpr u32 BLOOM_SIZE_BITS = 65536; // 8KB
  static constexpr u32 BLOOM_SIZE_BYTES = BLOOM_SIZE_BITS / 8;
  u8 bloom[BLOOM_SIZE_BYTES] = {};

  void bloomInsert(const String &hash);
  bool bloomMayExist(const String &hash) const;
  void bloomRebuild();

  void trainDictionary(u32 maxSamples = 100);

  BlobStore(BlockDevice *dev, Allocator *alloc, Array<String> *keys = nullptr);

  // ─── Ref-based public API ────────────────────────────────────────────────

  // Get ref for a hash. Returns 0 if not found.
  u32 getBlobRef(const String &hash);

  // Read full blob content by ref
  String getBlob(u32 ref);

  // Get blob original size by ref
  u32 getBlobSize(u32 ref);

  // Write content to a blob by ref (partial write with start offset)
  void writeBlob(u32 ref, const String &content, u64 start = 0);

  // Read blob content by ref (partial read, end=0 means to end)
  String readBlob(u32 ref, u64 start = 0, u64 end = 0);

  // Set the hash of a blob ref externally (user-provided hash)
  void setBlob(u32 ref, const String &hash);

  // Auto-hash a blob's content, write the hash. Returns the computed hash.
  String setBlob(u32 ref);

  // Freeze a blob at a byte position. Works even if blob not yet written
  // (deferred).
  bool freezeBlob(u64 position, u32 blobRef);

  // Thaw a blob — allow Xylem to relocate it. Does NOT remove the blob.
  void thawBlob(u32 blobRef);

  // Set blob usage explicitly
  void setBlobUsed(u32 ref, bool used);

  // Allocate a ref for a hash (internal)
  u32 allocRef(const String &hash);

  // Resolve any pending freezes (called after writeBlob)
  void resolvePendingFreezes();

  // Persist pending freezes to block device (power-loss safe)
  void savePendingFreezes();

  // Load pending freezes from block device on mount
  void loadPendingFreezes();

  // Persist refs mapping and usage status
  void saveRefsAndUsage();

  // Load refs mapping and usage status on mount
  void loadRefsAndUsage();

  // ─── Internal hash-based API (used by engine internals) ──────────────────

  // Reads content; respects [minOffset, maxOffset) range (0,0 = all)
  String readHash(const String &hash, u64 minOffset = 0, u64 maxOffset = 0,
                  Map<String, String> *cache = nullptr);

  // Stores content keyed by hash; no-op if hash already present (content dedup)
  bool writeHash(const String &hash, u64 minOffset, const String &data,
                 const String &encryptionKey = "", const String &oldHash = "",
                 u64 seq = 0);

  // Stores diff data keyed by hash, referencing baseHash
  bool writeDiffHash(const String &hash, const String &baseHash,
                     const String &diffBin, const String &encryptionKey = "");
  bool writeHashInternal(const String &hash, u64 minOffset, const String &data,
                         const String &encryptionKey = "");

  // Moves a blob to a fixed byte address (relocating anything in the way)
  bool fixBlob(const String &hash, u64 byteAddress);

  // Removes the blob and frees its blocks
  bool removeHash(const String &hash);

  // On mount: scan BAM for BLOB blocks and rebuild index from first-block
  // headers
  void scanFromDevice();

  // Check if placing a blob of `size` bytes at `position` would overlap any
  // fixed blob
  bool wouldOverlap(u64 position, u32 size) const;

  // Check if a hash is fixed at a position
  bool isFixed(const String &hash) const;

private:
  // Read raw payload chain starting at blockIdx, returns assembled bytes
  String readChain(u32 blockIdx) const;
};

} // namespace Xylem

#endif // XYLEM_BLOBSTORE_HPP
