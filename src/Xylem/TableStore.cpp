#include <Xylem/TableStore.hpp>
#include <Xylem/BlobStore.hpp>
#include <Xylem/CryptItem.hpp>
#include <Security/Crypto.hpp>
#include <algorithm>
#include <cmath>
#include <regex>
#include <cstdlib>


namespace Xylem {

static bool isRegexSyntax(const String& pattern) {
    for (usz i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '\\') {
            if (i + 1 < pattern.size()) {
                char next = pattern[i+1];
                if (next == 'd' || next == 'D' || next == 'w' || next == 'W' || next == 's' || next == 'S' || next == 'b' || next == 'B') {
                    return true;
                }
                i++;
            }
        } else if (c == '[' || c == '(' || c == '^' || c == '$' || c == '|' || c == '+') {
            return true;
        } else if (c == '.') {
            if (i + 1 < pattern.size() && (pattern[i+1] == '*' || pattern[i+1] == '+')) {
                return true;
            }
        }
    }
    return false;
}

static String globToRegex(const String& glob) {
    String re;
    for (usz i = 0; i < glob.size(); ++i) {
        char c = glob[i];
        if (c == '\\') {
            if (i + 1 < glob.size()) {
                char next = glob[i+1];
                if (next == '*') {
                    re += "\\*";
                    i++;
                } else if (next == '?') {
                    re += "\\?";
                    i++;
                } else {
                    re += "\\\\";
                }
            } else {
                re += "\\\\";
            }
        } else if (c == '*') {
            re += ".*";
        } else if (c == '?') {
            re += ".";
        } else if (c == '.' || c == '+' || c == '(' || c == ')' || c == '[' || c == ']' ||
                   c == '{' || c == '}' || c == '^' || c == '$' || c == '|' || c == '$') {
            re += "\\";
            re += c;
        } else {
            re += c;
        }
    }
    return re;
}

static bool matchPattern(const String& segVal, const String& pattern) {
    if (isRegexSyntax(pattern)) {
        try {
            std::string sv((const char*)segVal.data(), segVal.size());
            std::string pt((const char*)pattern.data(), pattern.size());
            std::regex re(pt);
            return std::regex_match(sv, re);
        } catch (...) {
        }
    }
    
    bool hasGlob = false;
    for (usz i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            i++;
        } else if (pattern[i] == '*' || pattern[i] == '?') {
            hasGlob = true;
            break;
        }
    }
    
    if (hasGlob) {
        String reStr = globToRegex(pattern);
        try {
            std::string sv((const char*)segVal.data(), segVal.size());
            std::string pt((const char*)reStr.data(), reStr.size());
            std::regex re(pt);
            return std::regex_match(sv, re);
        } catch (...) {
        }
    }
    
    String unescaped;
    for (usz i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '\\') {
            if (i + 1 < pattern.size()) {
                unescaped += (char)pattern[i+1];
                i++;
            } else {
                unescaped += '\\';
            }
        } else {
            unescaped += c;
        }
    }
    return segVal == unescaped;
}

struct ColSelection {
    String name;
    bool hasRange = false;
    BlobRange range;
    bool hasHash = false;
    bool hasDiff = false;
};

static ColSelection parseColSelection(const String& spec) {
    ColSelection sel;
    long long bracketPos = -1;
    for (usz i = 0; i < spec.size(); ++i) {
        if (spec[i] == '[') { bracketPos = (long long)i; break; }
    }
    String base = (bracketPos >= 0) ? spec.slice(0, bracketPos) : spec;
    if (bracketPos >= 0) {
        sel.hasRange = true;
        sel.range = parseBlobRange(spec.slice(bracketPos));
    }
    if (base.endsWith(":hash")) {
        sel.hasHash = true;
        sel.name = base.substring(0, base.size() - 5);
    } else if (base.endsWith(":diff")) {
        sel.hasDiff = true;
        sel.name = base.substring(0, base.size() - 5);
    } else {
        sel.name = base;
    }
    return sel;
}

struct QueryStep {
    bool isFollow = false;
    bool isRepeatFollow = false;
    bool isAssert = false;
    Array<Clauses> groups;
};

static Array<QueryStep> groupClausesIntoSteps(const Array<Clauses>& clauses) {
    Array<QueryStep> steps;
    if (clauses.size() == 0) return steps;

    QueryStep current;
    current.isFollow = clauses[0].isFollow;
    current.isRepeatFollow = clauses[0].isRepeatFollow;
    current.isAssert = clauses[0].isAssert;
    current.groups.push(clauses[0]);

    for (usz i = 1; i < clauses.size(); ++i) {
        const auto& c = clauses[i];
        if (c.isFollow == current.isFollow &&
            c.isRepeatFollow == current.isRepeatFollow &&
            c.isAssert == current.isAssert &&
            c.isOrConnection) {
            current.groups.push(c);
        } else {
            steps.push(current);
            current.groups.clear();
            current.isFollow = c.isFollow;
            current.isRepeatFollow = c.isRepeatFollow;
            current.isAssert = c.isAssert;
            current.groups.push(c);
        }
    }
    steps.push(current);
    return steps;
}

struct PathPatternResolution {
    String op;
    String val;
};

static PathPatternResolution resolvePathSegmentPattern(const String& pattern) {
    if (isRegexSyntax(pattern)) {
        return {"reg", pattern};
    }
    
    bool hasGlob = false;
    for (usz i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\') {
            i++;
        } else if (pattern[i] == '*' || pattern[i] == '?') {
            hasGlob = true;
            break;
        }
    }
    
    if (hasGlob) {
        return {"reg", globToRegex(pattern)};
    }
    
    String unescaped;
    for (usz i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '\\') {
            if (i + 1 < pattern.size()) {
                unescaped += (char)pattern[i+1];
                i++;
            } else {
                unescaped += '\\';
            }
        } else {
            unescaped += c;
        }
    }
    return {"=", unescaped};
}

static Array<QueryStep> groupAndExpandClauses(const Array<Clauses>& clauses, TableStore* tableStore) {
    Array<QueryStep> rawSteps = groupClausesIntoSteps(clauses);
    Array<QueryStep> expandedSteps;
    
    for (usz i = 0; i < rawSteps.size(); ++i) {
        const QueryStep& step = rawSteps[i];
        
        bool hasPathOp = false;
        String pathCol;
        String pathPattern;
        usz pathGroupIdx = 0;
        usz pathClauseIdx = 0;
        
        for (usz gi = 0; gi < step.groups.size(); ++gi) {
            for (usz ci = 0; ci < step.groups[gi].size(); ++ci) {
                if (step.groups[gi][ci].op == "path") {
                    hasPathOp = true;
                    pathCol = step.groups[gi][ci].col;
                    pathPattern = step.groups[gi][ci].val;
                    pathGroupIdx = gi;
                    pathClauseIdx = ci;
                    break;
                }
            }
            if (hasPathOp) break;
        }
        
        if (hasPathOp) {
            String p = pathPattern;
            if (p.startsWith("\"") && p.endsWith("\"")) p = p.slice(1, p.size() - 1);
            Array<String> parts = p.split("/");
            Array<String> cleanParts;
            for (usz j = 0; j < parts.size(); ++j) {
                if (!parts[j].isEmpty()) cleanParts.push(parts[j]);
            }
            
            if (cleanParts.size() == 0) {
                QueryStep newStep;
                newStep.isFollow = step.isFollow;
                newStep.isRepeatFollow = step.isRepeatFollow;
                newStep.isAssert = step.isAssert;
                
                Clauses c;
                c.push({"parent_id", "=", "0"});
                newStep.groups.push(c);
                expandedSteps.push(newStep);
            } else {
                for (usz j = 0; j < cleanParts.size(); ++j) {
                    QueryStep newStep;
                    newStep.isAssert = step.isAssert;
                    
                    if (j == 0 && !step.isFollow && !step.isRepeatFollow) {
                        newStep.isFollow = false;
                        newStep.isRepeatFollow = false;
                        
                        Clauses c;
                        c.push({"parent_id", "=", "0"});
                        if (cleanParts[j] != "*" && cleanParts[j] != "**") {
                            auto res = resolvePathSegmentPattern(cleanParts[j]);
                            c.push({pathCol, res.op, res.val});
                        }
                        newStep.groups.push(c);
                        expandedSteps.push(newStep);
                        
                        if (cleanParts[j] == "**") {
                            QueryStep repStep;
                            repStep.isRepeatFollow = true;
                            repStep.isAssert = step.isAssert;
                            Clauses rc;
                            rc.push({"parent_id", "=", "parent.id"});
                            repStep.groups.push(rc);
                            expandedSteps.push(repStep);
                        }
                    } else {
                        if (cleanParts[j] == "*") {
                            newStep.isFollow = true;
                            Clauses c;
                            c.push({"parent_id", "=", "parent.id"});
                            newStep.groups.push(c);
                        } else if (cleanParts[j] == "**") {
                            newStep.isRepeatFollow = true;
                            Clauses c;
                            c.push({"parent_id", "=", "parent.id"});
                            newStep.groups.push(c);
                        } else {
                            newStep.isFollow = true;
                            Clauses c;
                            c.push({"parent_id", "=", "parent.id"});
                            auto res = resolvePathSegmentPattern(cleanParts[j]);
                            c.push({pathCol, res.op, res.val});
                            newStep.groups.push(c);
                        }
                        expandedSteps.push(newStep);
                    }
                }
                
                QueryStep& lastStep = expandedSteps[expandedSteps.size() - 1];
                Clauses pathBaseClauses = lastStep.groups[0];
                
                lastStep.groups.clear();
                for (usz gi = 0; gi < step.groups.size(); ++gi) {
                    Clauses merged = pathBaseClauses;
                    for (usz ci = 0; ci < step.groups[gi].size(); ++ci) {
                        if (gi == pathGroupIdx && ci == pathClauseIdx) continue;
                        merged.push(step.groups[gi][ci]);
                    }
                    lastStep.groups.push(merged);
                }
            }
        } else {
            expandedSteps.push(step);
        }
    }
    return expandedSteps;
}

