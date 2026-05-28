#include <Xylem/TableStore.hpp>
#include <Xylem/BlobStore.hpp>
#include <Xylem/CryptItem.hpp>
#include <Sec/Crypto.hpp>
#include <algorithm>
#include <cmath>
#include <regex>
#include <cstdlib>


namespace Xylem {

TableStore::TableStore(BlockDevice* dev, Allocator* alloc, Array<String>* keys) 
    : device(dev), allocator(alloc), globalKeys(keys) {}

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
    for(u64 i=0; i<rc; ++i) allRowIds[i] = readVLU();
    
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
    for (u64 rId : allRowIds) {
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
        if (modSeq && *modSeq > snapshotSeq) return true;
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

f32 TableStore::evaluateClause(const Map<String, String>& row, const Clause& clause, const Map<String, String>* parentRow) {
    if (!row.has(clause.col)) return -1.0f;
    String val = *row.get(clause.col);
    
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
        return (Sec::hash(val, 16) == targetVal) ? 1.0f : -1.0f;
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

f32 TableStore::evaluateClauses(const Map<String, String>& row, const Array<Clauses>& clausesGroups, const Map<String, String>* parentRow) {
    if (clausesGroups.size() == 0) return 1.0f;
    f32 maxScore = -1.0f;
    for (const auto& group : clausesGroups) {
        f32 groupMinScore = 1.0f;
        bool groupMatch = true;
        for (const auto& clause : group) {
            f32 s = evaluateClause(row, clause, parentRow);
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
                                             u64 snapshotSeq, u64 txId) {
    Array<Map<String, String>> result;

    for (usz gi = 0; gi < clauses.size(); ++gi) {
        const auto& group = clauses[gi];
        if (group.isAssert) {
            for (u64 rId : allRowIds) {
                if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                Map<String, String>* row = fetchRow(rId);
                if (!row) continue;
                Array<Clauses> temp; temp.push(group);
                if (evaluateClauses(*row, temp) >= 0.0f) {
                    return result; 
                }
            }
        }
    }

    auto applySelect = [&](Map<String, String>& row) -> Map<String, String> {
        Map<String, String> outRow;
        if (columns.size() == 0) {
            for (auto it = row.begin(); it != row.end(); ++it) {
                String val = globalKeys ? CryptItem::decrypt(it->value, *globalKeys) : it->value;
                // Resolve blob refs transparently
                if (isBlobRef(val) && blobStore) {
                    String hash = extractBlobHash(val);
                    String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
                    if (!content.isEmpty()) val = content;
                }
                outRow.set(it->key, val);
            }
        } else {
            for (const auto& colSpec : columns) {
                ParsedCol pc = parseCol(colSpec);
                if (!row.has(pc.name)) continue;
                String v = *row.get(pc.name);
                if (globalKeys) v = CryptItem::decrypt(v, *globalKeys);
                // Resolve blob refs transparently
                if (isBlobRef(v) && blobStore) {
                    String hash = extractBlobHash(v);
                    String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
                    if (!content.isEmpty()) v = content;
                }
                // Apply range spec for blob reads
                if (pc.rangeSpec.size() > 0) {
                    BlobRange br = parseBlobRange(pc.rangeSpec);
                    if (br.valid && !br.isInsert && !br.isAppend) {
                        u64 endPos = br.end > 0 ? br.end : (u64)v.size();
                        if (endPos > (u64)v.size()) endPos = (u64)v.size();
                        if (br.start < endPos) {
                            v = v.slice((long long)br.start, (long long)endPos);
                        }
                    }
                }
                outRow.set(pc.name, v);
            }
        }
        return outRow;
    };

    const Clause* cosClause = nullptr;
    for (const auto& group : clauses) {
        for (const auto& clause : group) {
            if (clause.op == "cos") cosClause = &clause;
        }
    }

    if (cosClause && hnsw) {
        if (uningestedVectors.size() > 0) {
            for (usz i = 0; i < uningestedVectors.size(); ++i) {
                u64 rId = uningestedVectors[i];
                Map<String, String>* row = fetchRow(rId);
                if (row && row->has("embedding")) {
                    String emb = *row->get("embedding");
                    if (globalKeys) {
                        String k;
                        String dec = CryptItem::decrypt(emb, *globalKeys, &k);
                        if (!k.isEmpty()) emb = dec;
                    }
                    // Resolve blob ref if embedding is stored as blob
                    if (isBlobRef(emb) && blobStore) {
                        String hash = extractBlobHash(emb);
                        String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
                        if (!content.isEmpty()) emb = content;
                    }
                    hnsw->insert(rId, reinterpret_cast<const f32*>(emb.data()));
                }
            }
            uningestedVectors.clear();
        }
        
        const f32* query = reinterpret_cast<const f32*>(cosClause->val.data());
        u32 fetchK = (length > 0) ? (length * 10) : 100;
        Array<u64> fastIds = hnsw->search(query, fetchK);
        
        u64 count = 0;
        for (usz i = 0; i < fastIds.size(); ++i) {
            u64 rId = fastIds[i];
            if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
            Map<String, String>* row = fetchRow(rId);
            if (!row) continue;
            
            if (evaluateClauses(*row, clauses) >= 0.0f) {
                result.push(applySelect(*row));
                count++;
                if (length > 0 && count >= length) break;
            }
        }
        return result;
    }

    struct ScoredRow {
        f32 score;
        Map<String, String> row;
    };
    Array<ScoredRow> matches;

    bool fastPathUsed = false;
    if (clauses.size() == 1) { 
        bool allEq = true;
        for (const auto& c : clauses[0]) {
            if (c.op != "=") { allEq = false; break; }
            if (c.val.startsWith("parent.")) { allEq = false; break; } 
        }
        if (allEq && clauses[0].size() > 0 && !clauses[0].isAssert && !disableIndex) {
            if (colHashIndexDirty) rebuildColHashIndex();
            
            Array<u64> candidateIds;
            bool firstCol = true;
            bool impossible = false;
            
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
                for (u64 rId : candidateIds) {
                    if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                    Map<String, String>* row = fetchRow(rId);
                    if (!row) continue;
                    f32 score = evaluateClauses(*row, clauses);
                    if (score >= 0.0f) {
                        ScoredRow sr; sr.score = score; sr.row = applySelect(*row);
                        matches.push(sr);
                    }
                }
            }
            fastPathUsed = true;
        }
    }

    if (!fastPathUsed) {
        for (u64 rId : allRowIds) {
            if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
            Map<String, String>* row = fetchRow(rId);
            if (!row) continue;
            f32 score = evaluateClauses(*row, clauses);
            if (score >= 0.0f) {
                ScoredRow sr;
                sr.score = score;
                sr.row = applySelect(*row);
                matches.push(sr);
            }
        }
    }
    
    if (length > 0 && matches.size() > length) {
        std::sort(matches.data(), matches.data() + matches.size(), [](const ScoredRow& a, const ScoredRow& b) {
            return a.score > b.score;
        });
    }
    
    u64 count = 0;
    for (usz i = 0; i < matches.size(); ++i) {
        if (length > 0 && count >= length) break;
        result.push(matches[i].row);
        count++;
    }
    
    return result;
}

// ─── Write ──────────────────────────────────────────────────────────────────

int TableStore::write(const Array<Clause>& columns, const Array<Clauses>& clauses,
                      const String& encryptionKey, u64 txId, bool isVolatile) {
    u16 tId = findOrCreateTable(columns);
    
    // Check ASSERT clauses
    for (usz gi = 0; gi < clauses.size(); ++gi) {
        const auto& group = clauses[gi];
        if (group.isAssert) {
            for (u64 rId : allRowIds) {
                Map<String, String>* row = fetchRow(rId);
                if (!row) continue;
                Array<Clauses> temp; temp.push(group);
                if (evaluateClauses(*row, temp) >= 0.0f) {
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
                String hash = Sec::hash(c.val, 16);
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
        if (evaluateClauses(*row, clauses) >= 0.0f) {
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
                    String hash = Sec::hash(content, 16);
                    blobStore->writeHash(hash, 0, content, keyToUse);
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
        for (u64 rId : candidateIds) {
            doUpdateRow(rId);
        }
    } else {
        for (u64 rId : allRowIds) {
            doUpdateRow(rId);
        }
    }
    evictIfNeeded();
    return 0;
}

// ─── Remove ─────────────────────────────────────────────────────────────────

bool TableStore::remove(const Array<Clauses>& clauses, u64 length) {
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
            if (evaluateClauses(*row, clauses) >= 0.0f) {
                toRemove.push(rId);
            }
        }
    } else {
        for (u64 rId : allRowIds) {
            Map<String, String>* row = fetchRow(rId);
            if (!row) continue;
            if (evaluateClauses(*row, clauses) >= 0.0f) {
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
    return true;
}

// ─── Graph Read ─────────────────────────────────────────────────────────────

Collection::TreeBranch* TableStore::graphRead(const Array<String>& columns, const Array<GraphOp>& ops,
                                               u64 limit, u64 snapshotSeq, u64 txId) {
    Collection::TreeBranch* root = new Collection::TreeBranch();
    root->setName("Root");
    
    Array<RowNode*> activeNodes;
    
    auto applySelect = [&](const Map<String, String>& row) -> Map<String, String> {
        Map<String, String> outRow;
        if (columns.size() == 0) {
            for (auto it = row.begin(); it != row.end(); ++it) {
                String val = globalKeys ? CryptItem::decrypt(it->value, *globalKeys) : it->value;
                if (isBlobRef(val) && blobStore) {
                    String hash = extractBlobHash(val);
                    String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
                    if (!content.isEmpty()) val = content;
                }
                outRow.set(it->key, val);
            }
        } else {
            for (const auto& colName : columns) {
                ParsedCol pc = parseCol(colName);
                if (!row.has(pc.name)) continue;
                String v = *row.get(pc.name);
                if (globalKeys) v = CryptItem::decrypt(v, *globalKeys);
                if (isBlobRef(v) && blobStore) {
                    String hash = extractBlobHash(v);
                    String content = blobStore->readHash(hash, 0, 0xFFFFFFFF);
                    if (!content.isEmpty()) v = content;
                }
                outRow.set(pc.name, v);
            }
        }
        return outRow;
    };
    
    for (usz opIdx = 0; opIdx < ops.size(); ++opIdx) {
        const GraphOp& op = ops[opIdx];
        
        if (op.type == GraphOpType::MATCH) {
            for (u64 rId : allRowIds) {
                if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                Map<String, String>* row = fetchRow(rId);
                if (!row) continue;
                if (evaluateClauses(*row, op.query) >= 0.0f) {
                    RowNode* n = new RowNode(rId, applySelect(*row));
                    activeNodes.push(n);
                    root->add(n);
                }
            }
        } else if (op.type == GraphOpType::FOLLOW) {
            Array<RowNode*> nextActive;
            bool fastPath = false;
            long long fastClauseIdx = -1;
            if (op.query.size() == 1) {
                for (usz ci = 0; ci < op.query[0].size(); ++ci) {
                    if (op.query[0][ci].op == "=" && op.query[0][ci].val.startsWith("parent.")) {
                        fastPath = true;
                        fastClauseIdx = (long long)ci;
                        break;
                    }
                }
            }
            
            if (fastPath && !disableIndex) {
                if (colHashIndexDirty) rebuildColHashIndex();
                String targetCol = op.query[0][fastClauseIdx].col;
                String parentKey = op.query[0][fastClauseIdx].val.substring(7);
                
                auto* valMap = colHashIndex.get(targetCol);
                
                for (RowNode* node : activeNodes) {
                    if (!node->row.has(parentKey)) continue;
                    String targetVal = *node->row.get(parentKey);
                    
                    if (valMap) {
                        auto* rowIds = valMap->get(targetVal);
                        if (rowIds) {
                            for (u64 rId : *rowIds) {
                                if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                                Map<String, String>* row = fetchRow(rId);
                                if (!row) continue;
                                if (evaluateClauses(*row, op.query, &node->row) >= 0.0f) {
                                    RowNode* child = new RowNode(rId, applySelect(*row));
                                    node->add(child);
                                    nextActive.push(child);
                                }
                            }
                        }
                    }
                }
            } else {
                for (RowNode* node : activeNodes) {
                    for (u64 rId : allRowIds) {
                        if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                        Map<String, String>* row = fetchRow(rId);
                        if (!row) continue;
                        if (evaluateClauses(*row, op.query, &node->row) >= 0.0f) {
                            RowNode* child = new RowNode(rId, applySelect(*row));
                            node->add(child);
                            nextActive.push(child);
                        }
                    }
                }
            }
            activeNodes = nextActive;
        } else if (op.type == GraphOpType::UNTIL) {
            Array<RowNode*> nextActive;
            for (RowNode* node : activeNodes) {
                if (evaluateClauses(node->row, op.query) < 0.0f) {
                    nextActive.push(node);
                }
            }
            activeNodes = nextActive;
        } else if (op.type == GraphOpType::REPEATFOLLOW) {
            Array<RowNode*> currentLevel = activeNodes;
            Array<RowNode*> allFound;
            Map<u64, bool> visited;
            for (RowNode* n : currentLevel) visited.set(n->rId, true);
            
            bool fastPath = false;
            long long fastClauseIdx = -1;
            if (op.query.size() == 1) {
                for (usz ci = 0; ci < op.query[0].size(); ++ci) {
                    if (op.query[0][ci].op == "=" && op.query[0][ci].val.startsWith("parent.")) {
                        fastPath = true;
                        fastClauseIdx = (long long)ci;
                        break;
                    }
                }
            }
            
            while (currentLevel.size() > 0) {
                Array<RowNode*> nextLevel;
                if (fastPath && !disableIndex) {
                    if (colHashIndexDirty) rebuildColHashIndex();
                    String targetCol = op.query[0][fastClauseIdx].col;
                    String parentKey = op.query[0][fastClauseIdx].val.substring(7);
                    auto* valMap = colHashIndex.get(targetCol);
                    
                    for (RowNode* node : currentLevel) {
                        if (op.untilQuery.size() > 0 && evaluateClauses(node->row, op.untilQuery) >= 0.0f) continue;
                        if (!node->row.has(parentKey)) continue;
                        String targetVal = *node->row.get(parentKey);
                        if (valMap) {
                            auto* rowIds = valMap->get(targetVal);
                            if (rowIds) {
                                for (u64 rId : *rowIds) {
                                    if (visited.has(rId)) continue;
                                    if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                                    Map<String, String>* row = fetchRow(rId);
                                    if (!row) continue;
                                    if (evaluateClauses(*row, op.query, &node->row) >= 0.0f) {
                                        RowNode* child = new RowNode(rId, applySelect(*row));
                                        node->add(child);
                                        nextLevel.push(child);
                                        allFound.push(child);
                                        visited.set(rId, true);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    for (RowNode* node : currentLevel) {
                        if (op.untilQuery.size() > 0 && evaluateClauses(node->row, op.untilQuery) >= 0.0f) {
                            continue;
                        }
                        for (u64 rId : allRowIds) {
                            if (visited.has(rId)) continue;
                            if (!isVisibleToSnapshot(rId, snapshotSeq, txId)) continue;
                            Map<String, String>* row = fetchRow(rId);
                            if (!row) continue;
                            if (evaluateClauses(*row, op.query, &node->row) >= 0.0f) {
                                RowNode* child = new RowNode(rId, applySelect(*row));
                                node->add(child);
                                nextLevel.push(child);
                                allFound.push(child);
                                visited.set(rId, true);
                            }
                        }
                    }
                }
                currentLevel = nextLevel;
            }
            activeNodes = allFound;
        }
    }
    
    if (limit > 0 && activeNodes.size() > limit) {
        for (usz i = limit; i < activeNodes.size(); ++i) {
            RowNode* extra = activeNodes[i];
            if (extra->parent) {
                if (Collection::TreeBranch* p = dynamic_cast<Collection::TreeBranch*>(extra->parent)) {
                    p->removeChild(extra);
                }
                delete extra;
            }
        }
    }
    
    return root;
}

// ─── Graph Write ────────────────────────────────────────────────────────────

int TableStore::graphWrite(const Array<GraphOp>& ops, const String& encryptionKey, u64 txId, bool isVolatile) {
    Array<u64> activeIds;
    
    for (usz opIdx = 0; opIdx < ops.size(); ++opIdx) {
        const GraphOp& op = ops[opIdx];
        
        if (op.type == GraphOpType::MATCH) {
            for (u64 rId : allRowIds) {
                Map<String, String>* row = fetchRow(rId);
                if (!row) continue;
                if (evaluateClauses(*row, op.query) >= 0.0f) {
                    activeIds.push(rId);
                }
            }
        } else if (op.type == GraphOpType::FOLLOW) {
            Array<u64> nextActive;
            bool fastPath = false;
            long long fastClauseIdx = -1;
            if (op.query.size() == 1) {
                for (usz ci = 0; ci < op.query[0].size(); ++ci) {
                    if (op.query[0][ci].op == "=" && op.query[0][ci].val.startsWith("parent.")) {
                        fastPath = true;
                        fastClauseIdx = (long long)ci;
                        break;
                    }
                }
            }
            
            if (fastPath && !disableIndex) {
                if (colHashIndexDirty) rebuildColHashIndex();
                String targetCol = op.query[0][fastClauseIdx].col;
                String parentKey = op.query[0][fastClauseIdx].val.substring(7);
                auto* valMap = colHashIndex.get(targetCol);
                
                for (u64 parentId : activeIds) {
                    Map<String, String>* pRow = fetchRow(parentId);
                    if (!pRow) continue;
                    if (!pRow->has(parentKey)) continue;
                    String targetVal = *pRow->get(parentKey);
                    
                    if (valMap) {
                        auto* rowIds = valMap->get(targetVal);
                        if (rowIds) {
                            for (u64 rId : *rowIds) {
                                Map<String, String>* row = fetchRow(rId);
                                if (!row) continue;
                                if (evaluateClauses(*row, op.query, pRow) >= 0.0f) {
                                    nextActive.push(rId);
                                }
                            }
                        }
                    }
                }
            } else {
                for (u64 parentId : activeIds) {
                    Map<String, String>* pRow = fetchRow(parentId);
                    if (!pRow) continue;
                    for (u64 rId : allRowIds) {
                        Map<String, String>* row = fetchRow(rId);
                        if (!row) continue;
                        if (evaluateClauses(*row, op.query, pRow) >= 0.0f) {
                            nextActive.push(rId);
                        }
                    }
                }
            }
            activeIds = nextActive;
        } else if (op.type == GraphOpType::UNTIL) {
            Array<u64> nextActive;
            for (u64 id : activeIds) {
                Map<String, String>* row = fetchRow(id);
                if (row && evaluateClauses(*row, op.query) < 0.0f) {
                    nextActive.push(id);
                }
            }
            activeIds = nextActive;
        } else if (op.type == GraphOpType::REPEATFOLLOW) {
            Array<u64> currentLevel = activeIds;
            Array<u64> allFound;
            Map<u64, bool> visited;
            for (u64 id : currentLevel) visited.set(id, true);
            
            bool fastPath = false;
            long long fastClauseIdx = -1;
            if (op.query.size() == 1) {
                for (usz ci = 0; ci < op.query[0].size(); ++ci) {
                    if (op.query[0][ci].op == "=" && op.query[0][ci].val.startsWith("parent.")) {
                        fastPath = true;
                        fastClauseIdx = (long long)ci;
                        break;
                    }
                }
            }
            
            while (currentLevel.size() > 0) {
                Array<u64> nextLevel;
                if (fastPath && !disableIndex) {
                    if (colHashIndexDirty) rebuildColHashIndex();
                    String targetCol = op.query[0][fastClauseIdx].col;
                    String parentKey = op.query[0][fastClauseIdx].val.substring(7);
                    auto* valMap = colHashIndex.get(targetCol);
                    
                    for (u64 parentId : currentLevel) {
                        Map<String, String>* pRow = fetchRow(parentId);
                        if (!pRow) continue;
                        if (op.untilQuery.size() > 0 && evaluateClauses(*pRow, op.untilQuery) >= 0.0f) continue;
                        if (!pRow->has(parentKey)) continue;
                        String targetVal = *pRow->get(parentKey);
                        
                        if (valMap) {
                            auto* rowIds = valMap->get(targetVal);
                            if (rowIds) {
                                for (u64 rId : *rowIds) {
                                    if (visited.has(rId)) continue;
                                    Map<String, String>* row = fetchRow(rId);
                                    if (!row) continue;
                                    if (evaluateClauses(*row, op.query, pRow) >= 0.0f) {
                                        nextLevel.push(rId);
                                        allFound.push(rId);
                                        visited.set(rId, true);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    for (u64 parentId : currentLevel) {
                        Map<String, String>* pRow = fetchRow(parentId);
                        if (!pRow) continue;
                        if (op.untilQuery.size() > 0 && evaluateClauses(*pRow, op.untilQuery) >= 0.0f) {
                            continue;
                        }
                        for (u64 rId : allRowIds) {
                            if (visited.has(rId)) continue;
                            Map<String, String>* row = fetchRow(rId);
                            if (!row) continue;
                            if (evaluateClauses(*row, op.query, pRow) >= 0.0f) {
                                nextLevel.push(rId);
                                allFound.push(rId);
                                visited.set(rId, true);
                            }
                        }
                    }
                }
                currentLevel = nextLevel;
            }
            activeIds = allFound;
        } else if (op.type == GraphOpType::SET) {
            for (u64 id : activeIds) {
                Map<String, String>* row = fetchRow(id);
                if (!row) continue;
                
                String keyToUse = encryptionKey;
                if (keyToUse.isEmpty() && globalKeys) {
                    for (const auto& existingCol : op.writeSet) {
                        if (row->has(existingCol.col)) {
                            String dummyKey;
                            CryptItem::decrypt(*row->get(existingCol.col), *globalKeys, &dummyKey);
                            if (!dummyKey.isEmpty()) {
                                keyToUse = dummyKey;
                                break;
                            }
                        }
                    }
                }
                
                for (const auto& col : op.writeSet) {
                    ParsedCol pc = parseCol(col.col);
                    if (pc.type == ColType::VIRTUAL) continue;
                    
                    if (pc.type == ColType::BLOB && blobStore) {
                        String content = col.val;
                        if (pc.rangeSpec.size() > 0) {
                            BlobRange br = parseBlobRange(pc.rangeSpec);
                            if (br.valid && row->has(pc.name)) {
                                String existing = resolveValue(*row->get(pc.name));
                                content = applyBlobRange(existing, col.val, br);
                            }
                        }
                        String hash = Sec::hash(content, 16);
                        blobStore->writeHash(hash, 0, content, keyToUse);
                        String ref = makeBlobRef(hash);
                        row->set(pc.name, keyToUse.isEmpty() ? ref : CryptItem::encrypt(ref, keyToUse));
                    } else {
                        row->set(pc.name, keyToUse.isEmpty() ? col.val : CryptItem::encrypt(col.val, keyToUse));
                    }
                }
                
                currentSeq++;
                rowModSeq.set(id, currentSeq);
                if (isVolatile) volatileRows.set(id, true);
                currentMemoryBytes += approxRowBytes(*row); 
                updateLru(id);
            }
            evictIfNeeded();
        }
    }
    return 0;
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
    Map<u64, bool> existMap;
    for (usz j = 0; j < allRowIds.size(); ++j) {
        existMap.set(allRowIds[j], true);
    }
    for (u64 rId = startId; rId < endId; ++rId) {
        if (existMap.has(rId)) {
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
    u16 tableId = 1;
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
    
    u64 numColumns = readVLU(p, pEnd);
    
    Array<u64> rowIds;
    rowIds.allocate(rowCount);
    Map<u64, bool> existMap;
    for (usz j = 0; j < allRowIds.size(); ++j) {
        existMap.set(allRowIds[j], true);
    }
    for (u32 i = 0; i < rowCount; ++i) {
        rowIds[i] = rowStartIndex + i;
        if (!existMap.has(rowIds[i])) {
            allRowIds.push(rowIds[i]);
            existMap.set(rowIds[i], true);
        }
    }
    
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

} // namespace Xylem
