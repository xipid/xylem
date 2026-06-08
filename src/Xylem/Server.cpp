#include <Xylem/Server.hpp>
#include <Xylem/QueryParser.hpp>
#include <Encoding/Yaml.hpp>
#include <cstdio>
#include <unistd.h>

namespace Xylem {

using namespace Collection;
using namespace Rho;

XylemServer::XylemServer(XylemEngine& eng, std::mutex* mtx) : engine(eng), engineMutex(mtx) {}

bool XylemServer::hasAnyPermissions() {
    if (permsCacheChecked) return permsCacheResult;
    Array<String> cols; cols.push("id");
    Array<Clauses> clauses;
    clauses.push(WHERE("name", "path", "/perms/**"));
    auto rows = engine.read(cols, clauses, 1);
    permsCacheResult = (rows.size() > 0);
    permsCacheChecked = true;
    return permsCacheResult;
}

XylemServer::~XylemServer() {
    server.destroy();
}

void XylemServer::hook(Lines::Bind& bind) {
    server.hook(bind);
    
    server.onUpgrade([this](Packet firstPkt, Tunnel& tunnel, Cart cart) {
        std::unique_lock<std::mutex> lockVal;
        if (engineMutex) lockVal = std::unique_lock<std::mutex>(*engineMutex);
        
        // printf("[SERVER] Client upgraded, tunnel=%p\n", &tunnel);
        String clientPubKey;
        String clientHash;
        
        if (const String* proofed = cart.meta.get(Meta::Proofed)) {
            Array<String> keys = Security::parseProofed(*proofed, tunnel.ephemeralKeypair.secretKey);
            if (keys.size() > 0) {
                clientPubKey = keys[0];
                clientHash = Security::hash(clientPubKey, 8);
            }
        }
        
        clientIdentities.set(&tunnel, clientHash);
        clientPubKeys.set(&tunnel, clientPubKey);
        
        if (!firstPkt.payload.isEmpty()) {
            handlePacket(firstPkt, tunnel);
        }
    });

    server.onPacket([this](Packet p, Tunnel& tunnel, Cart cart) {
        std::unique_lock<std::mutex> lockVal;
        if (engineMutex) lockVal = std::unique_lock<std::mutex>(*engineMutex);
        handlePacket(p, tunnel);
    });

    server.onDisconnect([this](Map<u64, String> reason, Tunnel& tunnel, Cart cart) {
        std::unique_lock<std::mutex> lockVal;
        if (engineMutex) lockVal = std::unique_lock<std::mutex>(*engineMutex);
        clientIdentities.remove(&tunnel);
        clientPubKeys.remove(&tunnel);
    });
}

void XylemServer::update() {
    server.update();
}

// ─── Permission Checking ─────────────────────────────────────────────────────

static void writeHierarchical(XylemEngine& engine, const String& path, const Map<String, String>& extraCols) {
    String cleanPath = path;
    if (cleanPath.startsWith("/")) {
        cleanPath = cleanPath.slice(1);
    }
    Array<String> parts = cleanPath.split("/");
    Array<String> cleanParts;
    for (usz i = 0; i < parts.size(); ++i) {
        if (!parts[i].isEmpty()) cleanParts.push(parts[i]);
    }
    if (cleanParts.size() == 0) return;
    
    String currentParentId = "";
    String currentPathStr = "";
    
    Array<String> readCols; readCols.push("id");
    
    // Traverse down the directories and create them if missing
    for (usz i = 0; i < cleanParts.size() - 1; ++i) {
        Array<Clauses> dirClauses;
        dirClauses.push(WHERE("name", "=", cleanParts[i]) && WHERE("parent_id", "=", currentParentId));
        auto dirRows = engine.read(readCols, dirClauses);
        
        if (dirRows.size() > 0 && dirRows[0].has("id")) {
            currentParentId = *dirRows[0].get("id");
        } else {
            u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
            String newId(rnd);
            
            Array<Clause> dirCols;
            dirCols.push({"name", "=", cleanParts[i]});
            dirCols.push({"parent_id", "=", currentParentId});
            dirCols.push({"id", "=", newId});
            dirCols.push({"type", "=", "dir"});
            dirCols.push({"perms", "=", "755"});
            
            engine.write(dirCols);
            currentParentId = newId;
        }
    }
    
    // Write the leaf item
    u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
    String fileId(rnd);
    
    Array<Clause> fileCols;
    fileCols.push({"name", "=", cleanParts[cleanParts.size() - 1]});
    fileCols.push({"parent_id", "=", currentParentId});
    fileCols.push({"id", "=", fileId});
    
    for (auto it = extraCols.begin(); it != extraCols.end(); ++it) {
        fileCols.push({it->key, "=", it->value});
    }
    
    engine.write(fileCols);
}

String XylemServer::getPermPath(const String& action, const String& path) {
    String cleanPath = path;
    if (cleanPath.startsWith("/")) {
        cleanPath = cleanPath.slice(1);
    }
    return "/perms/" + action + "/" + cleanPath;
}

bool XylemServer::checkPermission(const String& clientHash, const String& action, const String& path) {
    if (clientHash.isEmpty()) return false;
    
    String permPath = getPermPath(action, path);
    
    Array<String> cols; cols.push("id");
    Array<Clauses> clauses;
    clauses.push(WHERE("name", "path", permPath) && WHERE("owner", "=", clientHash));
    
    auto rows = engine.read(cols, clauses, 1);
    return rows.size() > 0;
}

String XylemServer::getPathForId(const String& id) {
    if (id.isEmpty()) return "/";
    
    Array<String> cols; cols.push("name"); cols.push("parent_id");
    Array<Clauses> clauses; clauses.push(WHERE("id", "=", id));
    
    auto rows = engine.read(cols, clauses, 1);
    if (rows.size() == 0) return "";
    
    String name = rows[0].has("name") ? *rows[0].get("name") : "";
    String pId = rows[0].has("parent_id") ? *rows[0].get("parent_id") : "";
    
    if (pId.isEmpty()) {
        return "/" + name;
    }
    
    String parentPath = getPathForId(pId);
    if (parentPath == "/") return "/" + name;
    return parentPath + "/" + name;
}

String XylemServer::getPathForRow(const Map<String, String>& row) {
    String name = row.has("name") ? *row.get("name") : "";
    String pId = row.has("parent_id") ? *row.get("parent_id") : "";
    if (pId.isEmpty()) {
        return "/" + name;
    }
    String parentPath = getPathForId(pId);
    if (parentPath == "/") return "/" + name;
    return parentPath + "/" + name;
}

String XylemServer::getPathForRowId(u64 rId) {
    Map<String, String>* row = engine.tableStore->fetchRow(rId);
    if (!row) return "";
    return getPathForRow(*row);
}

bool XylemServer::checkReadPermForRows(const String& clientHash, Array<Map<String, String>>& rows) {
    // If no permission entries exist, allow all reads (open mode)
    if (!hasAnyPermissions()) {
        return true;
    }
    
    Array<Map<String, String>> filtered;
    for (usz i = 0; i < rows.size(); ++i) {
        String path = getPathForRow(rows[i]);
        if (path.isEmpty()) {
            filtered.push(rows[i]);
            continue;
        }
        
        bool isDirectOwner = (rows[i].has("owner") && (*rows[i].get("owner") == clientHash)) ||
                             (rows[i].has("owning") && (rows[i].get("owning")->indexOf(clientHash) >= 0));
        bool isOwner = isDirectOwner || checkPermission(clientHash, "owning", path);
        bool hasReadMe = checkPermission(clientHash, "read/me", path);
        bool hasReadAll = checkPermission(clientHash, "read/*", path);
        bool allowed = (isOwner && hasReadMe) || hasReadAll;
        
        if (allowed) {
            filtered.push(rows[i]);
        }
    }
    rows = filtered;
    return true;
}

String XylemServer::getClientPubKey(const String& clientHash) {
    for (auto it = clientIdentities.begin(); it != clientIdentities.end(); ++it) {
        if (it->value == clientHash) {
            if (clientPubKeys.has(it->key)) {
                return *clientPubKeys.get(it->key);
            }
        }
    }
    return "";
}

static String hexToBin(const String& hex) {
    String bin;
    bin.allocate(hex.size() / 2);
    for (usz i = 0; i < bin.size(); ++i) {
        char high = hex[2 * i];
        char low = hex[2 * i + 1];
        auto hVal = (high >= 'a' && high <= 'f') ? (high - 'a' + 10) :
                    (high >= 'A' && high <= 'F') ? (high - 'A' + 10) : (high - '0');
        auto lVal = (low >= 'a' && low <= 'f') ? (low - 'a' + 10) :
                    (low >= 'A' && low <= 'F') ? (low - 'A' + 10) : (low - '0');
        bin.data()[i] = (hVal << 4) | lVal;
    }
    return bin;
}

static String serializeRowForSigning(const Array<Clause>& columns) {
    Array<Clause> sorted = columns;
    for (usz i = 0; i < sorted.size(); ++i) {
        for (usz j = i + 1; j < sorted.size(); ++j) {
            if (sorted[j].col < sorted[i].col) {
                Clause tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    
    String res;
    for (usz i = 0; i < sorted.size(); ++i) {
        if (sorted[i].col == "owning") continue;
        if (!res.isEmpty()) res += ";";
        res += sorted[i].col + "=" + sorted[i].val;
    }
    return res;
}

bool XylemServer::checkWritePerm(const String& clientHash, const Array<Clause>& columns, const Array<Clauses>& clauses) {
    // Verify owning signature if present in columns
    String owningVal;
    for (const auto& col : columns) {
        if (col.col == "owning") {
            owningVal = col.val;
            break;
        }
    }
    
    if (!owningVal.isEmpty()) {
        if (owningVal.size() != 144) {
            return false;
        }
        
        String owningHash = owningVal.slice(0, 16);
        String owningSigHex = owningVal.slice(16, 144);
        
        String clientHashHex;
        for (usz i = 0; i < clientHash.size(); ++i) {
            char buf[3];
            sprintf(buf, "%02x", (unsigned char)clientHash[i]);
            clientHashHex += buf;
        }
        
        if (owningHash != clientHashHex) {
            return false;
        }
        
        String clientPubKey = getClientPubKey(clientHash);
        if (clientPubKey.isEmpty() || clientPubKey.size() != 32) {
            return false;
        }
        
        String serialized = serializeRowForSigning(columns);
        String binarySig = hexToBin(owningSigHex);
        
        if (!Security::verifyX(clientPubKey, serialized, binarySig)) {
            return false;
        }
    }

    // If no permission entries exist, allow all writes (open mode)
    if (!hasAnyPermissions()) return true;
    
    if (clauses.size() == 0) {
        // CREATE operation
        String name;
        String pId;
        for (const auto& col : columns) {
            if (col.col == "name") name = col.val;
            if (col.col == "parent_id") pId = col.val;
        }
        
        String parentPath = getPathForId(pId);
        String targetPath = parentPath == "/" ? "/" + name : parentPath + "/" + name;
        if (targetPath.startsWith("//")) {
            targetPath = targetPath.substring(1);
        }
        
        if (targetPath.startsWith("/perms/")) {
            if (targetPath.startsWith("/perms/owning/")) {
                // Must have own.* permission
                String filePath = targetPath.substring(14);
                if (!filePath.startsWith("/")) filePath = "/" + filePath;
                
                bool ownAll = checkPermission(clientHash, "own/*", filePath);
                bool ownMe = checkPermission(clientHash, "own/me", filePath);
                
                String ownerVal;
                for (const auto& col : columns) {
                    if (col.col == "owner") ownerVal = col.val;
                }
                
                if (ownerVal == clientHash) {
                    return ownMe || ownAll;
                } else {
                    return ownAll;
                }
            } else {
                // Modifying custom permissions requires direct create permission on the perm path
                return checkPermission(clientHash, "create", targetPath);
            }
        }
        
        return checkPermission(clientHash, "create", targetPath);
    } else {
        // MODIFY operation
        if (!engine.isMounted()) return false;
        Array<u64> matchedIds = engine.tableStore->getMatchingRowIds(clauses, engine.tableStore->currentSeq, 0);
        for (usz i = 0; i < matchedIds.size(); ++i) {
            String filePath = getPathForRowId(matchedIds[i]);
            if (filePath.isEmpty()) continue;
            
            if (filePath.startsWith("/perms/")) {
                if (filePath.startsWith("/perms/owning/")) {
                    return false; // Direct modification to owning perms is forbidden
                }
                if (!checkPermission(clientHash, "create", filePath)) return false;
            } else {
                Map<String, String>* row = engine.tableStore->fetchRow(matchedIds[i]);
                bool isOwner = false;
                if (row) {
                    if (row->has("owning")) {
                        isOwner = (row->get("owning")->indexOf(clientHash) >= 0);
                    } else if (row->has("owner")) {
                        isOwner = (*row->get("owner") == clientHash);
                    }
                }
                if (!isOwner) {
                    isOwner = checkPermission(clientHash, "owning", filePath);
                }
                bool allowed = (isOwner && checkPermission(clientHash, "modify/me", filePath)) ||
                               checkPermission(clientHash, "modify/*", filePath);
                if (!allowed) return false;
            }
        }
        return true;
    }
}

bool XylemServer::checkRmPerm(const String& clientHash, const Array<Clauses>& clauses) {
    // If no permission entries exist, allow all deletes (open mode)
    if (!hasAnyPermissions()) return true;
    
    if (!engine.isMounted()) return false;
    Array<u64> matchedIds = engine.tableStore->getMatchingRowIds(clauses, engine.tableStore->currentSeq, 0);
    for (usz i = 0; i < matchedIds.size(); ++i) {
        String filePath = getPathForRowId(matchedIds[i]);
        if (filePath.isEmpty()) continue;
        
        if (filePath.startsWith("/perms/")) {
            if (filePath.startsWith("/perms/owning/")) {
                // This is a disown operation
                // We must query the row to see the owner hash
                Map<String, String>* checkRow = engine.tableStore->fetchRow(matchedIds[i]);
                if (checkRow && checkRow->has("owner")) {
                    String ownerHash = *checkRow->get("owner");
                    String targetFilePath = filePath.substring(14);
                    if (!targetFilePath.startsWith("/")) targetFilePath = "/" + targetFilePath;
                    
                    if (ownerHash == clientHash) {
                        if (!checkPermission(clientHash, "disown/me", targetFilePath)) return false;
                    } else {
                        if (!checkPermission(clientHash, "disown/*", targetFilePath)) return false;
                    }
                }
            } else {
                // Must have rm permission on the perm path
                if (!checkPermission(clientHash, "unlink/*", filePath) && !checkPermission(clientHash, "rm/*", filePath)) return false;
            }
            Map<String, String>* row = engine.tableStore->fetchRow(matchedIds[i]);
            bool isOwner = false;
            if (row) {
                if (row->has("owning")) {
                    isOwner = (row->get("owning")->indexOf(clientHash) >= 0);
                } else if (row->has("owner")) {
                    isOwner = (*row->get("owner") == clientHash);
                }
            }
            if (!isOwner) {
                isOwner = checkPermission(clientHash, "owning", filePath);
            }
            bool allowed = (isOwner && (checkPermission(clientHash, "unlink/me", filePath) || checkPermission(clientHash, "rm/me", filePath))) ||
                           checkPermission(clientHash, "unlink/*", filePath) || checkPermission(clientHash, "rm/*", filePath);
            if (!allowed) return false;
        }
    }
    return true;
}

// ─── Request Handler ─────────────────────────────────────────────────────────

static void filterTreeRecursive(const String& clientHash, XylemServer* srv, TreeBranch* branch) {
    if (!branch) return;
    // If no permissions exist, skip filtering
    if (!srv->hasAnyPermissions()) return;
    
    Array<TreeItem*> filtered;
    for (usz i = 0; i < branch->size(); ++i) {
        TreeItem* child = (*branch)[i];
        if (RowNode* rn = dynamic_cast<RowNode*>(child)) {
            String path = srv->getPathForRow(rn->row);
            bool isOwner = srv->checkPermission(clientHash, "owning", path);
            bool allowed = (isOwner && srv->checkPermission(clientHash, "read/me", path)) ||
                           srv->checkPermission(clientHash, "read/*", path);
            if (allowed) {
                filtered.push(child);
                filterTreeRecursive(clientHash, srv, rn);
            } else {
                delete child;
            }
        } else if (TreeBranch* tb = dynamic_cast<TreeBranch*>(child)) {
            filtered.push(child);
            filterTreeRecursive(clientHash, srv, tb);
        } else {
            filtered.push(child);
        }
    }
    branch->clear();
    for (usz i = 0; i < filtered.size(); ++i) {
        branch->add(filtered[i]);
    }
}

void XylemServer::handlePacket(const Packet& p, Tunnel& tunnel) {
    Map<String, String> req = Map<String, String>::deserialize(p.payload);
    if (!req.has("_cmd")) return;
    
    String cmd = *req.get("_cmd");
    String clientHash = clientIdentities.has(&tunnel) ? *clientIdentities.get(&tunnel) : "";
    
    // Invalidate permission cache on write operations
    if (cmd == "write" || cmd == "writeVolatile" || cmd == "rm" || cmd == "query") {
        permsCacheChecked = false;
    }
    
    Map<String, String> resp;
    resp.set("_status", "error");
    resp.set("_code", "-1");
    
    if (cmd == "query") {
        String queryStr = req.has("query") ? *req.get("query") : "";
        printf("[SERVER] query received: '%s'\n", queryStr.c_str());
        Array<String> tokens = QueryParser::tokenize(queryStr, Array<String>());
        if (tokens.size() > 0) {
            String baseCmd = tokens[0].toUpperCase();
            bool isRead = (baseCmd == "READ" || baseCmd == "READ*" || baseCmd == "LS" || baseCmd == "CAT");
            printf("[SERVER] baseCmd: %s, isRead: %d\n", baseCmd.c_str(), isRead);
            
            if (isRead) {
                QueryResult qres = engine.query(queryStr);
                printf("[SERVER] qres.code: %d, readRows.size(): %d\n", qres.code, (int)qres.readRows.size());
                if (qres.code >= 0) {
                    String clientHashHex;
                    for (usz i = 0; i < clientHash.size(); ++i) {
                        char buf[3];
                        sprintf(buf, "%02x", (unsigned char)clientHash[i]);
                        clientHashHex += buf;
                    }
                    printf("[SERVER] clientHashHex: '%s' (size %d), hasAnyPerms: %d\n", 
                           clientHashHex.c_str(), (int)clientHash.size(), (int)hasAnyPermissions());
                    
                    checkReadPermForRows(clientHash, qres.readRows);
                    printf("[SERVER] after checkReadPermForRows, readRows.size(): %d\n", (int)qres.readRows.size());
                    
                    if (qres.treeResult) {
                        filterTreeRecursive(clientHash, this, qres.treeResult);
                        resp.set("treeResult", Encoding::toYAML(*qres.treeResult));
                        delete qres.treeResult;
                    }
                    resp.set("_status", "ok");
                    resp.set("_code", String::from((long long)qres.code));
                    
                    Map<String, String> rowsMap;
                    rowsMap.set("size", String::from((long long)qres.readRows.size()));
                    for (usz i = 0; i < qres.readRows.size(); i++) {
                        rowsMap.set(String::from((long long)i), qres.readRows[i].serialize());
                    }
                    resp.set("readRows", rowsMap.serialize());
                }
            } else if (baseCmd == "WRITE" || baseCmd == "WRITEVOLATILE") {
                // Parse write columns and clauses
                Array<Clause> writeCols;
                Array<Clauses> queryClauses;
                usz idx = 1;
                while (idx < tokens.size()) {
                    String t = tokens[idx];
                    String tu = t.toUpperCase();
                    if (tu == "WHERE" || tu == "ASSERT" || tu == "FOLLOW" || tu == "REPEATFOLLOW" || tu == "LIMIT" || tu == "PAGE") break;
                    
                    bool isThreeToken = false;
                    if (idx + 2 < tokens.size() && tokens[idx+1] == "=") {
                        writeCols.push({tokens[idx], "=", tokens[idx+2]});
                        idx += 2;
                        isThreeToken = true;
                    }
                    if (!isThreeToken) {
                        // fallback simple parse
                        long long eqPos = t.indexOf("=");
                        if (eqPos >= 0) {
                            writeCols.push({t.slice(0, eqPos), "=", t.slice(eqPos + 1)});
                        }
                    }
                    idx++;
                }
                
                // Parse clauses
                while (idx < tokens.size()) {
                    String t = tokens[idx];
                    if (t.toUpperCase() == "WHERE" || t.toUpperCase() == "ASSERT") {
                        Clauses group;
                        group.isAssert = (t.toUpperCase() == "ASSERT");
                        idx++;
                        while (idx < tokens.size()) {
                            String cStr = tokens[idx];
                            if (cStr.toUpperCase() == "WHERE" || cStr.toUpperCase() == "ASSERT" || cStr.toUpperCase() == "LIMIT" || cStr.toUpperCase() == "PAGE") {
                                idx--; break;
                            }
                            long long eqPos = cStr.indexOf("=");
                            if (eqPos >= 0) {
                                group.push({cStr.slice(0, eqPos), "=", cStr.slice(eqPos + 1)});
                            }
                            idx++;
                        }
                        queryClauses.push(group);
                    }
                    idx++;
                }
                
                if (checkWritePerm(clientHash, writeCols, queryClauses)) {
                    QueryResult qres = engine.query(queryStr);
                    if (qres.code == 0) {
                        resp.set("_status", "ok");
                        resp.set("_code", "0");
                        
                        // Apply ownership registration
                        if (queryClauses.size() == 0) {
                            String name;
                            String pId;
                            for (const auto& col : writeCols) {
                                if (col.col == "name") name = col.val;
                                if (col.col == "parent_id") pId = col.val;
                            }
                            String parentPath = getPathForId(pId);
                            String targetPath = parentPath == "/" ? "/" + name : parentPath + "/" + name;
                            if (targetPath.startsWith("//")) {
                                targetPath = targetPath.substring(1);
                            }
                            
                            if (!targetPath.startsWith("/perms/")) {
                                Map<String, String> extra; extra.set("owner", clientHash);
                                writeHierarchical(engine, getPermPath("owning", targetPath), extra);
                            }
                        } else {
                            Array<u64> matchedIds = engine.tableStore->getMatchingRowIds(queryClauses, engine.tableStore->currentSeq, 0);
                            for (usz i = 0; i < matchedIds.size(); ++i) {
                                String filePath = getPathForRowId(matchedIds[i]);
                                if (filePath.isEmpty() || filePath.startsWith("/perms/")) continue;
                                
                                // disown others
                                Array<Clauses> rmClauses;
                                rmClauses.push(WHERE("name", "path", getPermPath("owning", filePath)) && WHERE("owner", "!=", clientHash));
                                engine.rm(rmClauses);
                                
                                // register modifier as owner
                                Array<String> cols; cols.push("id");
                                Array<Clauses> checkC; checkC.push(WHERE("name", "path", getPermPath("owning", filePath)) && WHERE("owner", "=", clientHash));
                                auto rows = engine.read(cols, checkC, 1);
                                if (rows.size() == 0) {
                                    Map<String, String> extra; extra.set("owner", clientHash);
                                    writeHierarchical(engine, getPermPath("owning", filePath), extra);
                                }
                            }
                        }
                    } else {
                        resp.set("_code", String::from((long long)qres.code));
                    }
                } else {
                    resp.set("_status", "error");
                    resp.set("_code", "-1"); // unauthorized
                }
            } else if (baseCmd == "RM") {
                // Parse clauses
                Array<Clauses> queryClauses;
                if (tokens.size() > 1 && (tokens[1].startsWith("/") || tokens[1].startsWith("./") || tokens[1] == ".")) {
                    queryClauses.push(WHERE("name", "path", tokens[1] + "/**"));
                }
                
                if (checkRmPerm(clientHash, queryClauses)) {
                    QueryResult qres = engine.query(queryStr);
                    if (qres.code == 0) {
                        resp.set("_status", "ok");
                        resp.set("_code", "0");
                        
                        // Delete ownership rows
                        if (queryClauses.size() > 0) {
                            Array<u64> matchedIds = engine.tableStore->getMatchingRowIds(queryClauses, engine.tableStore->currentSeq, 0);
                            for (usz i = 0; i < matchedIds.size(); ++i) {
                                String filePath = getPathForRowId(matchedIds[i]);
                                if (filePath.isEmpty()) continue;
                                Array<Clauses> rmClauses;
                                rmClauses.push(WHERE("name", "path", getPermPath("owning", filePath)));
                                engine.rm(rmClauses);
                            }
                        }
                    } else {
                        resp.set("_code", String::from((long long)qres.code));
                    }
                } else {
                    resp.set("_status", "error");
                    resp.set("_code", "-1"); // unauthorized
                }
            } else {
                // Format, Mount, etc. are only local
                resp.set("_code", "-99");
            }
        }
    } else if (cmd == "read") {
        // Deserialize arguments
        Array<String> columns;
        if (req.has("columns")) {
            Map<String, String> colsMap = Map<String, String>::deserialize(*req.get("columns"));
            usz sz = (usz)parseLong(*colsMap.get("size"));
            for (usz i = 0; i < sz; i++) columns.push(*colsMap.get(String::from((long long)i)));
        }
        
        Array<Clauses> clauses;
        if (req.has("clauses")) {
            Map<String, String> clMap = Map<String, String>::deserialize(*req.get("clauses"));
            usz sz = (usz)parseLong(*clMap.get("size"));
            for (usz i = 0; i < sz; i++) {
                Map<String, String> gMap = Map<String, String>::deserialize(*clMap.get(String::from((long long)i)));
                Clauses group;
                group.isAssert = gMap.has("isAssert") && (*gMap.get("isAssert") == "true");
                usz groupSz = (usz)parseLong(*gMap.get("size"));
                for (usz j = 0; j < groupSz; j++) {
                    Map<String, String> cMap = Map<String, String>::deserialize(*gMap.get(String::from((long long)j)));
                    group.push({*cMap.get("col"), *cMap.get("op"), *cMap.get("val")});
                }
                clauses.push(group);
            }
        }
        
        u64 length = req.has("length") ? (u64)strtoull(req.get("length")->c_str(), nullptr, 0) : 0;
        u64 page = req.has("page") ? (u64)strtoull(req.get("page")->c_str(), nullptr, 0) : 0;
        bool readAll = req.has("readAllColumns") && (*req.get("readAllColumns") == "true");
        u64 txId = req.has("txId") ? (u64)strtoull(req.get("txId")->c_str(), nullptr, 0) : 0;
        
        auto rows = engine.read(columns, clauses, length, page, false, txId, readAll);
        checkReadPermForRows(clientHash, rows);
        
        resp.set("_status", "ok");
        resp.set("_code", "0");
        Map<String, String> rowsMap;
        rowsMap.set("size", String::from((long long)rows.size()));
        for (usz i = 0; i < rows.size(); i++) {
            rowsMap.set(String::from((long long)i), rows[i].serialize());
        }
        resp.set("rows", rowsMap.serialize());
    } else if (cmd == "write" || cmd == "writeVolatile") {
        Array<Clause> columns;
        if (req.has("columns")) {
            Map<String, String> colsMap = Map<String, String>::deserialize(*req.get("columns"));
            usz sz = (usz)parseLong(*colsMap.get("size"));
            for (usz i = 0; i < sz; i++) {
                Map<String, String> cMap = Map<String, String>::deserialize(*colsMap.get(String::from((long long)i)));
                columns.push({*cMap.get("col"), *cMap.get("op"), *cMap.get("val")});
            }
        }
        
        Array<Clauses> clauses;
        if (req.has("clauses")) {
            Map<String, String> clMap = Map<String, String>::deserialize(*req.get("clauses"));
            usz sz = (usz)parseLong(*clMap.get("size"));
            for (usz i = 0; i < sz; i++) {
                Map<String, String> gMap = Map<String, String>::deserialize(*clMap.get(String::from((long long)i)));
                Clauses group;
                group.isAssert = gMap.has("isAssert") && (*gMap.get("isAssert") == "true");
                usz groupSz = (usz)parseLong(*gMap.get("size"));
                for (usz j = 0; j < groupSz; j++) {
                    Map<String, String> cMap = Map<String, String>::deserialize(*gMap.get(String::from((long long)j)));
                    group.push({*cMap.get("col"), *cMap.get("op"), *cMap.get("val")});
                }
                clauses.push(group);
            }
        }
        u64 txId = req.has("txId") ? (u64)strtoull(req.get("txId")->c_str(), nullptr, 0) : 0;
        
        if (checkWritePerm(clientHash, columns, clauses)) {
            // Apply write: handle generated ID if any
            for (usz i = 0; i < columns.size(); ++i) {
                if (columns[i].col.endsWith(":generate")) {
                    String baseCol = columns[i].col.substring(0, columns[i].col.size() - 9);
                    columns[i].col = baseCol;
                    columns[i].val = engine.generateId(baseCol);
                }
            }
            
            int code = -1;
            bool isPermInsert = false;
            if (clauses.size() == 0 && cmd == "write") {
                String name;
                for (const auto& col : columns) {
                    if (col.col == "name") name = col.val;
                }
                if (name.startsWith("/perms/")) {
                    isPermInsert = true;
                    Map<String, String> extra;
                    for (const auto& col : columns) {
                        if (col.col != "name" && col.col != "parent_id" && col.col != "id") {
                            extra.set(col.col, col.val);
                        }
                    }
                    writeHierarchical(engine, name, extra);
                    code = 0;
                }
            }
            if (!isPermInsert) {
                if (cmd == "write") {
                    code = engine.write(columns, clauses, txId);
                } else {
                    code = engine.writeVolatile(columns, clauses, txId);
                }
            }
            
            if (code == 0) {
                resp.set("_status", "ok");
                resp.set("_code", "0");
                
                // Ownership registration
                if (clauses.size() == 0) {
                    String name;
                    String pId;
                    for (const auto& col : columns) {
                        if (col.col == "name") name = col.val;
                        if (col.col == "parent_id") pId = col.val;
                    }
                    String parentPath = getPathForId(pId);
                    String targetPath = parentPath == "/" ? "/" + name : parentPath + "/" + name;
                    if (targetPath.startsWith("//")) {
                        targetPath = targetPath.substring(1);
                    }
                    
                    if (!targetPath.startsWith("/perms/")) {
                        Map<String, String> extra; extra.set("owner", clientHash);
                        writeHierarchical(engine, getPermPath("owning", targetPath), extra);
                    }
                } else {
                    Array<u64> matchedIds = engine.tableStore->getMatchingRowIds(clauses, engine.tableStore->currentSeq, txId);
                    for (usz i = 0; i < matchedIds.size(); ++i) {
                        String filePath = getPathForRowId(matchedIds[i]);
                        if (filePath.isEmpty() || filePath.startsWith("/perms/")) continue;
                        
                        Array<Clauses> rmClauses;
                        rmClauses.push(WHERE("name", "path", getPermPath("owning", filePath)) && WHERE("owner", "!=", clientHash));
                        engine.rm(rmClauses);
                        
                        Array<String> cols; cols.push("id");
                        Array<Clauses> checkC; checkC.push(WHERE("name", "path", getPermPath("owning", filePath)) && WHERE("owner", "=", clientHash));
                        auto rows = engine.read(cols, checkC, 1);
                        if (rows.size() == 0) {
                            Map<String, String> extra; extra.set("owner", clientHash);
                            writeHierarchical(engine, getPermPath("owning", filePath), extra);
                        }
                    }
                }
            } else {
                resp.set("_code", String::from((long long)code));
            }
        } else {
            resp.set("_status", "error");
            resp.set("_code", "-1");
        }
    } else if (cmd == "rm") {
        Array<Clauses> clauses;
        if (req.has("clauses")) {
            Map<String, String> clMap = Map<String, String>::deserialize(*req.get("clauses"));
            usz sz = (usz)parseLong(*clMap.get("size"));
            for (usz i = 0; i < sz; i++) {
                Map<String, String> gMap = Map<String, String>::deserialize(*clMap.get(String::from((long long)i)));
                Clauses group;
                group.isAssert = gMap.has("isAssert") && (*gMap.get("isAssert") == "true");
                usz groupSz = (usz)parseLong(*gMap.get("size"));
                for (usz j = 0; j < groupSz; j++) {
                    Map<String, String> cMap = Map<String, String>::deserialize(*gMap.get(String::from((long long)j)));
                    group.push({*cMap.get("col"), *cMap.get("op"), *cMap.get("val")});
                }
                clauses.push(group);
            }
        }
        u64 length = req.has("length") ? (u64)strtoull(req.get("length")->c_str(), nullptr, 0) : 0;
        u64 as = req.has("as") ? (u64)strtoull(req.get("as")->c_str(), nullptr, 0) : 0;
        
        if (checkRmPerm(clientHash, clauses)) {
            bool ok = engine.rm(clauses, length, as);
            if (ok) {
                resp.set("_status", "ok");
                resp.set("_code", "0");
                
                // Delete ownership rows
                Array<u64> matchedIds = engine.tableStore->getMatchingRowIds(clauses, engine.tableStore->currentSeq, as);
                for (usz i = 0; i < matchedIds.size(); ++i) {
                    String filePath = getPathForRowId(matchedIds[i]);
                    if (filePath.isEmpty()) continue;
                    Array<Clauses> rmClauses;
                    rmClauses.push(WHERE("name", "path", getPermPath("owning", filePath)));
                    engine.rm(rmClauses);
                }
            } else {
                resp.set("_code", "-1");
            }
        } else {
            resp.set("_status", "error");
            resp.set("_code", "-1");
        }
    } else if (cmd == "lock") {
        u64 id = req.has("id") ? (u64)strtoull(req.get("id")->c_str(), nullptr, 0) : 0;
        bool reqExp = req.has("requiresExplicitAs") && (*req.get("requiresExplicitAs") == "true");
        u64 txId = engine.lock(Array<Clauses>(), id, reqExp);
        resp.set("_status", "ok");
        resp.set("_code", "0");
        resp.set("txId", String::from((long long)txId));
    } else if (cmd == "commit") {
        u64 txId = engine.commit();
        resp.set("_status", "ok");
        resp.set("_code", "0");
        resp.set("txId", String::from((long long)txId));
    } else if (cmd == "rollback") {
        u64 id = req.has("id") ? (u64)strtoull(req.get("id")->c_str(), nullptr, 0) : 0;
        bool ok = engine.rollback(id);
        resp.set("_status", ok ? "ok" : "error");
        resp.set("_code", ok ? "0" : "-1");
    } else if (cmd == "unlock") {
        u64 id = req.has("id") ? (u64)strtoull(req.get("id")->c_str(), nullptr, 0) : 0;
        int code = engine.unlock(id);
        resp.set("_status", code == 0 ? "ok" : "error");
        resp.set("_code", String::from((long long)code));
    } else if (cmd == "watch") {
        // Watches are registered
        Array<Clauses> clauses;
        if (req.has("clauses")) {
            Map<String, String> clMap = Map<String, String>::deserialize(*req.get("clauses"));
            usz sz = (usz)parseLong(*clMap.get("size"));
            for (usz i = 0; i < sz; i++) {
                Map<String, String> gMap = Map<String, String>::deserialize(*clMap.get(String::from((long long)i)));
                Clauses group;
                group.isAssert = gMap.has("isAssert") && (*gMap.get("isAssert") == "true");
                usz groupSz = (usz)parseLong(*gMap.get("size"));
                for (usz j = 0; j < groupSz; j++) {
                    Map<String, String> cMap = Map<String, String>::deserialize(*gMap.get(String::from((long long)j)));
                    group.push({*cMap.get("col"), *cMap.get("op"), *cMap.get("val")});
                }
                clauses.push(group);
            }
        }
        
        // Simple permission check: must have watch permission
        // Since watch permissions can be path-based, let's look for name/path clause
        String targetPath = "/";
        for (const auto& group : clauses) {
            for (const auto& c : group) {
                if (c.col == "name" && c.op == "path") targetPath = c.val;
            }
        }
        
        if (checkPermission(clientHash, "watch", targetPath)) {
            u64 watchId = engine.watch(clauses);
            resp.set("_status", "ok");
            resp.set("_code", "0");
            resp.set("watchId", String::from((long long)watchId));
        } else {
            resp.set("_status", "error");
            resp.set("_code", "-1");
        }
    } else if (cmd == "unwatch") {
        u64 id = req.has("id") ? (u64)strtoull(req.get("id")->c_str(), nullptr, 0) : 0;
        bool ok = engine.unwatch(id);
        resp.set("_status", ok ? "ok" : "error");
        resp.set("_code", ok ? "0" : "-1");
    } else if (cmd == "pull") {
        u64 id = req.has("id") ? (u64)strtoull(req.get("id")->c_str(), nullptr, 0) : 0;
        auto rows = engine.pull(id);
        checkReadPermForRows(clientHash, rows);
        
        resp.set("_status", "ok");
        resp.set("_code", "0");
        Map<String, String> rowsMap;
        rowsMap.set("size", String::from((long long)rows.size()));
        for (usz i = 0; i < rows.size(); i++) {
            rowsMap.set(String::from((long long)i), rows[i].serialize());
        }
        resp.set("rows", rowsMap.serialize());
    } else if (cmd == "cat") {
        String path = req.has("path") ? *req.get("path") : "";
        u64 start = req.has("start") ? (u64)strtoull(req.get("start")->c_str(), nullptr, 0) : 0;
        u64 end = req.has("end") ? (u64)strtoull(req.get("end")->c_str(), nullptr, 0) : 0;
        
        bool isOwner = checkPermission(clientHash, "owning", path);
        bool allowed = (isOwner && checkPermission(clientHash, "read/me", path)) ||
                       checkPermission(clientHash, "read/*", path);
        if (allowed) {
            QueryResult qres = engine.cat(path, start, end);
            resp.set("_status", qres.code == 0 ? "ok" : "error");
            resp.set("_code", String::from((long long)qres.code));
            if (qres.code == 0 && qres.readRows.size() > 0) {
                resp.set("content", *qres.readRows[0].get("content"));
            }
        }
    } else if (cmd == "tee") {
        String path = req.has("path") ? *req.get("path") : "";
        String content = req.has("content") ? *req.get("content") : "";
        u64 start = req.has("start") ? (u64)strtoull(req.get("start")->c_str(), nullptr, 0) : 0;
        u64 end = req.has("end") ? (u64)strtoull(req.get("end")->c_str(), nullptr, 0) : 0;
        
        bool isOwner = checkPermission(clientHash, "owning", path);
        bool allowed = (isOwner && checkPermission(clientHash, "modify/me", path)) ||
                       checkPermission(clientHash, "modify/*", path);
        if (allowed) {
            QueryResult qres = engine.tee(path, content, start, end);
            resp.set("_status", qres.code == 0 ? "ok" : "error");
            resp.set("_code", String::from((long long)qres.code));
        }
    } else if (cmd == "ls") {
        String path = req.has("path") ? *req.get("path") : "";
        QueryResult qres = engine.ls(path);
        checkReadPermForRows(clientHash, qres.readRows);
        
        resp.set("_status", "ok");
        resp.set("_code", String::from((long long)qres.code));
        Map<String, String> rowsMap;
        rowsMap.set("size", String::from((long long)qres.readRows.size()));
        for (usz i = 0; i < qres.readRows.size(); i++) {
            rowsMap.set(String::from((long long)i), qres.readRows[i].serialize());
        }
        resp.set("readRows", rowsMap.serialize());
    } else if (cmd == "cp" || cmd == "mv") {
        String src = req.has("src") ? *req.get("src") : "";
        String dst = req.has("dst") ? *req.get("dst") : "";
        
        bool isOwnerSrc = checkPermission(clientHash, "owning", src);
        bool readAllowedSrc = (isOwnerSrc && checkPermission(clientHash, "read/me", src)) ||
                              checkPermission(clientHash, "read/*", src);
                              
        bool createAllowedDst = checkPermission(clientHash, "create", dst);
        
        bool rmAllowedSrc = true;
        if (cmd == "mv") {
            rmAllowedSrc = (isOwnerSrc && (checkPermission(clientHash, "unlink/me", src) || checkPermission(clientHash, "rm/me", src))) ||
                           checkPermission(clientHash, "unlink/*", src) || checkPermission(clientHash, "rm/*", src);
        }
        
        if (readAllowedSrc && createAllowedDst && rmAllowedSrc) {
            QueryResult qres = (cmd == "cp") ? engine.cp(src, dst) : engine.mv(src, dst);
            resp.set("_status", qres.code == 0 ? "ok" : "error");
            resp.set("_code", String::from((long long)qres.code));
        }
    } else if (cmd == "generateId") {
        String column = req.has("column") ? *req.get("column") : "";
        String genId = engine.generateId(column);
        resp.set("_status", "ok");
        resp.set("_code", "0");
        resp.set("id", genId);
    }
    
    tunnel.push(resp.serialize(), p.channel);
    
    // Immediately flush the response to avoid waiting for the next update cycle
    if (tunnel.hookedStation && tunnel.readyToSend()) {
        String out = tunnel.flush();
        if (out.length() > 0) {
            Cart c;
            c.rail = tunnel.hookedRail;
            c.payload = out;
            c.isSecure = false;
            if (tunnel.hasHookedTarget) {
                c.target = tunnel.hookedTarget;
                c.isAddressed = true;
            } else if (tunnel.hasHookedTargetStr) {
                c.meta.put(Meta::Target, tunnel.hookedTargetStr);
                c.hasMeta = true;
            }
            tunnel.hookedStation->push(c);
        }
    }
}

} // namespace Xylem