TableStore::TableStore(BlockDevice* dev, Allocator* alloc, Array<String>* keys) 
    : device(dev), allocator(alloc), globalKeys(keys) {}

Array<u64> TableStore::getMatchingRowIds(const Array<Clauses>& clauses, u64 snapshotSeq, u64 txId) {
    Array<u64> result;
    Array<u64> candidateIds;
    bool fastPathUsed = false;
    if (clauses.size() == 1) { 
        bool allEq = true;
        for (const auto& c : clauses[0]) {
            if (c.op != "=") { allEq = false; break; }
            if (c.val.startsWith("parent.")) { allEq = false; break; } 
        }
        if (allEq && clauses[0].size() > 0 && !clauses[0].isAssert && !disableIndex) {
            if (colHashIndexDirty) rebuildColHashIndex();
            
            bool impossible = false;
            bool firstCol = true;
            for (const auto& c : clauses[0]) {
                auto* valMap = colHashIndex.get(c.col);
                if (!valMap) { impossible = true; break; } 
                
                auto* rowIds = valMap->get(c.val);
                if (!rowIds) { impossible = true; break; } 
                
                if (firstCol) {
                    candidateIds = *rowIds;
                    firstCol = false;
                } else {
                    Array<u64> intersected;
                    for (u64 rId : *rowIds) {
                        for (u64 crId : candidateIds) {
                            if (rId == crId) { intersected.push(rId); break; }
                        }
                    }
                    candidateIds = intersected;
                    if (candidateIds.size() == 0) { impossible = true; break; }
                }
            }
            if (!impossible) {
                fastPathUsed = true;
            }
        }
    }

    const Array<u64>& scanList = fastPathUsed ? candidateIds : allRowIds;
    for (u64 rId : scanList) {
        if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
        Map<String, String>* row = fetchRow(rId);
        if (row && evaluateClauses(*row, clauses, nullptr, rId) >= 0.0f) {
            result.push(rId);
        }
    }
    return result;
}

TableStore::~TableStore() {
    for (auto it = colBloomFilters.begin(); it != colBloomFilters.end(); ++it) {
        delete it->value;
    }
    colBloomFilters.clear();
}

usz approxRowBytes(const Map<String, String>& row) {
    usz b = 64;
    for(auto it = row.begin(); it != row.end(); ++it) {
        b += it->key.size() + it->value.size() + 16;
    }
    return b;
}

void TableStore::updateLru(u64 id) {
    if (lruMap.has(id)) {
        // Remove from current position
        LruNode& node = *lruMap.get(id);
        if (node.prev != 0xFFFFFFFFFFFFFFFFULL) lruMap.get(node.prev)->next = node.next;
        else lruHead = node.next;
        if (node.next != 0xFFFFFFFFFFFFFFFFULL) lruMap.get(node.next)->prev = node.prev;
        else lruTail = node.prev;
        // Append to tail
        node.prev = lruTail; node.next = 0xFFFFFFFFFFFFFFFFULL;
        if (lruTail != 0xFFFFFFFFFFFFFFFFULL) lruMap.get(lruTail)->next = id;
        lruTail = id;
        if (lruHead == 0xFFFFFFFFFFFFFFFFULL) lruHead = id;
    } else {
        LruNode node; node.id = id; node.prev = lruTail; node.next = 0xFFFFFFFFFFFFFFFFULL;
        lruMap.set(id, node);
        if (lruTail != 0xFFFFFFFFFFFFFFFFULL) lruMap.get(lruTail)->next = id;
        lruTail = id;
        if (lruHead == 0xFFFFFFFFFFFFFFFFULL) lruHead = id;
    }
}

void TableStore::evictIfNeeded() {
    if (!saveToDisk) return;
    while (currentMemoryBytes > maxMemoryBytes && lruHead != 0xFFFFFFFFFFFFFFFFULL) {
        u64 evictId = lruHead;
        LruNode& node = *lruMap.get(evictId);
        lruHead = node.next;
        if (lruHead != 0xFFFFFFFFFFFFFFFFULL) lruMap.get(lruHead)->prev = 0xFFFFFFFFFFFFFFFFULL;
        else lruTail = 0xFFFFFFFFFFFFFFFFULL;
        lruMap.remove(evictId);
        
        if (allRows.has(evictId)) {
            Map<String, String>& row = *allRows.get(evictId);
            if (saveToDisk) {
                // If the block is dirty, we just mark it dirty. The volatile flag ensures it goes to SWAP.
                u64 blockId = evictId / 1000;
                dirtyBlocks.set(blockId, true);
                // We do NOT call volatileRows.remove(evictId) here because we still need to know it's volatile when flushing!
                // But we remove from memory.
                saveToDisk(evictId, &row); // This just calls an external callback if any
            }
            currentMemoryBytes -= approxRowBytes(row);
            allRows.remove(evictId);
        }
    }
}

Map<String, String>* TableStore::fetchRow(u64 id) {
    if (allRows.has(id)) {
        updateLru(id);
        return allRows.get(id);
    }
    u64 blockId = id / 1000;
    if (loadBlock(blockId)) {
        if (allRows.has(id)) {
            updateLru(id);
            return allRows.get(id);
        }
    }
    if (fetchFromDisk) {
        Map<String, String>* r = fetchFromDisk(id);
        if (r) {
            allRows.set(id, *r);
            currentMemoryBytes += approxRowBytes(*r);
            delete r;
            updateLru(id);
            dirtyBlocks.set(blockId, true);
            evictIfNeeded();
            return allRows.get(id);
        }
    }
    return nullptr;
}

// ─── Schema Persistence ─────────────────────────────────────────────────────

void TableStore::loadSchemas() {
    if (!readBlob) return;
    String data = readBlob("XYLM_TABLE_ROOT");
    if (data.isEmpty()) return;
    
    const u8* ptr = data.data();
    const u8* end = data.data() + data.size();
    auto readVLU = [&ptr, end]() -> u64 {
        u64 val = 0; int shift = 0;
        while (ptr < end) {
            u8 b = *ptr++;
            val |= (u64)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
        }
        return val;
    };
    
    nextRowId = readVLU();
    u64 rc = readVLU();
    allRowIds.allocate(rc);
    for(u64 i=0; i<rc; ++i) {
        allRowIds[i] = readVLU();
        allRowIdsMap.set(allRowIds[i], true);
    }
    
    u64 hasHnsw = readVLU();
    if (hasHnsw) {
        hnsw = new HNSW(readVLU());
        hnsw->entryPoint = readVLU();
        hnsw->maxLevel = (int)readVLU() - 1;
        hnsw->maxMemoryBytes = this->maxMemoryBytes;
        if (saveHNSWToDisk) hnsw->saveToDisk = saveHNSWToDisk;
        if (fetchHNSWFromDisk) hnsw->fetchFromDisk = fetchHNSWFromDisk;
        if (removeHNSWFromDisk) hnsw->removeFromDisk = removeHNSWFromDisk;
    }
    
    u64 uningestCount = readVLU();
    uningestedVectors.allocate(uningestCount);
    for(u64 i=0; i<uningestCount; ++i) uningestedVectors[i] = readVLU();

    // Load MVCC currentSeq
    if (ptr < end) {
        currentSeq = readVLU();
    }

    // Load ZSTD dictionary if available
    if (ptr < end) {
        u64 dictLen = readVLU();
        if (dictLen > 0 && ptr + dictLen <= end) {
            String dict((const u8*)ptr, dictLen);
            ptr += dictLen;
#ifdef XI_ZSTD_ENABLED
            if (blobStore) {
                blobStore->zstd.setDictionary(dict);
            }
#endif
        }
    }

    // Load blobRefCounts
    blobRefCounts.clear();
    bool refCountsLoaded = false;
    if (ptr < end) {
        u64 refCountSize = readVLU();
        for (u64 i = 0; i < refCountSize && ptr < end; ++i) {
            u64 hashLen = readVLU();
            if (end - ptr < (ptrdiff_t)hashLen) break;
            String hash((const u8*)ptr, hashLen);
            ptr += hashLen;
            u64 refVal = readVLU();
            blobRefCounts.set(hash, (u32)refVal);
        }
        refCountsLoaded = true;
    }

    if (!refCountsLoaded) {
        // Rebuild ref counts (fallback for legacy databases)
        for (u64 rId : allRowIds) {
            Map<String, String>* row = fetchRow(rId);
            if (row) {
                for (auto it = row->begin(); it != row->end(); ++it) {
                    String v = it->value;
                    if (globalKeys) v = CryptItem::decrypt(v, *globalKeys);
                    if (isBlobRef(v)) incrementBlobRef(extractBlobHash(v));
                }
            }
        }
    }
}

void TableStore::saveSchemas() {
    if (!writeBlob) return;
    usz needed = 20; // nextRowId + counts
    needed += allRowIds.size() * 10; // VLU row IDs
    needed += uningestedVectors.size() * 10;
    needed += 40; // HNSW metadata
#ifdef XI_ZSTD_ENABLED
    if (blobStore && blobStore->zstd.dictionary.size() > 0) {
        needed += 10 + blobStore->zstd.dictionary[0].size();
    }
#endif
    needed += blobRefCounts.size() * 60; // VLU counts + hash strings
    
    String data; data.allocate(needed);
    u8* ptr = data.data();
    
    auto writeVLU = [&ptr](u64 val) {
        while (val >= 0x80) {
            *ptr++ = (val & 0x7F) | 0x80;
            val >>= 7;
        }
        *ptr++ = val & 0x7F;
    };
    
    writeVLU(nextRowId);
    writeVLU(allRowIds.size());
    for(usz i=0; i<allRowIds.size(); ++i) writeVLU(allRowIds[i]);
    
    if (hnsw) {
        writeVLU(1);
        writeVLU(hnsw->dim);
        writeVLU(hnsw->entryPoint);
        writeVLU((u64)(hnsw->maxLevel + 1));
    } else {
        writeVLU(0);
    }
    
    // Save only actual uningestedVectors (not all row IDs)
    writeVLU(uningestedVectors.size());
    for(usz i=0; i<uningestedVectors.size(); ++i) writeVLU(uningestedVectors[i]);

    // Save MVCC currentSeq
    writeVLU(currentSeq);
    
    // Save ZSTD dictionary if available
#ifdef XI_ZSTD_ENABLED
    if (blobStore && blobStore->zstd.dictionary.size() > 0) {
        const String& dict = blobStore->zstd.dictionary[0];
        writeVLU(dict.size());
        for (usz i = 0; i < dict.size(); ++i) *ptr++ = dict[i];
    } else {
        writeVLU(0);
    }
#else
    writeVLU(0);
#endif
    
    // Save blobRefCounts
    writeVLU(blobRefCounts.size());
    for (auto it = blobRefCounts.begin(); it != blobRefCounts.end(); ++it) {
        const String& hash = it->key;
        writeVLU(hash.size());
        for (usz i = 0; i < hash.size(); ++i) *ptr++ = hash[i];
        writeVLU(it->value);
    }
    
    writeBlob("XYLM_TABLE_ROOT", data.slice(0, ptr - data.data()));
}

