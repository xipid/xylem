#ifndef XYLEM_SERVER_HPP
#define XYLEM_SERVER_HPP

#include <Xylem/Xylem.hpp>
#include <Rho/Tunnel.hpp>
#include <Util/Server.hpp>
#include <Lines/Bind.hpp>
#include <Collection/Map.hpp>
#include <Collection/String.hpp>
#include <Collection/Array.hpp>

#include <mutex>

namespace Xylem {

class XylemServer {
public:
    XylemEngine& engine;
    Rho::Server server;
    Collection::Map<Rho::Tunnel*, Collection::String> clientIdentities;
    Collection::Map<Rho::Tunnel*, Collection::String> clientPubKeys;
    std::mutex* engineMutex = nullptr;
    
    // Permission cache: avoids querying /perms/ on every request
    bool permsCacheChecked = false;
    bool permsCacheResult = false;

    XylemServer(XylemEngine& eng, std::mutex* mtx = nullptr);
    ~XylemServer();

    void hook(Lines::Bind& bind);
    void update();

    bool hasAnyPermissions();
    bool checkPermission(const Collection::String& clientHash, const Collection::String& action, const Collection::String& path);
    Collection::String getClientPubKey(const Collection::String& clientHash);
    Collection::String getPathForId(const Collection::String& id);
    Collection::String getPathForRow(const Collection::Map<Collection::String, Collection::String>& row);
    Collection::String getPathForRowId(Xi::u64 rId);
    Collection::String getPermPath(const Collection::String& action, const Collection::String& path);

private:
    void handleCart(Rho::Cart& cart);
    void handlePacket(const Rho::Packet& p, Rho::Tunnel& tunnel);
    
    // Deconstruct paths and check permission helpers
    bool checkReadPermForRows(const Collection::String& clientHash, Collection::Array<Collection::Map<Collection::String, Collection::String>>& rows);
    bool checkWritePerm(const Collection::String& clientHash, const Collection::Array<Clause>& columns, const Collection::Array<Clauses>& clauses);
    bool checkRmPerm(const Collection::String& clientHash, const Collection::Array<Clauses>& clauses);
};

} // namespace Xylem

#endif // XYLEM_SERVER_HPP
