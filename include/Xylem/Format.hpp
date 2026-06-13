#ifndef XYLEM_FORMAT_HPP
#define XYLEM_FORMAT_HPP

#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <Xi/Primitives.hpp>

namespace Xylem {

using namespace Xi;
using namespace Collection;

enum class TypeTag : u8 {
    BOOL = 0x00,
    U8 = 0x01,
    I8 = 0x02,
    U16 = 0x03,
    I16 = 0x04,
    U32 = 0x05,
    I32 = 0x06,
    U64 = 0x07,
    I64 = 0x08,
    F32 = 0x09,
    F64 = 0x0A,
    TIME = 0x0B,
    STRING = 0x0C,
    STRING_N = 0x0D,
    BLOB = 0x0E,
    TREE = 0x0F,
    WATCH = 0x10,
    TENSOR = 0x11,
    UNKNOWN = 0x12,
    UNKNOWN_N = 0x13,
    COUNT = 0x14
};

enum class EncodingTag : u8 {
    RAW = 0x00,
    DELTA = 0x01,
    BITPACK = 0x02,
    VARLEN = 0x03
};

enum class BlockType : u8 {
    FREE = 0,
    SUPER = 1,
    BAM = 2,
    JOURNAL = 3,
    SCHEMA = 4,
    TABLE = 5,
    BLOB = 6,
    DICT = 7,
    RAW = 8
};

enum class BlockStatus : u8 {
    FREE = 0,
    USED = 1,
    BAD = 2,
    RESERVED = 3
};

constexpr u8 FLAG_HAS_CRC = 1 << 0;
constexpr u8 FLAG_ZSTD_COMPRESSED = 1 << 1;

constexpr u32 MAGIC_XYLM = 0x4D4C5958; // "XYLM" (LE)

// ─── Blob reference marker ──────────────────────────────────────────────────
// Stored in column values to indicate a blob reference:
//   [0x00, 'B', 'L', 'B'] + 16-byte BLAKE2b-128 hash = 20 bytes total
static constexpr u8  BLOB_MARKER[] = {0x00, 'B', 'L', 'B'};
static constexpr u32 BLOB_MARKER_LEN = 4;
static constexpr u32 BLOB_HASH_LEN = 16;
static constexpr u32 BLOB_REF_SIZE = BLOB_MARKER_LEN + BLOB_HASH_LEN; // 20

inline String makeBlobRef(const String& hash) {
    String ref;
    ref.allocate(BLOB_REF_SIZE);
    for (u32 i = 0; i < BLOB_MARKER_LEN; ++i) ref[i] = BLOB_MARKER[i];
    for (u32 i = 0; i < BLOB_HASH_LEN && i < (u32)hash.size(); ++i) ref[BLOB_MARKER_LEN + i] = hash[i];
    return ref;
}

inline bool isBlobRef(const String& val) {
    if ((u32)val.size() != BLOB_REF_SIZE) return false;
    for (u32 i = 0; i < BLOB_MARKER_LEN; ++i) {
        if ((u8)val[i] != BLOB_MARKER[i]) return false;
    }
    return true;
}

inline String extractBlobHash(const String& val) {
    if (!isBlobRef(val)) return String();
    return val.slice(BLOB_MARKER_LEN);
}

// ─── Lock reference marker ──────────────────────────────────────────────────
// Stored in column values to indicate a lock/commit reference:
//   [0x00, 'L', 'C', 'K'] + 8-byte LE u64 lock/commit ID = 12 bytes total
static constexpr u8  LOCK_MARKER[] = {0x00, 'L', 'C', 'K'};
static constexpr u32 LOCK_MARKER_LEN = 4;
static constexpr u32 LOCK_REF_SIZE = LOCK_MARKER_LEN + 8; // 12

inline String makeLockRef(u64 lockId) {
    String ref;
    ref.allocate(LOCK_REF_SIZE);
    for (u32 i = 0; i < LOCK_MARKER_LEN; ++i) ref[i] = LOCK_MARKER[i];
    *(u64*)(ref.data() + LOCK_MARKER_LEN) = lockId;
    return ref;
}

inline bool isLockRef(const String& val) {
    if ((u32)val.size() != LOCK_REF_SIZE) return false;
    for (u32 i = 0; i < LOCK_MARKER_LEN; ++i) {
        if ((u8)val[i] != LOCK_MARKER[i]) return false;
    }
    return true;
}

inline u64 extractLockId(const String& val) {
    if (!isLockRef(val)) return 0;
    return *(const u64*)(val.data() + LOCK_MARKER_LEN);
}


// Simple unoptimized CRC32 for metadata blocks
inline u32 crc32(const u8* data, usz len) {
    u32 crc = 0xFFFFFFFF;
    for (usz i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

inline u32 crc32(const String& str) {
    return crc32((const u8*)str.data(), str.size());
}

} // namespace Xylem

#endif // XYLEM_FORMAT_HPP