u16 TableStore::findOrCreateTable(const Array<Clause>& columns) {
    return 1;
}

// ─── Blob Helpers ───────────────────────────────────────────────────────────

String TableStore::resolveValue(const String& val) {
    String decVal = val;
    if (globalKeys) decVal = CryptItem::decrypt(val, *globalKeys);
    if (isBlobRef(decVal) && blobStore) {
        String hash = extractBlobHash(decVal);
        String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
        if (!content.isEmpty()) return content;
    }
    return decVal;
}

void TableStore::incrementBlobRef(const String& hash) {
    if (blobRefCounts.has(hash)) {
        (*blobRefCounts.get(hash))++;
    } else {
        blobRefCounts.set(hash, 1);
        if (blobStore) {
            auto ref = blobStore->getBlobRef(hash);
            blobStore->setBlobUsed(ref, true);
        }
    }
}

void TableStore::decrementBlobRef(const String& hash) {
    if (!blobRefCounts.has(hash)) return;
    u32& count = *blobRefCounts.get(hash);
    if (count <= 1) {
        blobRefCounts.remove(hash);
        if (blobStore && !blobStore->isFixed(hash)) {
            auto ref = blobStore->getBlobRef(hash);
            blobStore->setBlobUsed(ref, false);
        }
    } else {
        count--;
    }
}

bool TableStore::isBlobReferenced(const String& hash, u64 excludeRowId) {
    // Only used conceptually, now replaced by blobRefCounts logic.
    // Keeping for interface compatibility but ref counting handles it.
    if (blobRefCounts.has(hash) && *blobRefCounts.get(hash) > 0) return true;
    return false;
}

void TableStore::cleanupBlobRefs(const Map<String, String>& row, u64 /*excludeRowId*/) {
    if (!blobStore) return;
    for (auto it = row.begin(); it != row.end(); ++it) {
        String v = it->value;
        if (globalKeys) v = CryptItem::decrypt(v, *globalKeys);
        if (isBlobRef(v)) {
            decrementBlobRef(extractBlobHash(v));
        }
    }
}

String TableStore::applyBlobRange(const String& existingContent, const String& newData, const BlobRange& range) {
    if (range.isAppend) {
        // [+] = append
        String result;
        result.allocate(existingContent.size() + newData.size());
        for (usz i = 0; i < existingContent.size(); ++i) result[i] = existingContent[i];
        for (usz i = 0; i < newData.size(); ++i) result[existingContent.size() + i] = newData[i];
        return result;
    }
    if (range.isInsert) {
        // [30+] = insert at position, shifting right
        usz insertPos = (usz)range.start;
        if (insertPos > existingContent.size()) insertPos = existingContent.size();
        String result;
        result.allocate(existingContent.size() + newData.size());
        for (usz i = 0; i < insertPos; ++i) result[i] = existingContent[i];
        for (usz i = 0; i < newData.size(); ++i) result[insertPos + i] = newData[i];
        for (usz i = insertPos; i < existingContent.size(); ++i) result[newData.size() + i] = existingContent[i];
        return result;
    }
    // Overwrite mode: [0:20] or [30:]
    usz start = (usz)range.start;
    usz writeLen = newData.size();
    usz resultSize = existingContent.size();
    if (start + writeLen > resultSize) resultSize = start + writeLen;
    String result;
    result.allocate(resultSize);
    // Copy existing
    for (usz i = 0; i < existingContent.size(); ++i) result[i] = existingContent[i];
    // Fill any gap with zeros
    for (usz i = existingContent.size(); i < start; ++i) result[i] = 0;
    // Overwrite
    for (usz i = 0; i < writeLen; ++i) result[start + i] = newData[i];
    return result;
}

void TableStore::rebuildColHashIndex() {
    colHashIndex.clear();
    for (usz idx = 0; idx < allRowIds.size(); ++idx) {
        u64 rId = allRowIds[idx];
        Map<String, String>* row = fetchRow(rId);
        if (!row) continue;
        for (auto it = row->begin(); it != row->end(); ++it) {
            if (it->key == "content" || it->value.size() > 256) continue;
            String val = it->value;
            if (globalKeys) val = CryptItem::decrypt(val, *globalKeys);
            if (!colHashIndex.has(it->key)) colHashIndex.set(it->key, Map<String, Array<u64>>());
            auto* valMap = colHashIndex.get(it->key);
            if (!valMap->has(val)) valMap->set(val, Array<u64>());
            valMap->get(val)->push(rId);
        }
    }
    colHashIndexDirty = false;
}

void TableStore::invalidateColHashIndex() {
    colHashIndexDirty = true;
}

// ─── MVCC Helpers ───────────────────────────────────────────────────────────

bool TableStore::isVisibleToSnapshot(u64 rowId, u64 snapshotSeq, u64 txId) {
    // Non-transactional reads see everything (backward compatible)
    if (snapshotSeq == 0 && txId == 0) return true;

    // Check if this row was modified after our snapshot by another transaction
    auto* modSeq = rowModSeq.get(rowId);
    if (!modSeq) return true; // No modification info, legacy row, always visible
    
    // If we're in a transaction and this row was modified after our snapshot,
    // it's still visible if WE modified it
    // (We don't store per-tx ownership of rows, so we rely on touchedRowIds in LockState)
    // For read visibility, we allow all committed rows visible at snapshot time
    if (*modSeq > snapshotSeq) return false;
    return true;
}

bool TableStore::hasConflicts(const Array<u64>& touchedIds, u64 snapshotSeq) {
    for (usz i = 0; i < touchedIds.size(); ++i) {
        auto* modSeq = rowModSeq.get(touchedIds[i]);
        if (modSeq && *modSeq >= snapshotSeq) return true;
    }
    return false;
}

void TableStore::gcVersions(u64 oldestActiveSnapshot) {
    if (oldestActiveSnapshot == 0) return; // No active transactions, nothing to GC
    
    // Clean up rowModSeq entries for rows that:
    // 1. Have modSeq <= oldestActiveSnapshot (all txs can see them)
    // 2. Are no longer in any active transaction's touched set
    Array<u64> toClean;
    for (auto it = rowModSeq.begin(); it != rowModSeq.end(); ++it) {
        if (it->value <= oldestActiveSnapshot) {
            toClean.push(it->key);
        }
    }
    for (usz i = 0; i < toClean.size(); ++i) {
        rowModSeq.remove(toClean[i]);
    }
    
    // Clean up saveToDisk for evicted rows that are in allRowIds but not allRows
    // (they've been persisted and are only referenced by ID)
    if (saveToDisk) {
        for (u64 rId : allRowIds) {
            if (!allRows.has(rId) && !volatileRows.has(rId)) {
                // Row was evicted and persisted — remove the ROW_* blob
                // if no active transaction references it
                // (For now, keep it — full version chain GC is deferred)
            }
        }
    }
}

// ─── Clause Evaluation ─────────────────────────────────────────────────────

f32 TableStore::evaluateClause(const Map<String, String>& row, const Clause& clause, const Map<String, String>* parentRow, u64 rId) {
    if (clause.op == "path") {
        String key = clause.col + "::" + clause.val;
        auto* ids = precomputedPaths.get(key);
        if (ids) {
            for (usz i = 0; i < ids->size(); ++i) {
                if ((*ids)[i] == rId) return 1.0f;
            }
        }
        return -1.0f;
    }

    String val;
    if (!row.has(clause.col)) {
        if (clause.col == "parent_id") {
            val = "0";
        } else {
            return -1.0f;
        }
    } else {
        val = *row.get(clause.col);
    }
    
    // Decrypt
    if (globalKeys) val = CryptItem::decrypt(val, *globalKeys);
    
    // Resolve blob refs transparently
    if (isBlobRef(val) && blobStore) {
        String hash = extractBlobHash(val);
        String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
        if (!content.isEmpty()) val = content;
    }

    String targetVal = clause.val;
    if (parentRow && targetVal.startsWith("parent.")) {
        String pKey = targetVal.substring(7);
        if (parentRow->has(pKey)) {
            targetVal = *parentRow->get(pKey);
        } else {
            return -1.0f;
        }
    }

    if (clause.op == "=") return (val == targetVal) ? 1.0f : -1.0f;

    if (clause.op == "hash") {
        return (Security::hash(val, 16) == targetVal) ? 1.0f : -1.0f;
    }

    // Numeric comparison operators
    if (clause.op == "<" || clause.op == ">" || clause.op == "<=" ||
        clause.op == ">=" || clause.op == "==") {
        auto parseD = [](const String& s, double& out) -> bool {
            if (s.isEmpty()) return false;
            const char* p = (const char*)s.data();
            char* end = nullptr;
            out = std::strtod(p, &end);
            return end != p;
        };
        double a = 0.0, b = 0.0;
        if (!parseD(val, a) || !parseD(targetVal, b)) return -1.0f;
        bool match = false;
        if      (clause.op == "<" ) match = a <  b;
        else if (clause.op == ">" ) match = a >  b;
        else if (clause.op == "<=") match = a <= b;
        else if (clause.op == ">=") match = a >= b;
        else                        match = a == b; // =="
        return match ? 1.0f : -1.0f;
    }

    // Regex match
    if (clause.op == "reg") {
        try {
            std::string sv((const char*)val.data(),       val.size());
            std::string pt((const char*)targetVal.data(), targetVal.size());
            std::regex  re(pt);
            return std::regex_search(sv, re) ? 1.0f : -1.0f;
        } catch (...) {
            return -1.0f;
        }
    }

    if (clause.op == "cos") {
        if (val.size() != targetVal.size() || val.size() % sizeof(f32) != 0) return -1.0f;
        const f32* v1 = reinterpret_cast<const f32*>(val.data());
        const f32* v2 = reinterpret_cast<const f32*>(targetVal.data());
        usz len = val.size() / sizeof(f32);
        f32 dot = 0.0f, mag1 = 0.0f, mag2 = 0.0f;
        for (usz i = 0; i < len; ++i) {
            dot  += v1[i] * v2[i];
            mag1 += v1[i] * v1[i];
            mag2 += v2[i] * v2[i];
        }
        if (mag1 == 0.0f || mag2 == 0.0f) return 0.0f;
        return dot / (std::sqrt(mag1) * std::sqrt(mag2));
    }

    return -1.0f;
}

