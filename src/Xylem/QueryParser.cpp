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

    if (cleanParts.size() == 0) return ops;

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
    if (cmd == "UNMOUNT") {
        engine->unmount();
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

    // Graph Commands (GR, GW, ER, EW, VRW, VGW, VEW)
    if (cmd == "GRAPHREAD" || cmd == "GR" || cmd == "ER" || 
        cmd == "GRAPHWRITE" || cmd == "GW" || cmd == "EW" || 
        cmd == "GRAPHWRITEVOLATILE" || cmd == "VGW" || cmd == "VEW") {
        
        bool isWrite = (cmd == "GRAPHWRITE" || cmd == "GW" || cmd == "EW" || cmd == "GRAPHWRITEVOLATILE" || cmd == "VGW" || cmd == "VEW");
        bool isVolatile = (cmd == "GRAPHWRITEVOLATILE" || cmd == "VGW" || cmd == "VEW");
        
        Array<GraphOp> ops;
        Array<String> columns;
        usz idx = 1;
        
        // Handle ER / EW aliases which strictly imply EXTRACT next
        if (cmd == "ER" || cmd == "EW" || cmd == "VEW") {
            if (idx < tokens.size()) {
                ops = parseExtract(tokens[idx]);
                idx++;
            }
        }
        
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
            } else if (tu == "SET" && isWrite) {
                idx++;
                GraphOp setOp;
                setOp.type = GraphOpType::SET;
                while (idx < tokens.size()) {
                    setOp.writeSet.push(parseClauseStr(tokens[idx]));
                    idx++;
                }
                ops.push(setOp);
                break;
            } else if (!isWrite) {
                // columns for read
                columns.push(t);
                idx++;
            } else {
                idx++;
            }
        }
        
        if (isWrite) {
            if (isVolatile) {
                res.code = engine->graphWriteVolatile(ops, "");
            } else {
                res.code = engine->graphWrite(ops, 0, "");
            }
        } else {
            res.treeResult = engine->graphRead(columns, ops, 0, 0);
            res.code = (res.treeResult != nullptr) ? 0 : -1;
        }
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
        } else if (cmd == "WRITE") {
            res.code = engine->write(writeCols, queryClauses);
        } else if (cmd == "WRITEVOLATILE") {
            res.code = engine->writeVolatile(writeCols, queryClauses);
        } else if (cmd == "REMOVE") {
            res.code = engine->remove(queryClauses) ? 0 : -1;
        }
        return res;
    }

    res.code = -99; // Unknown command
    return res;
}

} // namespace Xylem
