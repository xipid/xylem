#include <Xylem/QueryParser.hpp>
#include <Xylem/Xylem.hpp>
#include <cstdlib>

using namespace Collection;

namespace Xylem {

Array<String> QueryParser::tokenize(const String& query, const Array<String>& args) {
    Array<String> tokens;
    String current;
    bool inQuotes = false;
    char quoteChar = 0;
    bool escape = false;

    for (usz i = 0; i < query.size(); ++i) {
        char c = query[i];

        if (escape) {
            current += c;
            escape = false;
            continue;
        }

        if (c == '\\') {
            escape = true;
            continue;
        }

        if (!inQuotes && (c == '\'' || c == '"')) {
            inQuotes = true;
            quoteChar = c;
            continue;
        }

        if (inQuotes && c == quoteChar) {
            inQuotes = false;
            tokens.push(current);
            current = "";
            continue;
        }

        if (inQuotes) {
            current += c;
            continue;
        }

        // Comments
        if (c == '#') {
            while (i < query.size() && query[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < query.size() && query[i + 1] == '/') {
            while (i < query.size() && query[i] != '\n') ++i;
            continue;
        }

        // Arguments
        if (c == '%' && i + 1 < query.size() && query[i + 1] >= '1' && query[i + 1] <= '9') {
            int argIdx = query[i + 1] - '1';
            if (argIdx >= 0 && (usz)argIdx < args.size()) {
                current += args[argIdx];
            }
            ++i; // skip digit
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!current.isEmpty()) {
                tokens.push(current);
                current = "";
            }
            continue;
        }

        current += c;
    }

    if (!current.isEmpty()) {
        tokens.push(current);
    }

    return tokens;
}

Array<GraphOp> QueryParser::parseExtract(const String& pathStr) {
    Array<GraphOp> ops;
    if (pathStr.isEmpty()) return ops;

    // Remove leading/trailing quotes if passed raw (though tokenizer strips quotes normally)
    String p = pathStr;
    if (p.startsWith("\"") && p.endsWith("\"")) p = p.slice(1, p.size() - 1);

    Array<String> parts = p.split("/");
    Array<String> cleanParts;
    for (usz i = 0; i < parts.size(); ++i) {
        if (!parts[i].isEmpty()) cleanParts.push(parts[i]);
    }

    if (cleanParts.size() == 0) {
        // Root MATCH
        GraphOp match;
        match.type = GraphOpType::MATCH;
        Clause rootParent;
        rootParent.col = "parent_id";
        rootParent.op = "=";
        rootParent.val = "0";

        Clauses group;
        group.push(rootParent);
        match.query.push(group);
        ops.push(match);
        return ops;
    }

    // Root MATCH
    GraphOp match;
    match.type = GraphOpType::MATCH;
    Clause rootClause;
    rootClause.col = "name";
    rootClause.op = "=";
    rootClause.val = cleanParts[0];
    
    // As per user instructions, parent_id=0 means no parent (root)
    Clause rootParent;
    rootParent.col = "parent_id";
    rootParent.op = "=";
    rootParent.val = "0";

    Clauses group;
    group.push(rootClause);
    group.push(rootParent);
    match.query.push(group);
    ops.push(match);

    // Subsequent FOLLOWs
    for (usz i = 1; i < cleanParts.size(); ++i) {
        GraphOp follow;
        follow.type = GraphOpType::FOLLOW;
        
        Clause pId;
        pId.col = "parent_id";
        pId.op = "=";
        pId.val = "parent.id";
        
        Clause pName;
        pName.col = "name";
        pName.op = "=";
        pName.val = cleanParts[i];
        
        Clauses fGroup;
        fGroup.push(pId);
        fGroup.push(pName);
        follow.query.push(fGroup);
        ops.push(follow);
    }

    return ops;
}

static Clause parseClauseStr(const String& str) {
    Clause c;
    c.op = "=";
    // Look for operators
    Array<String> ops;
    ops.push("=="); ops.push("<="); ops.push(">="); ops.push("reg");
    ops.push("cos"); ops.push("hash"); ops.push("="); ops.push("<"); ops.push(">");
    for (const String& op : ops) {
        long long pos = str.indexOf(op);
        if (pos >= 0) {
            c.col = str.slice(0, pos);
            c.op = op;
            c.val = str.slice(pos + op.size(), str.size());
            return c;
        }
    }
    // Default fallback
    c.col = str;
    c.val = "";
    return c;
}

QueryResult QueryParser::execute(XylemEngine* engine, const String& queryStr, const Array<String>& args) {
    QueryResult res;
    Array<String> tokens = tokenize(queryStr, args);
    if (tokens.size() == 0) return res;

    String cmd = tokens[0].toUpperCase();

    if (cmd == "FORMAT") {
        res.code = engine->format() ? 0 : -1;
        return res;
    }
    if (cmd == "MOUNT") {
        res.code = engine->mount() ? 0 : -1;
        return res;
    }
    if (cmd == "DESTROY") {
        engine->destroy();
        res.code = 0;
        return res;
    }
    if (cmd == "MEMORY" && tokens.size() > 1) {
        String sizeStr = tokens[1];
        usz size = 0;
        if (sizeStr.endsWith("MB")) {
            size = (usz)std::atoll((const char*)sizeStr.slice(0, sizeStr.size() - 2).data()) * 1024 * 1024;
        } else if (sizeStr.endsWith("KB")) {
            size = (usz)std::atoll((const char*)sizeStr.slice(0, sizeStr.size() - 2).data()) * 1024;
        } else {
            size = (usz)std::atoll((const char*)sizeStr.data());
        }
        engine->maxCache = size;
        if (engine->tableStore) engine->tableStore->maxMemoryBytes = size;
        res.code = 0;
        return res;
    }

    // Parse range spec from command: CMD[x:y] or CMD[:y] or CMD[x:]
    auto parseCmdRange = [](const String& cmdStr) -> BlobRange {
        BlobRange r = {0, 0, false, false, false};
        long long bracket = -1;
        for (usz i = 0; i < cmdStr.size(); ++i) {
            if (cmdStr[i] == '[') { bracket = (long long)i; break; }
        }
        if (bracket < 0) return r;
        String rangeSpec = cmdStr.slice(bracket);
        return parseBlobRange(rangeSpec);
    };

    // Extract base command name without range spec
    auto baseCmdName = [](const String& cmdStr) -> String {
        for (usz i = 0; i < cmdStr.size(); ++i) {
            if (cmdStr[i] == '[') return cmdStr.slice(0, i);
        }
        return cmdStr;
    };

    String baseCmd = baseCmdName(tokens[0]).toUpperCase();
    BlobRange cmdRange = parseCmdRange(tokens[0]);

    // ─── CAT command ───
    if (baseCmd == "CAT") {
        if (tokens.size() < 2) { res.code = -1; return res; }
        String path = tokens[1];
        u64 start = cmdRange.valid ? cmdRange.start : 0;
        u64 end   = cmdRange.valid ? cmdRange.end : 0;

        // Check for additional WHERE/ASSERT/FOLLOW clauses
        // For now, delegate to engine->cat()
        res = engine->cat(path, start, end);
        return res;
    }

    // ─── TEE command ───
    if (baseCmd == "TEE") {
        if (tokens.size() < 3) { res.code = -1; return res; }
        String path = tokens[1];
        // Gather content from remaining tokens
        String content;
        for (usz i = 2; i < tokens.size(); ++i) {
            if (i > 2) content += " ";
            content += tokens[i];
        }
        u64 start = cmdRange.valid ? cmdRange.start : 0;
        u64 end   = cmdRange.valid ? cmdRange.end : 0;
        res = engine->tee(path, content, start, end);
        return res;
    }

    // ─── LS command ───
    if (baseCmd == "LS") {
        String path = tokens.size() >= 2 ? tokens[1] : "";
        res = engine->ls(path);
        return res;
    }

    // ─── UNLINK command ───
    if (baseCmd == "UNLINK") {
        if (tokens.size() < 2) { res.code = -1; return res; }
        String path = tokens[1];
        res.code = engine->unlink(path) ? 0 : -1;
        return res;
    }

    // ─── VACCUM command ───
    if (baseCmd == "VACCUM") {
        if (cmdRange.valid) {
            if (cmdRange.end > 0) {
                res.code = engine->vaccum(cmdRange.start, cmdRange.end) ? 0 : -1;
            } else {
                res.code = engine->vaccum(cmdRange.start) ? 0 : -1;
            }
        } else {
            engine->vaccum();
            res.code = 0;
        }
        return res;
    }

    // Graph Commands (GR, GW, VGW)
    if (cmd == "GRAPHREAD" || cmd == "GR" ||
        cmd == "GRAPHWRITE" || cmd == "GW" ||
        cmd == "GRAPHWRITEVOLATILE" || cmd == "VGW") {
        
        bool isWrite = (cmd == "GRAPHWRITE" || cmd == "GW" || cmd == "GRAPHWRITEVOLATILE" || cmd == "VGW");
        bool isVolatile = (cmd == "GRAPHWRITEVOLATILE" || cmd == "VGW");
        
        Array<GraphOp> ops;
        Array<String> columns;
        usz idx = 1;
        
        // Handle EXTRACT keyword
        while (idx < tokens.size()) {
            String t = tokens[idx];
            String tu = t.toUpperCase();
            
            if (tu == "EXTRACT") {
                idx++;
                if (idx < tokens.size()) {
                    Array<GraphOp> extOps = parseExtract(tokens[idx]);
                    for(usz i=0; i<extOps.size(); ++i) ops.push(extOps[i]);
                    idx++;
                }
            } else if (tu == "MATCH" || tu == "FOLLOW" || tu == "REPEATFOLLOW" || tu == "UNTIL" || (tu == "SET" && isWrite)) {
                GraphOp op;
                if (tu == "MATCH") op.type = GraphOpType::MATCH;
                else if (tu == "FOLLOW") op.type = GraphOpType::FOLLOW;
                else if (tu == "REPEATFOLLOW") op.type = GraphOpType::REPEATFOLLOW;
                else if (tu == "UNTIL") op.type = GraphOpType::UNTIL;
                else if (tu == "SET") op.type = GraphOpType::SET;
                
                idx++;
                Clauses group;
                while (idx < tokens.size()) {
                    String t2 = tokens[idx].toUpperCase();
                    if (t2 == "MATCH" || t2 == "FOLLOW" || t2 == "REPEATFOLLOW" || t2 == "UNTIL" || t2 == "SET" || t2 == "REMOVE" || t2 == "EXTRACT") {
                        break;
                    }
                    if (t2 == "OR") {
                        if (group.size() > 0) { op.query.push(group); group.clear(); }
                        idx++; continue;
                    }
                    if (op.type == GraphOpType::SET) {
                        op.writeSet.push(parseClauseStr(tokens[idx]));
                    } else {
                        group.push(parseClauseStr(tokens[idx]));
                    }
                    idx++;
                }
                if (group.size() > 0) op.query.push(group);
                ops.push(op);
            } else if (tu == "REMOVE" && isWrite) {
                GraphOp op;
                op.type = GraphOpType::REMOVE;
                ops.push(op);
                idx++;
            } else if (!isWrite) {
                // columns for read
                columns.push(t);
                idx++;
            } else {
                idx++;
            }
        }
        
        if (isWrite) {
            for (usz i = 0; i < ops.size(); ++i) {
                if (ops[i].type == GraphOpType::SET) {
                    for (usz j = 0; j < ops[i].writeSet.size(); ++j) {
                        if (ops[i].writeSet[j].col.endsWith(":generate")) {
                            String baseCol = ops[i].writeSet[j].col.substring(0, ops[i].writeSet[j].col.size() - 9);
                            ops[i].writeSet[j].col = baseCol;
                            ops[i].writeSet[j].val = engine->generateId(baseCol);
                        }
                    }
                }
            }
            if (isVolatile) {
                res.code = engine->graphWriteVolatile(ops, 0, "");
            } else {
                res.code = engine->graphWrite(ops, 0, "");
            }
        } else {
            res.treeResult = engine->graphRead(columns, ops, 0, 0);
            res.code = (res.treeResult != nullptr) ? 0 : -1;
        }
        return res;
    }

    if (cmd == "FREEZE" || cmd == "THAW") {
        Array<Clauses> queryClauses;
        Clauses currentGroup;
        bool inWhere = false;
        u64 freezePos = 0;
        usz idx = 1;
        
        if (cmd == "FREEZE") {
            if (idx < tokens.size()) {
                freezePos = (u64)strtoull((const char*)tokens[idx].data(), nullptr, 0);
                idx++;
            }
        }
        
        while (idx < tokens.size()) {
            String t = tokens[idx];
            String tu = t.toUpperCase();
            if (tu == "WHERE") {
                inWhere = true;
                idx++;
                if (idx < tokens.size()) {
                    currentGroup.push(parseClauseStr(tokens[idx]));
                }
            } else if (tu == "OR") {
                if (idx + 1 < tokens.size() && tokens[idx+1].toUpperCase() == "WHERE") {
                    if (currentGroup.size() > 0) {
                        queryClauses.push(currentGroup);
                        currentGroup.clear();
                    }
                    inWhere = true;
                    idx += 2;
                    if (idx < tokens.size()) {
                        currentGroup.push(parseClauseStr(tokens[idx]));
                    }
                }
            } else {
                if (inWhere) currentGroup.push(parseClauseStr(t));
            }
            idx++;
        }
        if (currentGroup.size() > 0) queryClauses.push(currentGroup);
        
        Array<String> readCols;
        readCols.push("id");
        readCols.push("content:blob");
        Array<Map<String, String>> rows = engine->read(readCols, queryClauses);
        u64 affected = 0;
        for (const auto& row : rows) {
            if (row.has("content:blob")) {
                u32 ref = (u32)row.get("content:blob")->toInt();
                if (cmd == "FREEZE") {
                    if (engine->freezeBlob(freezePos, ref)) affected++;
                } else {
                    engine->thawBlob(ref);
                    affected++;
                }
            }
        }
        res.code = affected;
        return res;
    }

    if (cmd == "VACUUM" || cmd == "VACCUM") {
        if (tokens.size() == 1) { engine->vaccum(); res.code = 0; }
        else if (tokens.size() == 2) { engine->vaccum(strtoull((const char*)tokens[1].data(), nullptr, 0)); res.code = 0; }
        else if (tokens.size() == 3) { engine->vaccum(strtoull((const char*)tokens[1].data(), nullptr, 0), strtoull((const char*)tokens[2].data(), nullptr, 0)); res.code = 0; }
        return res;
    }

    if (cmd == "LOCK") {
        bool req = false;
        if (tokens.size() > 2 && tokens[1].toUpperCase() == "AS") {
            req = tokens[2] == "1" || tokens[2].toUpperCase() == "TRUE";
        }
        res.code = engine->lock(Array<Clauses>(), 0, req);
        return res;
    }

    if (cmd == "UNLOCK") {
        if (tokens.size() > 1) res.code = engine->unlock(strtoull((const char*)tokens[1].data(), nullptr, 0));
        return res;
    }

    if (cmd == "ROLLBACK") {
        if (tokens.size() > 1) res.code = engine->rollback(strtoull((const char*)tokens[1].data(), nullptr, 0)) ? 0 : -1;
        return res;
    }

    if (cmd == "UNWATCH") {
        if (tokens.size() > 1) res.code = engine->unwatch(strtoull((const char*)tokens[1].data(), nullptr, 0)) ? 0 : -1;
        return res;
    }

    if (cmd == "PULL") {
        if (tokens.size() > 1) {
            u64 watchId = strtoull((const char*)tokens[1].data(), nullptr, 0);
            res.readRows = engine->pull(watchId);
            res.code = 0;
        } else {
            res.code = -1;
        }
        return res;
    }

    if (cmd == "WATCH") {
        Array<Clauses> queryClauses;
        Clauses currentGroup;
        bool inWhere = false;
        usz idx = 1;
        
        while (idx < tokens.size()) {
            String t = tokens[idx];
            String tu = t.toUpperCase();
            if (tu == "WHERE") {
                inWhere = true;
                idx++;
                if (idx < tokens.size()) currentGroup.push(parseClauseStr(tokens[idx]));
            } else if (tu == "OR") {
                if (idx + 1 < tokens.size() && tokens[idx+1].toUpperCase() == "WHERE") {
                    if (currentGroup.size() > 0) { queryClauses.push(currentGroup); currentGroup.clear(); }
                    inWhere = true;
                    idx += 2;
                    if (idx < tokens.size()) currentGroup.push(parseClauseStr(tokens[idx]));
                }
            } else {
                if (inWhere) currentGroup.push(parseClauseStr(t));
            }
            idx++;
        }
        if (currentGroup.size() > 0) queryClauses.push(currentGroup);
        
        res.code = engine->watch(queryClauses);
        return res;
    }

    // Standard CRUD (READ, WRITE, WRITEVOLATILE, REMOVE)
    if (cmd == "READ" || cmd == "WRITE" || cmd == "WRITEVOLATILE" || cmd == "REMOVE") {
        Array<String> columns;
        Array<Clause> writeCols;
        Array<Clauses> queryClauses;
        Clauses currentGroup;
        
        usz idx = 1;
        bool inWhere = false;
        
        while (idx < tokens.size()) {
            String t = tokens[idx];
            String tu = t.toUpperCase();
            
            if (tu == "WHERE") {
                inWhere = true;
                idx++;
                if (idx < tokens.size()) {
                    currentGroup.push(parseClauseStr(tokens[idx]));
                }
            } else if (tu == "OR") {
                if (idx + 1 < tokens.size() && tokens[idx+1].toUpperCase() == "WHERE") {
                    if (currentGroup.size() > 0) {
                        queryClauses.push(currentGroup);
                        currentGroup.clear();
                    }
                    inWhere = true;
                    idx += 2;
                    if (idx < tokens.size()) {
                        currentGroup.push(parseClauseStr(tokens[idx]));
                    }
                } else {
                    // Invalid OR
                }
            } else {
                if (inWhere) {
                    currentGroup.push(parseClauseStr(t));
                } else {
                    if (cmd == "READ") columns.push(t);
                    else writeCols.push(parseClauseStr(t));
                }
            }
            idx++;
        }
        
        if (currentGroup.size() > 0) {
            queryClauses.push(currentGroup);
        }
        
        if (cmd == "READ") {
            res.readRows = engine->read(columns, queryClauses);
            res.code = res.readRows.size();
        } else if (cmd == "WRITE" || cmd == "WRITEVOLATILE") {
            for (usz i = 0; i < writeCols.size(); ++i) {
                if (writeCols[i].col.endsWith(":generate")) {
                    String baseCol = writeCols[i].col.substring(0, writeCols[i].col.size() - 9);
                    writeCols[i].col = baseCol;
                    writeCols[i].val = engine->generateId(baseCol);
                }
            }
            if (cmd == "WRITE") {
                res.code = engine->write(writeCols, queryClauses);
            } else {
                res.code = engine->writeVolatile(writeCols, queryClauses);
            }
        } else if (cmd == "REMOVE") {
            res.code = engine->remove(queryClauses) ? 0 : -1;
        }
        return res;
    }

    res.code = -99; // Unknown command
    return res;
}

} // namespace Xylem