f32 TableStore::evaluateClauses(const Map<String, String>& row, const Array<Clauses>& clausesGroups, const Map<String, String>* parentRow, u64 rId) {
    if (clausesGroups.size() == 0) return 1.0f;
    f32 maxScore = -1.0f;
    for (const auto& group : clausesGroups) {
        f32 groupMinScore = 1.0f;
        bool groupMatch = true;
        for (const auto& clause : group) {
            f32 s = evaluateClause(row, clause, parentRow, rId);
            if (s < 0.0f) {
                groupMatch = false;
                break;
            }
            if (s < groupMinScore) groupMinScore = s;
        }
        if (groupMatch) {
            if (groupMinScore > maxScore) maxScore = groupMinScore;
        }
    }
    return maxScore;
}

// ─── Read ───────────────────────────────────────────────────────────────────

Array<Map<String, String>> TableStore::read(const Array<String>& columns, const Array<Clauses>& clauses,
                                             u64 length, bool tombstones,
                                             u64 snapshotSeq, u64 txId,
                                             bool readAllColumns) {
    Array<Map<String, String>> result;

    if (colBloomFiltersDirty) rebuildBloomFilters();

    Array<QueryStep> steps = groupAndExpandClauses(clauses, this);

    if (steps.size() == 0) {
        QueryStep allStep;
        allStep.isFollow = false;
        allStep.isRepeatFollow = false;
        allStep.isAssert = false;
        steps.push(allStep);
    }
    Map<String, u64> idToRowId;
    Map<String, Array<u64>> parentToRowIds;
    for (u64 rId : allRowIds) {
        if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
        Map<String, String>* row = fetchRow(rId);
        if (!row) continue;
        
        String idVal = row->has("id") ? *row->get("id") : "";
        if (globalKeys) idVal = CryptItem::decrypt(idVal, *globalKeys);
        if (!idVal.isEmpty()) {
            idToRowId.set(idVal, rId);
        }
        
        String pId = row->has("parent_id") ? *row->get("parent_id") : "0";
        if (globalKeys) pId = CryptItem::decrypt(pId, *globalKeys);
        parentToRowIds[pId].push(rId);
    }



    Array<u64> currentRows;
    Array<u64> assertRows;
    bool inAssertPath = false;

    for (usz si = 0; si < steps.size(); ++si) {
        const QueryStep& step = steps[si];

        // Apply Bloom filter check to filter step.groups
        Array<Clauses> activeGroups;
        for (const auto& group : step.groups) {
            bool groupImpossible = false;
            for (const auto& clause : group) {
                if ((clause.op == "=" || clause.op == "==") && !clause.val.startsWith("parent.")) {
                    if (clause.col == "parent_id" && clause.val == "0") {
                        continue;
                    }
                    if (colBloomFilters.has(clause.col)) {
                        auto* bf = colBloomFilters.get(clause.col);
                        if (bf && !(*bf)->contains(clause.val)) {
                            groupImpossible = true;
                            break;
                        }
                    }
                }
            }
            if (!groupImpossible) {
                activeGroups.push(group);
            }
        }

        if (step.isAssert) {
            // Check if this step starts a new assert path
            bool startsAssertPath = false;
            if (!inAssertPath) {
                for (const auto& group : activeGroups) {
                    for (const auto& clause : group) {
                        if ((clause.col == "parent_id" && clause.val == "0") || clause.op == "path") {
                            startsAssertPath = true;
                            break;
                        }
                    }
                    if (startsAssertPath) break;
                }
            }

            if (startsAssertPath) {
                inAssertPath = true;
                assertRows.clear();
            }

            if (inAssertPath) {
                if (activeGroups.size() == 0) {
                    assertRows.clear();
                } else if (startsAssertPath) {
                    for (u64 rId : allRowIds) {
                        if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                        Map<String, String>* row = fetchRow(rId);
                        if (!row) continue;
                        if (activeGroups.size() == 0 || evaluateClauses(*row, activeGroups, nullptr, rId) >= 0.0f) {
                            assertRows.push(rId);
                        }
                    }
                } else if (step.isFollow) {
                    Array<u64> nextRows;
                    for (usz rIdx = 0; rIdx < assertRows.size(); ++rIdx) {
                        u64 parentId = assertRows[rIdx];
                        Map<String, String>* parentRow = fetchRow(parentId);
                        if (!parentRow) continue;

                        Array<u64> candidates;
                        bool hasIndex = false;
                        for (const auto& group : activeGroups) {
                            for (const auto& clause : group) {
                                if (clause.op == "=" && clause.val.startsWith("parent.")) {
                                    String pKey = clause.val.substring(7);
                                    if (parentRow->has(pKey)) {
                                        String targetVal = *parentRow->get(pKey);
                                        if (clause.col == "parent_id" && parentToRowIds.has(targetVal)) {
                                            const auto& childIds = *parentToRowIds.get(targetVal);
                                            for (usz k = 0; k < childIds.size(); ++k) candidates.push(childIds[k]);
                                            hasIndex = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (hasIndex) break;
                        }

                        const Array<u64>& scanList = hasIndex ? candidates : allRowIds;
                        for (u64 rId : scanList) {
                            if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                            Map<String, String>* row = fetchRow(rId);
                            if (!row) continue;
                            if (evaluateClauses(*row, activeGroups, parentRow, rId) >= 0.0f) {
                                bool alreadyAdded = false;
                                for (usz k = 0; k < nextRows.size(); ++k) {
                                    if (nextRows[k] == rId) { alreadyAdded = true; break; }
                                }
                                if (!alreadyAdded) nextRows.push(rId);
                            }
                        }
                    }
                    assertRows = nextRows;
                } else if (step.isRepeatFollow) {
                    Array<u64> nextRows;
                    nextRows.allocate(assertRows.size());
                    for (usz k = 0; k < assertRows.size(); ++k) nextRows[k] = assertRows[k];
                    
                    Array<u64> queue;
                    queue.allocate(assertRows.size());
                    for (usz k = 0; k < assertRows.size(); ++k) queue[k] = assertRows[k];
                    Map<u64, bool> visited;
                    for (usz k = 0; k < assertRows.size(); ++k) visited.set(assertRows[k], true);

                    usz qIdx = 0;
                    while (qIdx < queue.size()) {
                        u64 parentId = queue[qIdx++];
                        Map<String, String>* parentRow = fetchRow(parentId);
                        if (!parentRow) continue;

                        Array<u64> candidates;
                        bool hasIndex = false;
                        for (const auto& group : activeGroups) {
                            for (const auto& clause : group) {
                                if (clause.op == "=" && clause.val.startsWith("parent.")) {
                                    String pKey = clause.val.substring(7);
                                    if (parentRow->has(pKey)) {
                                        String targetVal = *parentRow->get(pKey);
                                        if (clause.col == "parent_id" && parentToRowIds.has(targetVal)) {
                                            const auto& childIds = *parentToRowIds.get(targetVal);
                                            for (usz k = 0; k < childIds.size(); ++k) candidates.push(childIds[k]);
                                            hasIndex = true;
                                            break;
                                        }
                                    }
                                }
                            }
                            if (hasIndex) break;
                        }

                        const Array<u64>& scanList = hasIndex ? candidates : allRowIds;
                        for (u64 rId : scanList) {
                            if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                            if (visited.has(rId)) continue;
                            Map<String, String>* row = fetchRow(rId);
                            if (!row) continue;
                            if (evaluateClauses(*row, activeGroups, parentRow, rId) >= 0.0f) {
                                visited.set(rId, true);
                                queue.push(rId);
                                nextRows.push(rId);
                            }
                        }
                    }
                    assertRows = nextRows;
                }

                bool nextIsAssert = (si + 1 < steps.size() && steps[si+1].isAssert);
                if (!nextIsAssert) {
                    if (assertRows.size() == 0) {
                        return {};
                    }
                    inAssertPath = false;
                }
            } else {
                if (currentRows.size() == 0 || activeGroups.size() == 0) {
                    return {};
                }
                for (usz rIdx = 0; rIdx < currentRows.size(); ++rIdx) {
                    u64 rId = currentRows[rIdx];
                    Map<String, String>* row = fetchRow(rId);
                    if (!row || evaluateClauses(*row, activeGroups, nullptr, rId) < 0.0f) {
                        return {};
                    }
                }
            }
        } else if (step.isFollow) {
            inAssertPath = false;
            if (activeGroups.size() == 0) {
                currentRows.clear();
                continue;
            }
            Array<u64> nextRows;
            for (usz rIdx = 0; rIdx < currentRows.size(); ++rIdx) {
                u64 parentId = currentRows[rIdx];
                Map<String, String>* parentRow = fetchRow(parentId);
                if (!parentRow) continue;

                Array<u64> candidates;
                bool hasIndex = false;
                for (const auto& group : activeGroups) {
                    for (const auto& clause : group) {
                        if (clause.op == "=" && clause.val.startsWith("parent.")) {
                            String pKey = clause.val.substring(7);
                            if (parentRow->has(pKey)) {
                                String targetVal = *parentRow->get(pKey);
                                if (clause.col == "parent_id" && parentToRowIds.has(targetVal)) {
                                    const auto& childIds = *parentToRowIds.get(targetVal);
                                    for (usz k = 0; k < childIds.size(); ++k) candidates.push(childIds[k]);
                                    hasIndex = true;
                                    break;
                                } else if (colHashIndex.has(clause.col)) {
                                    auto* valMap = colHashIndex.get(clause.col);
                                    if (valMap->has(targetVal)) {
                                        const auto& childIds = *valMap->get(targetVal);
                                        for (usz k = 0; k < childIds.size(); ++k) candidates.push(childIds[k]);
                                        hasIndex = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (hasIndex) break;
                }

                const Array<u64>& scanList = hasIndex ? candidates : allRowIds;
                for (u64 rId : scanList) {
                    if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                    Map<String, String>* row = fetchRow(rId);
                    if (!row) continue;
                    if (evaluateClauses(*row, activeGroups, parentRow, rId) >= 0.0f) {
                        bool alreadyAdded = false;
                        for (usz k = 0; k < nextRows.size(); ++k) {
                            if (nextRows[k] == rId) { alreadyAdded = true; break; }
                        }
                        if (!alreadyAdded) nextRows.push(rId);
                    }
                }
            }
            currentRows = nextRows;
        } else if (step.isRepeatFollow) {
            inAssertPath = false;
            if (activeGroups.size() == 0) {
                currentRows.clear();
                continue;
            }
            Array<u64> nextRows;
            nextRows.allocate(currentRows.size());
            for (usz k = 0; k < currentRows.size(); ++k) nextRows[k] = currentRows[k];
            
            Array<u64> queue;
            queue.allocate(currentRows.size());
            for (usz k = 0; k < currentRows.size(); ++k) queue[k] = currentRows[k];
            Map<u64, bool> visited;
            for (usz k = 0; k < currentRows.size(); ++k) visited.set(currentRows[k], true);

            usz qIdx = 0;
            while (qIdx < queue.size()) {
                u64 parentId = queue[qIdx++];
                Map<String, String>* parentRow = fetchRow(parentId);
                if (!parentRow) continue;

                Array<u64> candidates;
                bool hasIndex = false;
                for (const auto& group : activeGroups) {
                    for (const auto& clause : group) {
                        if (clause.op == "=" && clause.val.startsWith("parent.")) {
                            String pKey = clause.val.substring(7);
                            if (parentRow->has(pKey)) {
                                String targetVal = *parentRow->get(pKey);
                                if (clause.col == "parent_id" && parentToRowIds.has(targetVal)) {
                                    const auto& childIds = *parentToRowIds.get(targetVal);
                                    for (usz k = 0; k < childIds.size(); ++k) candidates.push(childIds[k]);
                                    hasIndex = true;
                                    break;
                                } else if (colHashIndex.has(clause.col)) {
                                    auto* valMap = colHashIndex.get(clause.col);
                                    if (valMap->has(targetVal)) {
                                        const auto& childIds = *valMap->get(targetVal);
                                        for (usz k = 0; k < childIds.size(); ++k) candidates.push(childIds[k]);
                                        hasIndex = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (hasIndex) break;
                }

                const Array<u64>& scanList = hasIndex ? candidates : allRowIds;
                for (u64 rId : scanList) {
                    if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                    if (visited.has(rId)) continue;
                    Map<String, String>* row = fetchRow(rId);
                    if (!row) continue;
                    if (evaluateClauses(*row, activeGroups, parentRow, rId) >= 0.0f) {
                        visited.set(rId, true);
                        queue.push(rId);
                        nextRows.push(rId);
                    }
                }
            }
            currentRows = nextRows;
        } else {
            inAssertPath = false;
            if (activeGroups.size() == 0) {
                if (step.groups.size() > 0) {
                    currentRows.clear();
                }
                continue;
            }
            Array<u64> candidates;
            bool fastPathUsed = false;
            if (activeGroups.size() == 1) { 
                bool allEq = true;
                for (const auto& c : activeGroups[0]) {
                    if (c.op != "=") { allEq = false; break; }
                    if (c.val.startsWith("parent.")) { allEq = false; break; } 
                }
                if (allEq && activeGroups[0].size() > 0 && !disableIndex) {
                    if (colHashIndexDirty) rebuildColHashIndex();
                    
                    bool impossible = false;
                    bool firstCol = true;
                    for (const auto& c : activeGroups[0]) {
                        auto* valMap = colHashIndex.get(c.col);
                        if (!valMap) { impossible = true; break; } 
                        
                        auto* rowIds = valMap->get(c.val);
                        if (!rowIds) { impossible = true; break; } 
                        
                        if (firstCol) {
                            candidates = *rowIds;
                            firstCol = false;
                        } else {
                            Array<u64> intersected;
                            for (u64 rId : *rowIds) {
                                for (u64 crId : candidates) {
                                    if (rId == crId) { intersected.push(rId); break; }
                                }
                            }
                            candidates = intersected;
                            if (candidates.size() == 0) { impossible = true; break; }
                        }
                    }
                    if (!impossible) {
                        fastPathUsed = true;
                    }
                }
            }

            const Array<u64>& scanList = fastPathUsed ? candidates : allRowIds;
            for (u64 rId : scanList) {
                if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                Map<String, String>* row = fetchRow(rId);
                if (!row) continue;
                if (activeGroups.size() == 0 || evaluateClauses(*row, activeGroups, nullptr, rId) >= 0.0f) {
                    currentRows.push(rId);
                }
            }
        }
    }

    auto applySelect = [&](Map<String, String>& row) -> Map<String, String> {
        Map<String, String> outRow;
        
        Map<String, ColSelection> selMap;
        for (usz i = 0; i < columns.size(); ++i) {
            ColSelection sel = parseColSelection(columns[i]);
            selMap.set(sel.name, sel);
        }

        Array<String> outputKeys;
        if (readAllColumns || columns.size() == 0) {
            for (auto it = row.begin(); it != row.end(); ++it) {
                outputKeys.push(it->key);
            }
        } else {
            for (usz i = 0; i < columns.size(); ++i) {
                ColSelection sel = parseColSelection(columns[i]);
                outputKeys.push(sel.name);
            }
        }

        for (usz i = 0; i < outputKeys.size(); ++i) {
            String key = outputKeys[i];
            if (!row.has(key)) continue;

            String rawVal = *row.get(key);
            String decryptedVal = globalKeys ? CryptItem::decrypt(rawVal, *globalKeys) : rawVal;

            ColSelection sel;
            if (selMap.has(key)) {
                sel = *selMap.get(key);
            } else {
                sel.name = key;
            }

            if (sel.hasHash) {
                if (isBlobRef(decryptedVal)) {
                    outRow.set(key, extractBlobHash(decryptedVal));
                } else {
                    outRow.set(key, Security::hash(decryptedVal, 16));
                }
            } else if (sel.hasDiff) {
                if (isBlobRef(decryptedVal) && blobStore) {
                    String hash = extractBlobHash(decryptedVal);
                    auto* meta = blobStore->index.get(hash);
                    if (meta && meta->isDiff) {
                        String diffBin = blobStore->readChain(meta->blockIdx);
                        #ifdef XI_ZSTD_ENABLED
                        diffBin = blobStore->zstd.decompress(diffBin);
                        #endif
                        if (globalKeys && globalKeys->size() > 0) {
                            String dec = CryptItem::decrypt(diffBin, *globalKeys);
                            if (!dec.isEmpty()) diffBin = dec;
                        }
                        outRow.set(key, diffBin);
                    } else {
                        String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
                        outRow.set(key, content);
                    }
                } else {
                    outRow.set(key, decryptedVal);
                }
            } else if (sel.hasRange) {
                String val;
                if (isBlobRef(decryptedVal) && blobStore) {
                    String hash = extractBlobHash(decryptedVal);
                    u64 start = sel.range.start;
                    u64 end = (sel.range.end > 0 && !sel.range.isSingleIndex) ? sel.range.end : 0xFFFFFFFF;
                    if (sel.range.isSingleIndex) {
                        end = start + 1;
                    }
                    val = blobStore->readHash(hash, start, end);
                } else {
                    val = decryptedVal;
                    u64 start = sel.range.start;
                    u64 end = (sel.range.end > 0 && !sel.range.isSingleIndex) ? sel.range.end : (u64)val.size();
                    if (sel.range.isSingleIndex) {
                        end = start + 1;
                    }
                    if (start < (u64)val.size()) {
                        if (end > (u64)val.size()) end = (u64)val.size();
                        val = val.slice((long long)start, (long long)end);
                    } else {
                        val = "";
                    }
                }
                outRow.set(key, val);
            } else {
                String val;
                if (isBlobRef(decryptedVal) && blobStore) {
                    String hash = extractBlobHash(decryptedVal);
                    val = blobStore->readHash(hash, 0, 0xFFFFFFFF);
                } else {
                    val = decryptedVal;
                }
                outRow.set(key, val);
            }
        }
        return outRow;
    };

    const Clause* cosClause = nullptr;
    if (steps.size() > 0 && steps[0].groups.size() > 0 && steps[0].groups[0].size() > 0 && steps[0].groups[0][0].op == "cos") {
        cosClause = &steps[0].groups[0][0];
    }

    if (cosClause) {
        struct ScoredRow {
            f32 score;
            u64 rId;
        };
        Array<ScoredRow> scored;
        for (usz i = 0; i < currentRows.size(); ++i) {
            u64 rId = currentRows[i];
            Map<String, String>* row = fetchRow(rId);
            if (!row) continue;
            f32 s = evaluateClause(*row, *cosClause, nullptr, rId);
            scored.push({s, rId});
        }
        std::sort(scored.data(), scored.data() + scored.size(), [](const ScoredRow& a, const ScoredRow& b) {
            return a.score > b.score;
        });
        u64 count = 0;
        for (usz i = 0; i < scored.size(); ++i) {
            if (length > 0 && count >= length) break;
            Map<String, String>* row = fetchRow(scored[i].rId);
            if (row) {
                result.push(applySelect(*row));
                count++;
            }
        }
    } else {
        u64 count = 0;
        for (usz i = 0; i < currentRows.size(); ++i) {
            if (length > 0 && count >= length) break;
            Map<String, String>* row = fetchRow(currentRows[i]);
            if (row) {
                result.push(applySelect(*row));
                count++;
            }
        }
    }

    return result;
}

// ─── Write ──────────────────────────────────────────────────────────────────

int TableStore::write(const Array<Clause>& columns, const Array<Clauses>& clauses,
                      const String& encryptionKey, u64 txId, bool isVolatile) {
    u16 tId = findOrCreateTable(columns);
    
    resolvePathsInClauses(clauses, currentSeq, txId);
    
    // Check ASSERT clauses
    for (usz gi = 0; gi < clauses.size(); ++gi) {
        const auto& group = clauses[gi];
        if (group.isAssert) {
            for (u64 rId : allRowIds) {
                Map<String, String>* row = fetchRow(rId);
                if (!row) continue;
                Array<Clauses> temp; temp.push(group);
                if (evaluateClauses(*row, temp, nullptr, rId) >= 0.0f) {
                    return (int)(gi + 1); // 1-based ASSERT clause index
                }
            }
        }
    }

    if (clauses.size() == 0) {
        // ─── Insert ─────────────────────────────────────────────────────────
        
        // Prevent duplicates from journal replay by checking if row with this explicit id already exists
        String explicitId;
        for (const auto& c : columns) {
            ParsedCol pc = parseCol(c.col);
            if (pc.name == "id") { explicitId = c.val; break; }
        }
        if (!explicitId.isEmpty()) {
            Array<String> idCols; idCols.push("id");
            Array<Clauses> checkWhere; checkWhere.push(WHERE("id", "=", explicitId));
            // Bypass MVCC snapshot for this check to guarantee uniqueness globally
            if (read(idCols, checkWhere, 0, false, 0, 0).size() > 0) {
                return 0; // Already exists, drop this insert (likely from journal replay)
            }
        }

        // Check for virtual columns
        bool hasVirtual = false;
        Map<String, String> virtualRow; // For watcher notification
        
        Map<String, String> row;
        for (auto& c : columns) {
            ParsedCol pc = parseCol(c.col);
            
            if (pc.type == ColType::VIRTUAL) {
                hasVirtual = true;
                virtualRow.set(pc.name, c.val);
                continue;
            }
            
            if (pc.type == ColType::BLOB && blobStore) {
                // Hash the content, store blob, save reference
                String hash = Security::hash(c.val, 16);
                blobStore->writeHash(hash, 0, c.val, encryptionKey);
                incrementBlobRef(hash);
                String ref = makeBlobRef(hash);
                row.set(pc.name, encryptionKey.isEmpty() ? ref : CryptItem::encrypt(ref, encryptionKey));
            } else {
                row.set(pc.name, encryptionKey.isEmpty() ? c.val : CryptItem::encrypt(c.val, encryptionKey));
            }
        }
        
        // If ALL columns are virtual, just notify watchers, don't store
        if (row.size() == 0 && hasVirtual) {
            // virtualRow is passed to watchers via the engine
            return 0;
        }
        
        // Also add virtual columns to the row for watcher notification (but not storage)
        // The virtual columns were already excluded from `row`.
        
        u64 rId = nextRowId++;
        allRows.set(rId, row);
        allRowIds.push(rId);
        allRowIdsMap.set(rId, true);
        if (isVolatile) volatileRows.set(rId, true);
        else dirtyBlocks.set(rId / 1000, true);
        currentMemoryBytes += approxRowBytes(row);
        updateLru(rId);
        evictIfNeeded();
        
        if (!tableToRows.has(tId)) tableToRows.set(tId, Array<u64>());
        tableToRows.get(tId)->push(rId);
        
        // Update MVCC mod sequence
        currentSeq++;
        rowModSeq.set(rId, currentSeq);
        
        // Track embedding for lazy HNSW ingestion
        if (row.has("embedding")) {
            String emb = *row.get("embedding");
            if (globalKeys) {
                String k;
                String dec = CryptItem::decrypt(emb, *globalKeys, &k);
                if (!k.isEmpty()) emb = dec;
            }
            // Resolve blob ref to get actual embedding size for HNSW dim detection
            String rawEmb = emb;
            if (isBlobRef(rawEmb) && blobStore) {
                String hash = extractBlobHash(rawEmb);
                rawEmb = blobStore->readHash(hash, 0, 0xFFFFFFFF);
            }
            if (!hnsw && rawEmb.size() > 0) {
                hnsw = new HNSW(rawEmb.size() / sizeof(f32));
                hnsw->maxMemoryBytes = this->maxMemoryBytes;
                if (saveHNSWToDisk) hnsw->saveToDisk = saveHNSWToDisk;
                if (fetchHNSWFromDisk) hnsw->fetchFromDisk = fetchHNSWFromDisk;
                if (removeHNSWFromDisk) hnsw->removeFromDisk = removeHNSWFromDisk;
            }
            uningestedVectors.push(rId);
        }
        
        // Incremental index update
        if (!colHashIndexDirty) {
            for (auto it = row.begin(); it != row.end(); ++it) {
                if (it->key == "content" || it->value.size() > 256) continue;
                String val = it->value;
                if (globalKeys) val = CryptItem::decrypt(val, *globalKeys);
                if (!colHashIndex.has(it->key)) colHashIndex.set(it->key, Map<String, Array<u64>>());
                colHashIndex.get(it->key)->operator[](val).push(rId);
            }
        }
        colBloomFiltersDirty = true;
        return 0;
    }
    
    // ─── Update ─────────────────────────────────────────────────────────────
    Array<u64> candidateIds;
    bool fastPathUsed = false;
    if (clauses.size() == 1) { 
        bool allEq = true;
        for (const auto& c : clauses[0]) {
            if (c.op != "=") { allEq = false; break; }
            if (c.val.startsWith("parent.")) { allEq = false; break; } 
        }
        if (allEq && clauses[0].size() > 0 && !clauses[0].isAssert && !disableIndex) {
            if (colHashIndexDirty) rebuildColHashIndex();
            
            bool impossible = false;
            bool firstCol = true;
            for (const auto& c : clauses[0]) {
                auto* valMap = colHashIndex.get(c.col);
                if (!valMap) { impossible = true; break; } 
                
                auto* rowIds = valMap->get(c.val);
                if (!rowIds) { impossible = true; break; } 
                
                if (firstCol) {
                    candidateIds = *rowIds;
                    firstCol = false;
                } else {
                    Array<u64> intersected;
                    for (u64 rId : *rowIds) {
                        for (u64 crId : candidateIds) {
                            if (rId == crId) { intersected.push(rId); break; }
                        }
                    }
                    candidateIds = intersected;
                    if (candidateIds.size() == 0) { impossible = true; break; }
                }
            }
            if (!impossible) {
                fastPathUsed = true;
            }
        }
    }

    auto doUpdateRow = [&](u64 rId) {
        Map<String, String>* row = fetchRow(rId);
        if (!row) return;
        if (evaluateClauses(*row, clauses, nullptr, rId) >= 0.0f) {
            String keyToUse = encryptionKey;
            if (keyToUse.isEmpty() && globalKeys) {
                for (const auto& existingCol : columns) {
                    ParsedCol pc = parseCol(existingCol.col);
                    if (row->has(pc.name)) {
                        String dummyKey;
                        CryptItem::decrypt(*row->get(pc.name), *globalKeys, &dummyKey);
                        if (!dummyKey.isEmpty()) {
                            keyToUse = dummyKey;
                            break;
                        }
                    }
                }
            }
            
            for (const auto& col : columns) {
                ParsedCol pc = parseCol(col.col);
                if (pc.type == ColType::VIRTUAL) continue; // Skip virtual in updates
                
                String oldBlobHashToDecrement;
                if (row->has(pc.name)) {
                    String oldV = *row->get(pc.name);
                    if (globalKeys) oldV = CryptItem::decrypt(oldV, *globalKeys);
                    else if (!keyToUse.isEmpty()) {
                        Array<String> tempKeys; tempKeys.push(keyToUse);
                        oldV = CryptItem::decrypt(oldV, tempKeys);
                    }
                    if (isBlobRef(oldV)) {
                        oldBlobHashToDecrement = extractBlobHash(oldV);
                    }
                    
                    // Remove rId from colHashIndex[pc.name][oldV]
                    if (!colHashIndexDirty && pc.name != "content" && oldV.size() <= 256 && colHashIndex.has(pc.name)) {
                        auto* valMap = colHashIndex.get(pc.name);
                        if (valMap->has(oldV)) {
                            auto* rowIds = valMap->get(oldV);
                            for (usz j = 0; j < rowIds->size(); ++j) {
                                if ((*rowIds)[j] == rId) {
                                    (*rowIds)[j] = (*rowIds)[rowIds->size() - 1];
                                    rowIds->pop();
                                    break;
                                }
                            }
                        }
                    }
                }

                if (pc.type == ColType::BLOB && blobStore) {
                    String content = col.val;
                    // Handle partial blob writes
                    if (pc.rangeSpec.size() > 0) {
                        BlobRange br = parseBlobRange(pc.rangeSpec);
                        if (br.valid && row->has(pc.name)) {
                            String existing = resolveValue(*row->get(pc.name));
                            content = applyBlobRange(existing, col.val, br);
                        }
                    }
                    String hash = Security::hash(content, 16);
                    blobStore->writeHash(hash, 0, content, keyToUse, oldBlobHashToDecrement, currentSeq);
                    incrementBlobRef(hash);
                    String ref = makeBlobRef(hash);
                    row->set(pc.name, keyToUse.isEmpty() ? ref : CryptItem::encrypt(ref, keyToUse));
                } else {
                    row->set(pc.name, keyToUse.isEmpty() ? col.val : CryptItem::encrypt(col.val, keyToUse));
                }

                // Add newVal to colHashIndex[pc.name][newVal]
                if (!colHashIndexDirty && pc.name != "content" && row->has(pc.name)) {
                    String newVal = *row->get(pc.name);
                    if (newVal.size() <= 256) {
                        if (globalKeys) newVal = CryptItem::decrypt(newVal, *globalKeys);
                        else if (!keyToUse.isEmpty()) {
                            Array<String> tempKeys; tempKeys.push(keyToUse);
                            newVal = CryptItem::decrypt(newVal, tempKeys);
                        }
                        if (!colHashIndex.has(pc.name)) colHashIndex.set(pc.name, Map<String, Array<u64>>());
                        colHashIndex.get(pc.name)->operator[](newVal).push(rId);
                    }
                }

                // Decrement old blob ref now that the new value has been set
                if (!oldBlobHashToDecrement.isEmpty()) {
                    decrementBlobRef(oldBlobHashToDecrement);
                }
            }
            
            // Update MVCC mod sequence
            currentSeq++;
            rowModSeq.set(rId, currentSeq);
            if (isVolatile) volatileRows.set(rId, true);
            else dirtyBlocks.set(rId / 1000, true);
            
            currentMemoryBytes += approxRowBytes(*row);
            updateLru(rId);
        }
    };

    if (fastPathUsed) {
        Array<u64> targets;
        targets.reserve(candidateIds.size());
        for (u64 rId : candidateIds) {
            targets.push(rId);
        }
        for (u64 rId : targets) {
            doUpdateRow(rId);
        }
    } else {
        for (u64 rId : allRowIds) {
            doUpdateRow(rId);
        }
    }
    colBloomFiltersDirty = true;
    evictIfNeeded();
    return 0;
}

// ─── Remove ─────────────────────────────────────────────────────────────────

bool TableStore::remove(const Array<Clauses>& clauses, u64 length) {
    resolvePathsInClauses(clauses, currentSeq, 0);

    Array<u64> toRemove;
    Array<u64> candidateIds;
    bool fastPathUsed = false;
    if (clauses.size() == 1) { 
        bool allEq = true;
        for (const auto& c : clauses[0]) {
            if (c.op != "=") { allEq = false; break; }
            if (c.val.startsWith("parent.")) { allEq = false; break; } 
        }
        if (allEq && clauses[0].size() > 0 && !clauses[0].isAssert && !disableIndex) {
            if (colHashIndexDirty) rebuildColHashIndex();
            
            bool impossible = false;
            bool firstCol = true;
            for (const auto& c : clauses[0]) {
                auto* valMap = colHashIndex.get(c.col);
                if (!valMap) { impossible = true; break; } 
                
                auto* rowIds = valMap->get(c.val);
                if (!rowIds) { impossible = true; break; } 
                
                if (firstCol) {
                    candidateIds = *rowIds;
                    firstCol = false;
                } else {
                    Array<u64> intersected;
                    for (u64 rId : *rowIds) {
                        for (u64 crId : candidateIds) {
                            if (rId == crId) { intersected.push(rId); break; }
                        }
                    }
                    candidateIds = intersected;
                    if (candidateIds.size() == 0) { impossible = true; break; }
                }
            }
            if (!impossible) {
                fastPathUsed = true;
            }
        }
    }

    if (fastPathUsed) {
        for (u64 rId : candidateIds) {
            Map<String, String>* row = fetchRow(rId);
            if (!row) continue;
            if (evaluateClauses(*row, clauses, nullptr, rId) >= 0.0f) {
                toRemove.push(rId);
            }
        }
    } else {
        for (u64 rId : allRowIds) {
            Map<String, String>* row = fetchRow(rId);
            if (!row) continue;
            if (evaluateClauses(*row, clauses, nullptr, rId) >= 0.0f) {
                toRemove.push(rId);
            }
        }
    }
    for (usz i = 0; i < toRemove.size(); ++i) {
        u64 rId = toRemove[i];
        Map<String, String>* row = fetchRow(rId);
        if (row) {
            // Remove from HNSW if it has embedding
            if (row->has("embedding") && hnsw) {
                hnsw->remove(rId);
            }
            
            // Cleanup from colHashIndex
            if (!colHashIndexDirty) {
                for (auto it = row->begin(); it != row->end(); ++it) {
                    if (it->key == "content" || it->value.size() > 256) continue;
                    String val = it->value;
                    if (globalKeys) val = CryptItem::decrypt(val, *globalKeys);
                    
                    if (colHashIndex.has(it->key)) {
                        auto* valMap = colHashIndex.get(it->key);
                        if (valMap->has(val)) {
                            auto* rowIds = valMap->get(val);
                            for (usz j = 0; j < rowIds->size(); ++j) {
                                if ((*rowIds)[j] == rId) {
                                    (*rowIds)[j] = (*rowIds)[rowIds->size() - 1];
                                    rowIds->pop();
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            
            // Cleanup blob references
            cleanupBlobRefs(*row, rId);
            
            currentMemoryBytes -= approxRowBytes(*row);
            allRows.remove(rId);
        } else if (hnsw) {
            hnsw->remove(rId);
        }
        for(usz j=0; j<allRowIds.size(); ++j) {
            if(allRowIds[j] == rId) {
                allRowIds[j] = allRowIds[allRowIds.size() - 1];
                allRowIds.pop();
                break;
            }
        }
        allRowIdsMap.remove(rId);
        for(usz j=0; j<uningestedVectors.size(); ++j) {
            if(uningestedVectors[j] == rId) {
                uningestedVectors[j] = uningestedVectors[uningestedVectors.size() - 1];
                uningestedVectors.pop();
                break;
            }
        }
        // Clean up MVCC tracking
        if (!volatileRows.has(rId)) {
            dirtyBlocks.set(rId / 1000, true);
        }
        rowModSeq.remove(rId);
        volatileRows.remove(rId);
    }
    colBloomFiltersDirty = true;
    return true;
}


// ─── Flush ──────────────────────────────────────────────────────────────────

// ─── Flush ──────────────────────────────────────────────────────────────────

void TableStore::flushAllRows() {
    if (!writeBlob) return;
    for (auto it = dirtyBlocks.begin(); it != dirtyBlocks.end(); ++it) {
        if (it->value) {
            u64 blockId = it->key;
            
            // Write persistent rows
            String data;
            serializeBlock(blockId, data, false);
            if (!data.isEmpty()) {
                writeBlob("ROW_BLOCK_" + String::from(blockId), data);
            }
            
            // Write volatile SWAP rows
            String volData;
            serializeBlock(blockId, volData, true);
            if (!volData.isEmpty()) {
                writeBlob("VOLATILE_BLOCK_" + String::from(blockId), volData);
            } else {
                if (blobStore) blobStore->removeHash("VOLATILE_BLOCK_" + String::from(blockId));
            }
            
            it->value = false;
        }
    }
}

bool TableStore::loadBlock(u64 blockId) {
    if (!readBlob) return false;
    bool loadedAny = false;
    
    // Load persistent block
    String data = readBlob("ROW_BLOCK_" + String::from(blockId));
    if (!data.isEmpty()) {
        deserializeBlock(blockId, data, false);
        loadedAny = true;
    }
    
    // Load volatile SWAP block
    String volData = readBlob("VOLATILE_BLOCK_" + String::from(blockId));
    if (!volData.isEmpty()) {
        deserializeBlock(blockId, volData, true);
        loadedAny = true;
    }
    
    return loadedAny;
}

void TableStore::serializeBlock(u64 blockId, String& out, bool volatileOnly) {
    u64 startId = blockId * 1000;
    u64 endId = startId + 1000;
    
    Array<u64> rowIds;
    for (u64 rId = startId; rId < endId; ++rId) {
        if (allRowIdsMap.has(rId)) {
            bool isVol = volatileRows.has(rId);
            if ((volatileOnly && isVol) || (!volatileOnly && !isVol)) {
                rowIds.push(rId);
            }
        }
    }
    
    u32 rowCount = rowIds.size();
    if (rowCount == 0) return;
    
    auto writeVLU = [](String& str, u64 val) {
        while (val >= 0x80) {
            str += (char)((val & 0x7F) | 0x80);
            val >>= 7;
        }
        str += (char)(val & 0x7F);
    };
    
    Array<String> colNames;
    for (u32 i = 0; i < rowCount; ++i) {
        Map<String, String>* row = fetchRow(rowIds[i]);
        if (row) {
            for (auto it = row->begin(); it != row->end(); ++it) {
                bool found = false;
                for (usz j = 0; j < colNames.size(); ++j) {
                    if (colNames[j] == it->key) { found = true; break; }
                }
                if (!found) colNames.push(it->key);
            }
        }
    }
    
    String payload;
    writeVLU(payload, rowCount);
    u32 tombstoneBytes = (rowCount + 7) / 8;
    for (u32 i = 0; i < tombstoneBytes; ++i) payload += (char)0;
    
    // Write relative row IDs (offset from startId)
    for (u32 i = 0; i < rowCount; ++i) {
        writeVLU(payload, rowIds[i] - startId);
    }
    
    writeVLU(payload, colNames.size());
    
    for (usz c = 0; c < colNames.size(); ++c) {
        String name = colNames[c];
        writeVLU(payload, name.size());
        payload += name;
        payload += (char)TypeTag::STRING;
        
        for (u32 i = 0; i < rowCount; ++i) {
            Map<String, String>* row = fetchRow(rowIds[i]);
            String val = (row && row->has(name)) ? *row->get(name) : String();
            writeVLU(payload, val.size());
        }
        for (u32 i = 0; i < rowCount; ++i) {
            Map<String, String>* row = fetchRow(rowIds[i]);
            String val = (row && row->has(name)) ? *row->get(name) : String();
            payload += val;
        }
    }
    
    String compressed;
#ifdef XI_ZSTD_ENABLED
    if (blobStore) {
        compressed = blobStore->zstd.compress(payload);
    } else {
        compressed = payload;
    }
#else
    compressed = payload;
#endif

    out.clear();
    out += (char)BlockType::TABLE;
    u16 tableId = 1 | 0x8000; // Flag bit 15 to indicate new format with relative row IDs
    out += (char)(tableId & 0xFF); out += (char)((tableId >> 8) & 0xFF);
    writeVLU(out, startId);
    writeVLU(out, compressed.size());
    out += compressed;
}

void TableStore::deserializeBlock(u64 blockId, const String& in, bool isVolatile) {
    if (in.size() < 4) return;
    const u8* ptr = (const u8*)in.data();
    const u8* end = (const u8*)in.data() + in.size();
    
    if (*ptr++ != (u8)BlockType::TABLE) return;
    u16 tableId = *ptr++; tableId |= ((u16)*ptr++ << 8);
    
    bool hasRelativeIds = (tableId & 0x8000) != 0;
    tableId &= 0x7FFF;
    
    auto readVLU = [](const u8*& p, const u8* e) -> u64 {
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
    
    u64 rowStartIndex = readVLU(ptr, end);
    u64 compressedSize = readVLU(ptr, end);
    
    if (end - ptr < (ptrdiff_t)compressedSize) return;
    String compressed((const u8*)ptr, compressedSize);
    
    String payload;
#ifdef XI_ZSTD_ENABLED
    if (blobStore) {
        payload = blobStore->zstd.decompress(compressed);
    } else {
        payload = compressed;
    }
#else
    payload = compressed;
#endif

    if (payload.isEmpty()) return;
    
    const u8* p = (const u8*)payload.data();
    const u8* pEnd = (const u8*)payload.data() + payload.size();
    
    u64 rowCount = readVLU(p, pEnd);
    u32 tombstoneBytes = (rowCount + 7) / 8;
    p += tombstoneBytes;
    
    Array<u64> rowIds;
    rowIds.allocate(rowCount);
    
    if (hasRelativeIds) {
        for (u32 i = 0; i < rowCount; ++i) {
            rowIds[i] = rowStartIndex + readVLU(p, pEnd);
            if (!allRowIdsMap.has(rowIds[i])) {
                allRowIds.push(rowIds[i]);
                allRowIdsMap.set(rowIds[i], true);
            }
        }
    } else {
        for (u32 i = 0; i < rowCount; ++i) {
            rowIds[i] = rowStartIndex + i;
            if (!allRowIdsMap.has(rowIds[i])) {
                allRowIds.push(rowIds[i]);
                allRowIdsMap.set(rowIds[i], true);
            }
        }
    }
    
    u64 numColumns = readVLU(p, pEnd);
    
    Array<Map<String, String>> rows;
    rows.allocate(rowCount);
    
    for (u64 c = 0; c < numColumns; ++c) {
        u64 nameLen = readVLU(p, pEnd);
        if (pEnd - p < (ptrdiff_t)nameLen) break;
        String colName((const u8*)p, nameLen);
        p += nameLen;
        
        p++; // skip typeTag
        
        Array<u64> lens;
        lens.allocate(rowCount);
        for (u32 i = 0; i < rowCount; ++i) {
            lens[i] = readVLU(p, pEnd);
        }
        for (u32 i = 0; i < rowCount; ++i) {
            u64 len = lens[i];
            if (pEnd - p < (ptrdiff_t)len) break;
            String val((const u8*)p, len);
            p += len;
            if (len > 0) {
                rows[i].set(colName, val);
            }
        }
    }
    
    for (u32 i = 0; i < rowCount; ++i) {
        if (rows[i].size() > 0) {
            allRows.set(rowIds[i], rows[i]);
            if (isVolatile) volatileRows.set(rowIds[i], true);
            currentMemoryBytes += approxRowBytes(rows[i]);
        }
    }
}

void TableStore::flushHnsw() {
    if (!hnsw || !saveHNSWToDisk) return;
    for (auto& pair : hnsw->nodes) {
        saveHNSWToDisk(pair.key, pair.value);
    }
}

Array<u64> TableStore::resolvePathPattern(const String& colName, const String& pathPattern, u64 snapshotSeq, u64 txId) {
    Array<u64> matchingRowIds;
    
    String p = pathPattern;
    if (p.startsWith("\"") && p.endsWith("\"")) p = p.slice(1, p.size() - 1);
    Array<String> parts = p.split("/");
    Array<String> cleanParts;
    for (usz i = 0; i < parts.size(); ++i) {
        if (!parts[i].isEmpty()) cleanParts.push(parts[i]);
    }
    
    Array<String> currentParentVals;
    currentParentVals.push("0");

    Map<String, u64> idToRowId;
    Map<String, Array<u64>> parentToRowIds;
    for (u64 rId : allRowIds) {
        if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
        Map<String, String>* row = fetchRow(rId);
        if (!row) continue;
        
        String idVal = row->has("id") ? *row->get("id") : "";
        if (globalKeys) idVal = CryptItem::decrypt(idVal, *globalKeys);
        if (!idVal.isEmpty()) {
            idToRowId.set(idVal, rId);
        }
        
        String pId = row->has("parent_id") ? *row->get("parent_id") : "0";
        if (globalKeys) pId = CryptItem::decrypt(pId, *globalKeys);
        parentToRowIds[pId].push(rId);
    }
    
    for (usz step = 0; step < cleanParts.size(); ++step) {
        const String& seg = cleanParts[step];
        Array<String> nextParentVals;
        Array<u64> stepMatchingRowIds;
        
        if (seg == "*") {
            for (usz j = 0; j < currentParentVals.size(); ++j) {
                String pId = currentParentVals[j];
                if (parentToRowIds.has(pId)) {
                    const auto& childIds = *parentToRowIds.get(pId);
                    for (usz k = 0; k < childIds.size(); ++k) {
                        u64 rId = childIds[k];
                        Map<String, String>* row = fetchRow(rId);
                        if (!row) continue;
                        String idVal = row->has("id") ? *row->get("id") : "";
                        if (globalKeys) idVal = CryptItem::decrypt(idVal, *globalKeys);
                        if (!idVal.isEmpty()) {
                            nextParentVals.push(idVal);
                            stepMatchingRowIds.push(rId);
                        }
                    }
                }
            }
        } else if (seg == "**") {
            Array<String> descendants = currentParentVals;
            
            for (usz j = 0; j < currentParentVals.size(); ++j) {
                if (idToRowId.has(currentParentVals[j])) {
                    stepMatchingRowIds.push(*idToRowId.get(currentParentVals[j]));
                }
            }
            
            Array<String> queue = currentParentVals;
            Map<String, bool> visited;
            for (usz j = 0; j < currentParentVals.size(); ++j) visited.set(currentParentVals[j], true);
            
            usz qIdx = 0;
            while (qIdx < queue.size()) {
                String pVal = queue[qIdx++];
                
                if (parentToRowIds.has(pVal)) {
                    const auto& childIds = *parentToRowIds.get(pVal);
                    for (usz k = 0; k < childIds.size(); ++k) {
                        u64 rId = childIds[k];
                        Map<String, String>* row = fetchRow(rId);
                        if (!row) continue;
                        String idVal = row->has("id") ? *row->get("id") : "";
                        if (globalKeys) idVal = CryptItem::decrypt(idVal, *globalKeys);
                        if (!idVal.isEmpty() && !visited.has(idVal)) {
                            visited.set(idVal, true);
                            descendants.push(idVal);
                            queue.push(idVal);
                            stepMatchingRowIds.push(rId);
                        }
                    }
                }
            }
            nextParentVals = descendants;
        } else {
            for (usz j = 0; j < currentParentVals.size(); ++j) {
                String pId = currentParentVals[j];
                if (parentToRowIds.has(pId)) {
                    const auto& childIds = *parentToRowIds.get(pId);
                    for (usz k = 0; k < childIds.size(); ++k) {
                        u64 rId = childIds[k];
                        Map<String, String>* row = fetchRow(rId);
                        if (!row) continue;
                        
                        String segVal = row->has(colName) ? *row->get(colName) : "";
                        if (globalKeys) segVal = CryptItem::decrypt(segVal, *globalKeys);
                        
                        if (matchPattern(segVal, seg)) {
                            String idVal = row->has("id") ? *row->get("id") : "";
                            if (globalKeys) idVal = CryptItem::decrypt(idVal, *globalKeys);
                            if (!idVal.isEmpty()) {
                                nextParentVals.push(idVal);
                                stepMatchingRowIds.push(rId);
                            }
                        }
                    }
                }
            }
        }
        
        currentParentVals = nextParentVals;
        if (step == cleanParts.size() - 1) {
            matchingRowIds = stepMatchingRowIds;
        }
    }
    
    return matchingRowIds;
}


void TableStore::resolvePathsInClauses(const Array<Clauses>& clauses, u64 snapshotSeq, u64 txId) {
    precomputedPaths.clear();
    for (usz gi = 0; gi < clauses.size(); ++gi) {
        const auto& group = clauses[gi];
        for (const auto& clause : group) {
            if (clause.op == "path") {
                String key = clause.col + "::" + clause.val;
                if (!precomputedPaths.has(key)) {
                    precomputedPaths.set(key, resolvePathPattern(clause.col, clause.val, snapshotSeq, txId));
                }
            }
        }
    }
}

void TableStore::rebuildBloomFilters() {
    for (auto it = colBloomFilters.begin(); it != colBloomFilters.end(); ++it) {
        delete it->value;
    }
    colBloomFilters.clear();
    
    for (u64 rId : allRowIds) {
        Map<String, String>* row = fetchRow(rId);
        if (!row) continue;
        for (auto it = row->begin(); it != row->end(); ++it) {
            String val = it->value;
            if (globalKeys) val = CryptItem::decrypt(val, *globalKeys);
            
            if (!colBloomFilters.has(it->key)) {
                colBloomFilters.set(it->key, new BloomFilter(8192));
            }
            (*colBloomFilters.get(it->key))->add(val);
        }
    }
    colBloomFiltersDirty = false;
}

} // namespace Xylem
