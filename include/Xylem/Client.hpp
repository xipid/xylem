#ifndef XYLEM_CLIENT_HPP
#define XYLEM_CLIENT_HPP

#include <Xylem/Xylem.hpp>
#include <Rho/Tunnel.hpp>
#include <Util/Client.hpp>
#include <Lines/Bind.hpp>
#include <Collection/Map.hpp>
#include <Collection/String.hpp>
#include <Collection/Array.hpp>

namespace Xylem {

class XylemClient {
public:
    Rho::Client client;
    Lines::Bind* bind = nullptr;
    bool isConnected = false;

    XylemClient();
    ~XylemClient();

    bool connect(Lines::Bind& b, const Resource::NumericalAddress& address, const Security::KeyPair& staticKeyPair);
    bool connect(Lines::Bind& b, const Collection::String& addressStr, const Security::KeyPair& staticKeyPair);
    bool connect(Lines::Bind& b, const char* addressStr, const Security::KeyPair& staticKeyPair);

    bool isMounted() const;
    void update();

    // ─── Query Parser ────────────────────────────────────────────────────────
    QueryResult query(const Collection::String& queryString, const Collection::Array<Collection::String>& sanitized = Collection::Array<Collection::String>());

    // ─── Database ────────────────────────────────────────────────────────────
    Collection::Array<Collection::Map<Collection::String,Collection::String>> read(
        const Collection::Array<Collection::String>& columns,
        const Collection::Array<Clauses>& clauses,
        u64 length = 0, u64 page = 0, bool tombstones = false, u64 txId = 0,
        bool readAllColumns = false);

    int write(const Collection::Array<Clause>& columns,
              const Collection::Array<Clauses>& clauses = Collection::Array<Clauses>(),
              u64 txId = 0, const Collection::String& encryptionKey = "");

    int writeVolatile(const Collection::Array<Clause>& columns,
                      const Collection::Array<Clauses>& clauses = Collection::Array<Clauses>(),
                      u64 txId = 0, const Collection::String& encryptionKey = "");

    bool rm(const Collection::Array<Clauses>& clauses, u64 length = 0, u64 as = 0);

    // Transactions (MVCC)
    u64 lock(const Collection::Array<Clauses>& clauses = Collection::Array<Clauses>(), u64 id = 0, bool requiresExplicitAs = true);
    u64 commit(const Collection::Array<Clauses>& clauses = Collection::Array<Clauses>(), u64 id = 0);
    bool rollback(u64 id);
    int unlock(u64 id);

    Collection::String generateId(const Collection::String& column);

    // File-like convenience API
    QueryResult cat(const Collection::String& path, u64 start = 0, u64 end = 0);
    QueryResult tee(const Collection::String& path, const Collection::String& content, u64 start = 0, u64 end = 0);
    QueryResult ls(const Collection::String& path = "");
    bool rm(const Collection::String& path);
    QueryResult cp(const Collection::String& src, const Collection::String& dst);
    QueryResult mv(const Collection::String& src, const Collection::String& dst);

    // Reactivity
    u64 watch(const Collection::Array<Clauses>& clauses);
    bool unwatch(u64 id);
    Collection::Array<Collection::Map<Collection::String,Collection::String>> pull(u64 id);

private:
    Collection::Map<Collection::String, Collection::String> sendRequest(const Collection::Map<Collection::String, Collection::String>& req);
    
    Collection::String serializeClauses(const Collection::Array<Clauses>& clauses);
    Collection::String serializeClauseArray(const Collection::Array<Clause>& columns);
};

} // namespace Xylem

#endif // XYLEM_CLIENT_HPP
