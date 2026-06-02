#include <Xylem/XBDiff.hpp>
#include <Xylem/BlobStore.hpp>

using namespace Collection;

namespace Xylem {

XBDiff::XBDiff() : blobStore(nullptr) {}

XBDiff::XBDiff(const String& baseVal) : baseContent(baseVal), blobStore(nullptr) {
    if (!baseVal.isEmpty()) {
        Segment seg;
        seg.type = Segment::COPY;
        seg.length = baseVal.size();
        seg.sourceOffset = 0;
        segments.push(seg);
    }
}

XBDiff::ByteRef::operator u8() const {
    return diff.getByte(index);
}

XBDiff::ByteRef& XBDiff::ByteRef::operator=(u8 val) {
    diff.setByte(index, val);
    return *this;
}

XBDiff::ByteRef XBDiff::operator[](usz index) {
    return ByteRef{*this, index};
}

u8 XBDiff::operator[](usz index) const {
    return getByte(index);
}

u8 XBDiff::getByte(usz index) const {
    usz currentPos = 0;
    for (usz i = 0; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        if (index >= currentPos && index < currentPos + seg.length) {
            usz offset = index - currentPos;
            if (seg.type == Segment::COPY) {
                if (seg.sourceOffset + offset < baseContent.size()) {
                    return (u8)baseContent[seg.sourceOffset + offset];
                }
                return 0;
            } else if (seg.type == Segment::LITERAL) {
                if (offset < seg.literalData.size()) {
                    return (u8)seg.literalData[offset];
                }
                return 0;
            } else if (seg.type == Segment::HASH) {
                if (blobStore) {
                    String hashContent = blobStore->readHash(seg.hash, 0, 0xFFFFFFFF);
                    if (seg.hashOffset + offset < hashContent.size()) {
                        return (u8)hashContent[seg.hashOffset + offset];
                    }
                }
                return 0;
            }
        }
        currentPos += seg.length;
    }
    return 0;
}

void XBDiff::setByte(usz index, u8 val) {
    char c = (char)val;
    splice(index, 1, String::from((const u8*)&c, 1));
}

usz XBDiff::size() const {
    usz total = 0;
    for (usz i = 0; i < segments.size(); ++i) {
        total += segments[i].length;
    }
    return total;
}

void XBDiff::insertHash(usz index, const String& hash) {
    usz len = 0;
    if (blobStore) {
        u32 ref = blobStore->getBlobRef(hash);
        if (ref != 0) {
            len = blobStore->getBlobSize(ref);
        } else {
            auto* meta = blobStore->index.get(hash);
            if (meta) len = meta->originalSize;
        }
    }
    
    Array<Segment> newSegments;
    usz currentPos = 0;
    usz i = 0;
    
    for (; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        if (currentPos + seg.length <= index) {
            newSegments.push(seg);
            currentPos += seg.length;
        } else {
            if (index > currentPos) {
                usz leftLen = index - currentPos;
                Segment leftSeg = seg;
                leftSeg.length = leftLen;
                if (seg.type == Segment::LITERAL) {
                    leftSeg.literalData = seg.literalData.slice(0, leftLen);
                }
                newSegments.push(leftSeg);
            }
            break;
        }
    }
    
    Segment hashSeg;
    hashSeg.type = Segment::HASH;
    hashSeg.length = len;
    hashSeg.hash = hash;
    hashSeg.hashOffset = 0;
    newSegments.push(hashSeg);
    
    if (i < segments.size()) {
        const Segment& seg = segments[i];
        usz offsetInSeg = index - currentPos;
        usz rightLen = seg.length - offsetInSeg;
        Segment rightSeg = seg;
        rightSeg.length = rightLen;
        if (seg.type == Segment::COPY) {
            rightSeg.sourceOffset += offsetInSeg;
        } else if (seg.type == Segment::LITERAL) {
            rightSeg.literalData = seg.literalData.slice(offsetInSeg);
        } else if (seg.type == Segment::HASH) {
            rightSeg.hashOffset += offsetInSeg;
        }
        newSegments.push(rightSeg);
        i++;
    }
    
    for (; i < segments.size(); ++i) {
        newSegments.push(segments[i]);
    }
    
    segments = newSegments;
    hashesInserted.push(hash);
}

void XBDiff::set(usz position, const String& array) {
    splice(position, array.size(), array);
}

void XBDiff::splice(usz start, usz deleteCount, const String& insertData) {
    Array<Segment> newSegments;
    usz currentPos = 0;
    usz i = 0;
    
    for (; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        if (currentPos + seg.length <= start) {
            newSegments.push(seg);
            currentPos += seg.length;
        } else {
            if (start > currentPos) {
                usz leftLen = start - currentPos;
                Segment leftSeg = seg;
                leftSeg.length = leftLen;
                if (seg.type == Segment::LITERAL) {
                    leftSeg.literalData = seg.literalData.slice(0, leftLen);
                }
                newSegments.push(leftSeg);
            }
            break;
        }
    }
    
    if (insertData.size() > 0) {
        Segment newSeg;
        newSeg.type = Segment::LITERAL;
        newSeg.length = insertData.size();
        newSeg.literalData = insertData;
        newSegments.push(newSeg);
    }
    
    usz remainingDelete = deleteCount;
    for (; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        usz segEnd = currentPos + seg.length;
        
        if (segEnd <= start + deleteCount) {
            remainingDelete -= seg.length;
            currentPos += seg.length;
        } else {
            usz offsetInSeg = (start + deleteCount) - currentPos;
            usz rightLen = seg.length - offsetInSeg;
            
            Segment rightSeg = seg;
            rightSeg.length = rightLen;
            if (seg.type == Segment::COPY) {
                rightSeg.sourceOffset += offsetInSeg;
            } else if (seg.type == Segment::LITERAL) {
                rightSeg.literalData = seg.literalData.slice(offsetInSeg);
            } else if (seg.type == Segment::HASH) {
                rightSeg.hashOffset += offsetInSeg;
            }
            newSegments.push(rightSeg);
            currentPos += seg.length;
            i++;
            break;
        }
    }
    
    for (; i < segments.size(); ++i) {
        newSegments.push(segments[i]);
    }
    
    segments = newSegments;
}

String XBDiff::toBinary() const {
    String stream;
    auto writeVarInt = [](String& str, u64 val) {
        while (val >= 0x80) {
            str += (char)((val & 0x7F) | 0x80);
            val >>= 7;
        }
        str += (char)(val & 0x7F);
    };
    
    writeVarInt(stream, segments.size());
    for (usz i = 0; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        stream += (char)seg.type;
        writeVarInt(stream, seg.length);
        if (seg.type == Segment::COPY) {
            writeVarInt(stream, seg.sourceOffset);
        } else if (seg.type == Segment::LITERAL) {
            stream += seg.literalData;
        } else if (seg.type == Segment::HASH) {
            writeVarInt(stream, seg.hash.size());
            stream += seg.hash;
            writeVarInt(stream, seg.hashOffset);
        }
    }
    return stream;
}

String XBDiff::toString() const {
    String s;
    s += "Xylem Blob Diff (xbdiff) version 1.0\n";
    s += "Total size: " + String::from((u64)size()) + " bytes\n";
    s += "Segments count: " + String::from((u64)segments.size()) + "\n";
    for (usz i = 0; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        s += "  [" + String::from((u64)i) + "] ";
        if (seg.type == Segment::COPY) {
            s += "COPY: offset " + String::from((u64)seg.sourceOffset) + ", len " + String::from((u64)seg.length) + "\n";
        } else if (seg.type == Segment::LITERAL) {
            s += "LITERAL: len " + String::from((u64)seg.length) + "\n";
            s += "    data: \"";
            for (usz j = 0; j < seg.literalData.size() && j < 32; ++j) {
                char c = seg.literalData[j];
                if (c >= 32 && c <= 126) s += c;
                else s += ".";
            }
            if (seg.literalData.size() > 32) s += "...";
            s += "\"\n";
        } else if (seg.type == Segment::HASH) {
            s += "HASH: " + seg.hash + ", offset " + String::from((u64)seg.hashOffset) + ", len " + String::from((u64)seg.length) + "\n";
        }
    }
    return s;
}

String XBDiff::toBinaryContent(BlobStore* bs) const {
    String result;
    result.allocate(size());
    usz written = 0;
    for (usz i = 0; i < segments.size(); ++i) {
        const Segment& seg = segments[i];
        if (seg.type == Segment::COPY) {
            for (usz j = 0; j < seg.length; ++j) {
                if (seg.sourceOffset + j < baseContent.size()) {
                    result[written++] = baseContent[seg.sourceOffset + j];
                } else {
                    result[written++] = 0;
                }
            }
        } else if (seg.type == Segment::LITERAL) {
            for (usz j = 0; j < seg.length; ++j) {
                result[written++] = seg.literalData[j];
            }
        } else if (seg.type == Segment::HASH) {
            String hashContent;
            BlobStore* actualBs = bs ? bs : blobStore;
            if (actualBs) {
                hashContent = actualBs->readHash(seg.hash, 0, 0xFFFFFFFF);
            }
            for (usz j = 0; j < seg.length; ++j) {
                if (seg.hashOffset + j < hashContent.size()) {
                    result[written++] = hashContent[seg.hashOffset + j];
                } else {
                    result[written++] = 0;
                }
            }
        }
    }
    return result.slice(0, written);
}

XBDiff XBDiff::create(const String& oldData, const String& newData) {
    XBDiff df;
    df.baseContent = oldData;
    
    if (newData.isEmpty()) return df;
    if (oldData.isEmpty()) {
        Segment seg;
        seg.type = Segment::LITERAL;
        seg.length = newData.size();
        seg.literalData = newData;
        df.segments.push(seg);
        return df;
    }
    
    Map<u32, Array<usz>> posMap;
    if (oldData.size() >= 4) {
        for (usz i = 0; i <= oldData.size() - 4; ++i) {
            u32 val = *(const u32*)(oldData.data() + i);
            if (!posMap.has(val)) {
                posMap.set(val, Array<usz>());
            }
            posMap.get(val)->push(i);
        }
    }
    
    usz j = 0;
    usz literalStart = 0;
    
    auto flushLiteral = [&](usz endIdx) {
        if (endIdx > literalStart) {
            Segment seg;
            seg.type = Segment::LITERAL;
            seg.length = endIdx - literalStart;
            seg.literalData = newData.slice(literalStart, (long long)endIdx);
            df.segments.push(seg);
        }
    };
    
    while (j < newData.size()) {
        bool matchFound = false;
        usz bestMatchOffset = 0;
        usz bestMatchLen = 0;
        
        if (j + 4 <= newData.size()) {
            u32 val = *(const u32*)(newData.data() + j);
            auto* matchPositions = posMap.get(val);
            if (matchPositions) {
                for (usz k = 0; k < matchPositions->size(); ++k) {
                    usz oldPos = (*matchPositions)[k];
                    usz len = 4;
                    while (oldPos + len < oldData.size() && j + len < newData.size() &&
                           oldData[oldPos + len] == newData[j + len]) {
                        len++;
                    }
                    if (len > bestMatchLen) {
                        bestMatchLen = len;
                        bestMatchOffset = oldPos;
                        matchFound = true;
                    }
                }
            }
        }
        
        if (matchFound && bestMatchLen >= 8) {
            flushLiteral(j);
            
            Segment seg;
            seg.type = Segment::COPY;
            seg.length = bestMatchLen;
            seg.sourceOffset = bestMatchOffset;
            df.segments.push(seg);
            
            j += bestMatchLen;
            literalStart = j;
        } else {
            j++;
        }
    }
    
    flushLiteral(j);
    return df;
}

XBDiff XBDiff::fromBinary(const String& bin, const String& baseVal) {
    XBDiff df;
    df.baseContent = baseVal;
    if (bin.isEmpty()) return df;
    
    const u8* ptr = (const u8*)bin.data();
    const u8* end = ptr + bin.size();
    
    auto readVarInt = [](const u8*& p, const u8* e) -> u64 {
        u64 val = 0; int shift = 0;
        while (p < e) {
            u8 b = *p++;
            val |= (u64)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift > 63) break;
        }
        return val;
    };
    
    u64 numSegs = readVarInt(ptr, end);
    for (u64 i = 0; i < numSegs && ptr < end; ++i) {
        Segment seg;
        seg.type = (Segment::Type)*ptr++;
        seg.length = (usz)readVarInt(ptr, end);
        if (seg.type == Segment::COPY) {
            seg.sourceOffset = (usz)readVarInt(ptr, end);
        } else if (seg.type == Segment::LITERAL) {
            if (ptr + seg.length <= end) {
                seg.literalData = String(ptr, seg.length);
                ptr += seg.length;
            }
        } else if (seg.type == Segment::HASH) {
            u64 hashLen = readVarInt(ptr, end);
            if (ptr + hashLen <= end) {
                seg.hash = String(ptr, hashLen);
                ptr += hashLen;
                df.hashesInserted.push(seg.hash);
            }
            seg.hashOffset = (usz)readVarInt(ptr, end);
        }
        df.segments.push(seg);
    }
    return df;
}

} // namespace Xylem
