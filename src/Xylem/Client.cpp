#include <Xylem/Client.hpp>
#include <Encoding/Yaml.hpp>
#include <Security/Crypto.hpp>
#include <unistd.h>

namespace Xylem {

using namespace Collection;
using namespace Rho;

XylemClient::XylemClient() {}

XylemClient::~XylemClient() {
    client.destroy();
}

bool XylemClient::connect(Lines::Bind& b, const Resource::NumericalAddress& address, const Security::KeyPair& staticKeyPair) {
    printf("[CLIENT] connect start, address size = %zu\n", address.size());
    bind = &b;
    client.staticKeyPairs.push(staticKeyPair);
    client.hook(b, address);
    
    bool announced = false;
    client.onAnnounce([&](Cart& c) {
        printf("[CLIENT] onAnnounce callback triggered!\n");
        client.upgrade();
        announced = true;
    });
    printf("[CLIENT] sending probe...\n");
    client.probe();
    
    u64 start = Xi::millis();
    int iter = 0;
    while (!announced && Xi::millis() - start < 5000) {
        bind->update();
        client.update();
        usleep(1000);
        iter++;
        if (iter % 500 == 0) {
            client.probe();
        }
        if (iter % 1000 == 0) {
            printf("[CLIENT] loop tick, elapsed = %llu ms\n", (unsigned long long)(Xi::millis() - start));
        }
    }
    
    printf("[CLIENT] connect finished, announced = %s\n", announced ? "true" : "false");
    isConnected = announced;
    return isConnected;
}

bool XylemClient::connect(Lines::Bind& b, const String& addressStr, const Security::KeyPair& staticKeyPair) {
    Resource::NumericalAddress numAddr(addressStr);
    if (numAddr.size() > 0) {
        return connect(b, numAddr, staticKeyPair);
    }
    bind = &b;
    client.staticKeyPairs.push(staticKeyPair);
    client.hook(b, addressStr);
    
    bool announced = false;
    client.onAnnounce([&](Cart& c) {
        client.upgrade();
        announced = true;
    });
    client.probe();
    
    u64 start = Xi::millis();
    while (!announced && Xi::millis() - start < 5000) {
        bind->update();
        client.update();
        usleep(1000);
    }
    
    isConnected = announced;
    return isConnected;
}

bool XylemClient::isMounted() const {
    return isConnected && client.tunnel && !client.tunnel->isDestroyed;
}

bool XylemClient::connect(Lines::Bind& b, const char* addressStr, const Security::KeyPair& staticKeyPair) {
    return connect(b, String(addressStr), staticKeyPair);
}

void XylemClient::update() {
    client.update();
}

Map<String, String> XylemClient::sendRequest(const Map<String, String>& req) {
    if (!isConnected || !client.tunnel) {
        Map<String, String> errResp;
        errResp.set("_status", "error");
        errResp.set("_code", "-1");
        return errResp;
    }
    
    bool gotResponse = false;
    Map<String, String> response;
    
    client.onPacket([&](Packet p) {
        response = Map<String, String>::deserialize(p.payload);
        gotResponse = true;
    });
    
    client.push(Packet(req.serialize()));
    client.pushCart();
    
    u64 start = Xi::millis();
    while (!gotResponse && Xi::millis() - start < 10000) {
        bind->update();
        client.update();
        if (client.tunnel && client.tunnel->isDestroyed) {
            break;
        }
        usleep(1000);
    }
    
    if (!gotResponse) {
        Map<String, String> errResp;
        errResp.set("_status", "error");
        errResp.set("_code", "-2");
        return errResp;
    }
    
    return response;
}

String XylemClient::serializeClauses(const Array<Clauses>& clauses) {
    Map<String, String> clMap;
    clMap.set("size", String::from((long long)clauses.size()));
    for (usz i = 0; i < clauses.size(); i++) {
        Map<String, String> gMap;
        gMap.set("isAssert", clauses[i].isAssert ? "true" : "false");
        gMap.set("size", String::from((long long)clauses[i].size()));
        for (usz j = 0; j < clauses[i].size(); j++) {
            Map<String, String> cMap;
            cMap.set("col", clauses[i][j].col);
            cMap.set("op", clauses[i][j].op);
            cMap.set("val", clauses[i][j].val);
            gMap.set(String::from((long long)j), cMap.serialize());
        }
        clMap.set(String::from((long long)i), gMap.serialize());
    }
    return clMap.serialize();
}

String XylemClient::serializeClauseArray(const Array<Clause>& columns) {
    Map<String, String> colsMap;
    colsMap.set("size", String::from((long long)columns.size()));
    for (usz i = 0; i < columns.size(); i++) {
        Map<String, String> cMap;
        cMap.set("col", columns[i].col);
        cMap.set("op", columns[i].op);
        cMap.set("val", columns[i].val);
        colsMap.set(String::from((long long)i), cMap.serialize());
    }
    return colsMap.serialize();
}

QueryResult XylemClient::query(const String& queryString, const Array<String>& sanitized) {
    String substituted = "";
    for (usz i = 0; i < queryString.size(); ++i) {
        if (queryString[i] == '%' && i + 1 < queryString.size() && queryString[i+1] >= '1' && queryString[i+1] <= '9') {
            int argIdx = queryString[i+1] - '1';
            if (argIdx >= 0 && (usz)argIdx < sanitized.size()) {
                substituted += sanitized[argIdx];
            }
            i++;
        } else {
            substituted.push(queryString[i]);
        }
    }

    Map<String, String> req;
    req.set("_cmd", "query");
    req.set("query", substituted);
    
    Map<String, String> resp = sendRequest(req);
    QueryResult qres;
    qres.code = resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
    
    if (resp.has("readRows")) {
        Map<String, String> rowsMap = Map<String, String>::deserialize(*resp.get("readRows"));
        usz size = rowsMap.has("size") ? (usz)parseLong(*rowsMap.get("size")) : 0;
        for (usz i = 0; i < size; i++) {
            qres.readRows.push(Map<String, String>::deserialize(*rowsMap.get(String::from((long long)i))));
        }
    }
    
    if (resp.has("treeResult")) {
        TreeBranch* tb = new TreeBranch();
        if (Encoding::parseYAML(*resp.get("treeResult"), *tb)) {
            qres.treeResult = tb;
        } else {
            delete tb;
        }
    }
    
    return qres;
}

Array<Map<String,String>> XylemClient::read(const Array<String>& columns, const Array<Clauses>& clauses,
                                            u64 length, u64 page, bool tombstones, u64 txId,
                                            bool readAllColumns) {
    Map<String, String> req;
    req.set("_cmd", "read");
    
    Map<String, String> colsMap;
    colsMap.set("size", String::from((long long)columns.size()));
    for (usz i = 0; i < columns.size(); i++) {
        colsMap.set(String::from((long long)i), columns[i]);
    }
    req.set("columns", colsMap.serialize());
    req.set("clauses", serializeClauses(clauses));
    req.set("length", String::from((long long)length));
    req.set("page", String::from((long long)page));
    req.set("readAllColumns", readAllColumns ? "true" : "false");
    req.set("txId", String::from((long long)txId));
    
    Map<String, String> resp = sendRequest(req);
    Array<Map<String, String>> rows;
    
    if (resp.has("rows")) {
        Map<String, String> rowsMap = Map<String, String>::deserialize(*resp.get("rows"));
        usz size = rowsMap.has("size") ? (usz)parseLong(*rowsMap.get("size")) : 0;
        for (usz i = 0; i < size; i++) {
            rows.push(Map<String, String>::deserialize(*rowsMap.get(String::from((long long)i))));
        }
    }
    
    return rows;
}

int XylemClient::write(const Array<Clause>& columns, const Array<Clauses>& clauses, u64 txId, const String& encryptionKey) {
    Map<String, String> req;
    req.set("_cmd", "write");
    req.set("columns", serializeClauseArray(columns));
    req.set("clauses", serializeClauses(clauses));
    req.set("txId", String::from((long long)txId));
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
}

int XylemClient::writeVolatile(const Array<Clause>& columns, const Array<Clauses>& clauses, u64 txId, const String& encryptionKey) {
    Map<String, String> req;
    req.set("_cmd", "writeVolatile");
    req.set("columns", serializeClauseArray(columns));
    req.set("clauses", serializeClauses(clauses));
    req.set("txId", String::from((long long)txId));
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
}

bool XylemClient::rm(const Array<Clauses>& clauses, u64 length, u64 as) {
    Map<String, String> req;
    req.set("_cmd", "rm");
    req.set("clauses", serializeClauses(clauses));
    req.set("length", String::from((long long)length));
    req.set("as", String::from((long long)as));
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("_status") && (*resp.get("_status") == "ok");
}

u64 XylemClient::lock(const Array<Clauses>& clauses, u64 id, bool requiresExplicitAs) {
    Map<String, String> req;
    req.set("_cmd", "lock");
    req.set("id", String::from((long long)id));
    req.set("requiresExplicitAs", requiresExplicitAs ? "true" : "false");
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("txId") ? (u64)strtoull(resp.get("txId")->c_str(), nullptr, 0) : 0;
}

u64 XylemClient::commit(const Array<Clauses>& clauses, u64 id) {
    Map<String, String> req;
    req.set("_cmd", "commit");
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("txId") ? (u64)strtoull(resp.get("txId")->c_str(), nullptr, 0) : 0;
}

bool XylemClient::rollback(u64 id) {
    Map<String, String> req;
    req.set("_cmd", "rollback");
    req.set("id", String::from((long long)id));
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("_status") && (*resp.get("_status") == "ok");
}

int XylemClient::unlock(u64 id) {
    Map<String, String> req;
    req.set("_cmd", "unlock");
    req.set("id", String::from((long long)id));
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
}

String XylemClient::generateId(const String& column) {
    Map<String, String> req;
    req.set("_cmd", "generateId");
    req.set("column", column);
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("id") ? *resp.get("id") : "";
}

QueryResult XylemClient::cat(const String& path, u64 start, u64 end) {
    Map<String, String> req;
    req.set("_cmd", "cat");
    req.set("path", path);
    req.set("start", String::from((long long)start));
    req.set("end", String::from((long long)end));
    
    Map<String, String> resp = sendRequest(req);
    QueryResult qres;
    qres.code = resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
    if (resp.has("content")) {
        Map<String, String> row;
        row.set("content", *resp.get("content"));
        qres.readRows.push(row);
    }
    return qres;
}

QueryResult XylemClient::tee(const String& path, const String& content, u64 start, u64 end) {
    Map<String, String> req;
    req.set("_cmd", "tee");
    req.set("path", path);
    req.set("content", content);
    req.set("start", String::from((long long)start));
    req.set("end", String::from((long long)end));
    
    Map<String, String> resp = sendRequest(req);
    QueryResult qres;
    qres.code = resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
    return qres;
}

QueryResult XylemClient::ls(const String& path) {
    Map<String, String> req;
    req.set("_cmd", "ls");
    req.set("path", path);
    
    Map<String, String> resp = sendRequest(req);
    QueryResult qres;
    qres.code = resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
    if (resp.has("readRows")) {
        Map<String, String> rowsMap = Map<String, String>::deserialize(*resp.get("readRows"));
        usz size = rowsMap.has("size") ? (usz)parseLong(*rowsMap.get("size")) : 0;
        for (usz i = 0; i < size; i++) {
            qres.readRows.push(Map<String, String>::deserialize(*rowsMap.get(String::from((long long)i))));
        }
    }
    return qres;
}

bool XylemClient::rm(const String& path) {
    String normPath = path;
    if (normPath.endsWith("/") && normPath.size() > 1) {
        normPath = normPath.slice(0, normPath.size() - 1);
    }
    Array<Clauses> queryClauses;
    queryClauses.push(WHERE("name", "path", normPath + "/**"));
    return rm(queryClauses);
}

QueryResult XylemClient::cp(const String& src, const String& dst) {
    Map<String, String> req;
    req.set("_cmd", "cp");
    req.set("src", src);
    req.set("dst", dst);
    
    Map<String, String> resp = sendRequest(req);
    QueryResult qres;
    qres.code = resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
    return qres;
}

QueryResult XylemClient::mv(const String& src, const String& dst) {
    Map<String, String> req;
    req.set("_cmd", "mv");
    req.set("src", src);
    req.set("dst", dst);
    
    Map<String, String> resp = sendRequest(req);
    QueryResult qres;
    qres.code = resp.has("_code") ? (int)parseLong(*resp.get("_code")) : -1;
    return qres;
}

u64 XylemClient::watch(const Array<Clauses>& clauses) {
    Map<String, String> req;
    req.set("_cmd", "watch");
    req.set("clauses", serializeClauses(clauses));
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("watchId") ? (u64)strtoull(resp.get("watchId")->c_str(), nullptr, 0) : 0;
}

bool XylemClient::unwatch(u64 id) {
    Map<String, String> req;
    req.set("_cmd", "unwatch");
    req.set("id", String::from((long long)id));
    
    Map<String, String> resp = sendRequest(req);
    return resp.has("_status") && (*resp.get("_status") == "ok");
}

Array<Map<String,String>> XylemClient::pull(u64 id) {
    Map<String, String> req;
    req.set("_cmd", "pull");
    req.set("id", String::from((long long)id));
    
    Map<String, String> resp = sendRequest(req);
    Array<Map<String, String>> rows;
    if (resp.has("rows")) {
        Map<String, String> rowsMap = Map<String, String>::deserialize(*resp.get("rows"));
        usz size = rowsMap.has("size") ? (usz)parseLong(*rowsMap.get("size")) : 0;
        for (usz i = 0; i < size; i++) {
            rows.push(Map<String, String>::deserialize(*rowsMap.get(String::from((long long)i))));
        }
    }
    return rows;
}

} // namespace Xylem
