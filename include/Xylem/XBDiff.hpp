#ifndef XYLEM_XBDIFF_HPP
#define XYLEM_XBDIFF_HPP

#include <Collection/Array.hpp>
#include <Collection/String.hpp>

namespace Xylem {

class BlobStore;

class XBDiff {
public:
    struct Segment {
        enum Type {
            COPY = 1,
            LITERAL = 2,
            HASH = 3
        };
        Type type;
        usz length;
        
        // For COPY
        usz sourceOffset = 0;
        
        // For LITERAL
        Collection::String literalData;
        
        // For HASH
        Collection::String hash;
        usz hashOffset = 0;
    };

    Collection::String baseContent;
    Collection::Array<Segment> segments;
    Collection::Array<Collection::String> hashesInserted;
    BlobStore* blobStore = nullptr;

    XBDiff();
    explicit XBDiff(const Collection::String& baseVal);

    // Proxy for array-like writing: df[index] = byte
    struct ByteRef {
        XBDiff& diff;
        usz index;
        operator u8() const;
        ByteRef& operator=(u8 val);
    };

    ByteRef operator[](usz index);
    u8 operator[](usz index) const;

    u8 getByte(usz index) const;
    void setByte(usz index, u8 val);

    usz size() const;

    void insertHash(usz index, const Collection::String& hash);
    void set(usz position, const Collection::String& array);
    void splice(usz start, usz deleteCount, const Collection::String& insertData = Collection::String());

    Collection::String toBinary() const;
    Collection::String toString() const;
    Collection::String toBinaryContent(BlobStore* bs = nullptr) const;

    static XBDiff create(const Collection::String& oldData, const Collection::String& newData);
    static XBDiff fromBinary(const Collection::String& bin, const Collection::String& baseVal = Collection::String());
};

} // namespace Xylem

#endif // XYLEM_XBDIFF_HPP
