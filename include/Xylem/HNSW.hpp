#ifndef XYLEM_HNSW_HPP
#define XYLEM_HNSW_HPP

#include <Collection/Array.hpp>
#include <Collection/Map.hpp>
#include <Xi/Random.hpp>
#include <cmath>
#include <algorithm>
#include <queue> 

namespace Xylem {

using namespace Collection;

class HNSW {
public:
    struct Node {
        u64 id;
        Array<f32> vec;
        Array<Array<u64>> neighbors; 
        
        usz approxBytes() const {
            usz b = sizeof(Node) + vec.size() * sizeof(f32);
            for(usz i=0; i<neighbors.size(); ++i) b += neighbors[i].size() * sizeof(u64);
            return b;
        }
    };

    usz dim;
    u32 M = 16;
    u32 M0 = 32;
    u32 efConstruction = 64;
    f32 m_L;
    
    // LRU and memory management
    usz currentMemoryBytes = 0;
    usz maxMemoryBytes = 1024 * 1024; // 1MB default
    
    Xi::Func<Node*(u64)> fetchFromDisk;
    Xi::Func<void(u64, Node*)> saveToDisk;
    Xi::Func<void(u64)> removeFromDisk;
    
    Map<u64, Node*> nodes;
    u64 entryPoint = 0;
    int maxLevel = -1;
    
    HNSW(usz dimension) : dim(dimension) {
        m_L = 1.0f / std::log((f32)M);
    }
    
    ~HNSW() {
        for (auto& pair : nodes) delete pair.value;
    }
    
    f32 distance(const f32* v1, const f32* v2) const {
        f32 dot = 0.0f, mag1 = 0.0f, mag2 = 0.0f;
        _Pragma("omp simd")
        for (usz i = 0; i < dim; ++i) {
            dot += v1[i] * v2[i];
            mag1 += v1[i] * v1[i];
            mag2 += v2[i] * v2[i];
        }
        if (mag1 == 0.0f || mag2 == 0.0f) return 1.0f;
        f32 cosSim = dot / (std::sqrt(mag1) * std::sqrt(mag2));
        return 1.0f - cosSim; 
    }

    Map<u64, u64> lruAccessTime;
    u64 currentTime = 0;

    void updateLru(u64 id) {
        if (id == entryPoint) return;
        if (nodes.has(id)) {
            Node* n = *nodes.get(id);
            if (n->neighbors.size() > 1) return; // Keep top layers pinned
        }
        currentTime++;
        lruAccessTime.set(id, currentTime);
    }
    
    void evictIfNeeded() {
        if (!saveToDisk) return;
        while (currentMemoryBytes > maxMemoryBytes && lruAccessTime.size() > 0) {
            u64 oldestId = 0;
            u64 minTime = (u64)-1;
            
            for (auto it = lruAccessTime.begin(); it != lruAccessTime.end(); ++it) {
                if (it->value < minTime) {
                    minTime = it->value;
                    oldestId = it->key;
                }
            }
            
            lruAccessTime.remove(oldestId);
            
            if (nodes.has(oldestId)) {
                Node* n = *nodes.get(oldestId);
                if (n->neighbors.size() <= 1 && oldestId != entryPoint) {
                    saveToDisk(oldestId, n);
                    currentMemoryBytes -= n->approxBytes();
                    delete n;
                    nodes.remove(oldestId);
                }
            }
        }
    }

    Node* fetchNode(u64 id) {
        if (nodes.has(id)) {
            updateLru(id);
            return *nodes.get(id);
        }
        if (fetchFromDisk) {
            Node* n = fetchFromDisk(id);
            if (n) {
                nodes.set(id, n);
                currentMemoryBytes += n->approxBytes();
                updateLru(id);
                evictIfNeeded();
                return n;
            }
        }
        return nullptr;
    }

    struct DistPair {
        f32 dist;
        u64 id;
        bool operator<(const DistPair& o) const { return dist < o.dist; }
        bool operator>(const DistPair& o) const { return dist > o.dist; }
    };

    Array<u64> searchLayer(u64 ep, const f32* query, int ef, int level) {
        std::priority_queue<DistPair, std::vector<DistPair>, std::less<DistPair>> topCandidates; 
        std::priority_queue<DistPair, std::vector<DistPair>, std::greater<DistPair>> candidates; 
        Map<u64, bool> visited;
        
        Node* epNode = fetchNode(ep);
        if (!epNode) return {};
        
        f32 d = distance(query, epNode->vec.data());
        topCandidates.push({d, ep});
        candidates.push({d, ep});
        visited.set(ep, true);
        
        while (!candidates.empty()) {
            DistPair c = candidates.top();
            candidates.pop();
            
            if (c.dist > topCandidates.top().dist) break;
            
            Node* cNode = fetchNode(c.id);
            if (!cNode) continue;
            if (level >= (int)cNode->neighbors.size()) continue;
            
            for (u64 neighborId : cNode->neighbors[level]) {
                if (!visited.has(neighborId)) {
                    visited.set(neighborId, true);
                    Node* nNode = fetchNode(neighborId);
                    if (!nNode) continue;
                    f32 nd = distance(query, nNode->vec.data());
                    
                    if (topCandidates.size() < (usz)ef || nd < topCandidates.top().dist) {
                        candidates.push({nd, neighborId});
                        topCandidates.push({nd, neighborId});
                        if (topCandidates.size() > (usz)ef) topCandidates.pop();
                    }
                }
            }
        }
        
        Array<u64> res;
        while (!topCandidates.empty()) {
            res.push(topCandidates.top().id);
            topCandidates.pop();
        }
        for (usz i = 0; i < res.size() / 2; ++i) {
            u64 t = res[i];
            res[i] = res[res.size() - 1 - i];
            res[res.size() - 1 - i] = t;
        }
        return res;
    }

    void remove(u64 id) {
        if (nodes.has(id)) {
            Node* n = *nodes.get(id);
            currentMemoryBytes -= n->approxBytes();
            delete n;
            nodes.remove(id);
        }
        if (lruAccessTime.has(id)) {
            lruAccessTime.remove(id);
        }
        if (removeFromDisk) removeFromDisk(id);
        
        if (id == entryPoint) {
            if (nodes.size() > 0) {
                entryPoint = nodes.begin()->key;
                maxLevel = -1; // Force rebuild path if entryPoint changes, or just let it naturally route
            } else {
                entryPoint = 0;
                maxLevel = -1;
            }
        }
    }

    void insert(u64 id, const f32* vec) {
        Node* node = new Node();
        node->id = id;
        node->vec.allocate(dim);
        for(usz i=0; i<dim; ++i) node->vec[i] = vec[i];
        
        int level = (int)(-std::log((f32)(Xi::randomNext() % 10000 + 1) / 10000.0f) * m_L);
        node->neighbors.allocate(level + 1);
        
        nodes.set(id, node);
        currentMemoryBytes += node->approxBytes();
        updateLru(id);
        evictIfNeeded();
        
        if (maxLevel == -1) {
            entryPoint = id;
            maxLevel = level;
            return;
        }
        
        u64 currObj = entryPoint;
        
        for (int l = maxLevel; l > level; --l) {
            Node* cNode = fetchNode(currObj);
            if (!cNode) break;
            f32 currDist = distance(vec, cNode->vec.data());
            bool changed = true;
            while (changed) {
                changed = false;
                if (l >= (int)cNode->neighbors.size()) break;
                for (u64 neighborId : cNode->neighbors[l]) {
                    Node* nNode = fetchNode(neighborId);
                    if (!nNode) continue;
                    f32 d = distance(vec, nNode->vec.data());
                    if (d < currDist) {
                        currDist = d;
                        currObj = neighborId;
                        cNode = fetchNode(currObj);
                        if (!cNode) { changed = false; break; }
                        changed = true;
                    }
                }
            }
        }
        
        for (int l = std::min(level, maxLevel); l >= 0; --l) {
            Array<u64> candidates = searchLayer(currObj, vec, efConstruction, l);
            u32 maxM = (l == 0) ? M0 : M;
            for (usz i = 0; i < std::min((usz)maxM, candidates.size()); ++i) {
                u64 nId = candidates[i];
                node->neighbors[l].push(nId);
                
                Node* nNode = fetchNode(nId);
                if (!nNode) continue;
                if (l >= (int)nNode->neighbors.size()) {
                    Array<Array<u64>> newNeighbors;
                    newNeighbors.allocate(l + 1);
                    for(usz k=0; k<nNode->neighbors.size(); ++k) newNeighbors[k] = nNode->neighbors[k];
                    currentMemoryBytes += (newNeighbors.size() - nNode->neighbors.size()) * sizeof(Array<u64>);
                    nNode->neighbors = newNeighbors;
                }
                nNode->neighbors[l].push(id);
                currentMemoryBytes += sizeof(u64);
                
                if (nNode->neighbors[l].size() > maxM) {
                    f32 maxD = -1.0f;
                    usz furthestIdx = 0;
                    for (usz k = 0; k < nNode->neighbors[l].size(); ++k) {
                        Node* tkNode = fetchNode(nNode->neighbors[l][k]);
                        if (!tkNode) continue;
                        f32 d = distance(nNode->vec.data(), tkNode->vec.data());
                        if (d > maxD) { maxD = d; furthestIdx = k; }
                    }
                    Array<u64> pruned;
                    for(usz k=0; k<nNode->neighbors[l].size(); ++k) {
                        if (k != furthestIdx) pruned.push(nNode->neighbors[l][k]);
                    }
                    nNode->neighbors[l] = pruned;
                    currentMemoryBytes -= sizeof(u64);
                }
            }
            currObj = candidates[0]; 
        }
        
        if (level > maxLevel) {
            maxLevel = level;
            entryPoint = id;
        }
    }

    Array<u64> search(const f32* query, int k) {
        if (maxLevel == -1) return Array<u64>();
        
        u64 currObj = entryPoint;
        for (int l = maxLevel; l > 0; --l) {
            Node* cNode = fetchNode(currObj);
            if (!cNode) break;
            f32 currDist = distance(query, cNode->vec.data());
            bool changed = true;
            while (changed) {
                changed = false;
                if (l >= (int)cNode->neighbors.size()) break;
                for (u64 neighborId : cNode->neighbors[l]) {
                    Node* nNode = fetchNode(neighborId);
                    if (!nNode) continue;
                    f32 d = distance(query, nNode->vec.data());
                    if (d < currDist) {
                        currDist = d;
                        currObj = neighborId;
                        cNode = fetchNode(currObj);
                        changed = true;
                    }
                }
            }
        }
        
        return searchLayer(currObj, query, std::max((u32)efConstruction, (u32)k), 0);
    }
};

} // namespace Xylem

#endif
