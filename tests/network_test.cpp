#include <Xylem/Xylem.hpp>
#include <Xylem/Client.hpp>
#include <Xylem/Server.hpp>
#include <Terminal/Format.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

using namespace Xylem;
using namespace Terminal;
using namespace Xi;
using namespace Collection;

int main() {
    Info("=== Xylem Network & Authorization Tests ===");

    // Generate KeyPairs for Client A (Admin) and Client B (User)
    Security::KeyPair keysA = Security::generateKeyPair();
    String hashA = Security::hash(keysA.publicKey, 8);
    String hexA = "";
    for (usz i = 0; i < hashA.size(); i++) {
        char buf[3];
        sprintf(buf, "%02x", (unsigned char)hashA[i]);
        hexA += buf;
    }
    Info("Client A Hash Hex: " + hexA);

    Security::KeyPair keysB = Security::generateKeyPair();
    String hashB = Security::hash(keysB.publicKey, 8);
    String hexB = "";
    for (usz i = 0; i < hashB.size(); i++) {
        char buf[3];
        sprintf(buf, "%02x", (unsigned char)hashB[i]);
        hexB += buf;
    }
    Info("Client B Hash Hex: " + hexB);

    unlink("/tmp/network_test.xy");

    // Phase 1: Bootstrap database and permissions
    {
        XylemEngine xm;
        xm.config.deviceSize = 1024 * 1024 * 10; // 10MB
        xm.config.blockSize = 4096;
        
        int fd = open("/tmp/network_test.xy", O_RDWR | O_CREAT, 0644);
        xm.config.onDeviceRead = [fd](u64 offset, u64 maxOffset) -> String {
            String buf;
            buf.allocate(maxOffset - offset);
            buf.fill(0xFF);
            pread(fd, buf.data(), buf.size(), offset);
            return buf;
        };
        xm.config.onDeviceWrite = [fd](u64 offset, String data) -> bool {
            return pwrite(fd, data.data(), data.size(), offset) == (ssize_t)data.size();
        };
        xm.config.onDeviceErase = [fd](u64 offset, u64 maxOffset) -> bool {
            String empty;
            empty.allocate(maxOffset - offset);
            empty.fill(0xFF);
            pwrite(fd, empty.data(), empty.size(), offset);
            return true;
        };

        if (!xm.format() || !xm.mount()) {
            Error("Local bootstrapping failed!");
            close(fd);
            return 1;
        }

        auto addPerm = [&](const String& action, const String& pathPattern, const String& ownerHash) {
            String fullPath = "/perms/" + action + "/" + pathPattern;
            String cleanPath = fullPath;
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
            
            for (usz i = 0; i < cleanParts.size() - 1; ++i) {
                Array<Clauses> dirClauses;
                dirClauses.push(WHERE("name", "=", cleanParts[i]) && WHERE("parent_id", "=", currentParentId));
                auto dirRows = xm.read(readCols, dirClauses);
                
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
                    
                    int res = xm.write(dirCols);
                    printf("[DEBUG addPerm dir] name=%s, parent_id=%s, id=%s, write_res=%d\n",
                           cleanParts[i].c_str(), currentParentId.c_str(), newId.c_str(), res);
                    currentParentId = newId;
                }
            }
            
            u64 rnd = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
            String fileId(rnd);
            
            Array<Clause> fileCols;
            fileCols.push({"name", "=", cleanParts[cleanParts.size() - 1]});
            fileCols.push({"parent_id", "=", currentParentId});
            fileCols.push({"id", "=", fileId});
            fileCols.push({"owner", "=", ownerHash});
            
            int res = xm.write(fileCols);
            printf("[DEBUG addPerm file] name=%s, parent_id=%s, id=%s, write_res=%d\n",
                   cleanParts[cleanParts.size() - 1].c_str(), currentParentId.c_str(), fileId.c_str(), res);
        };

        // Client A gets full wildcard access
        addPerm("create", "**", hashA);
        addPerm("read/me", "**", hashA);
        addPerm("read/*", "**", hashA);
        addPerm("modify/me", "**", hashA);
        addPerm("modify/*", "**", hashA);
        addPerm("unlink/*", "**", hashA);
        addPerm("watch", "**", hashA);
        addPerm("own/*", "**", hashA);
        addPerm("disown/me", "**", hashA);
        addPerm("disown/*", "**", hashA);

        // Client B gets restricted access (create and read/me only, watch)
        addPerm("create", "data/**", hashB);
        addPerm("read/me", "data/**", hashB);
        addPerm("watch", "data/**", hashB);

        Array<String> cols; cols.push("id"); cols.push("name"); cols.push("parent_id"); cols.push("owner");
        Array<Clauses> allCl;
        allCl.push(WHERE("id", "reg", ".*"));
        auto allRows = xm.read(cols, allCl, 100, 0, false, 0, true);
        printf("[DEBUG bootstrap] Total rows = %zu\n", allRows.size());
        for (const auto& row : allRows) {
            String hexOwner = "";
            if (row.has("owner")) {
                String o = *row.get("owner");
                for (usz i = 0; i < o.size(); i++) {
                    char buf[3];
                    sprintf(buf, "%02x", (unsigned char)o[i]);
                    hexOwner += buf;
                }
            }
            printf("[DEBUG bootstrap row] id=%s, name=%s, parent_id=%s, ownerHex=%s\n",
                   row.has("id") ? row.get("id")->c_str() : "",
                   row.has("name") ? row.get("name")->c_str() : "",
                   row.has("parent_id") ? row.get("parent_id")->c_str() : "",
                   hexOwner.c_str());
        }

        xm.flush();
        xm.destroy();
        close(fd);
        Success("Bootstrap finished, database closed.");
    }

    pid_t pid = fork();
    if (pid < 0) {
        Error("Failed to fork server process!");
        return 1;
    }

    if (pid == 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        // Child Process: Run the Server
        XylemEngine xm;
        xm.config.deviceSize = 1024 * 1024 * 10;
        xm.config.blockSize = 4096;
        
        int fd = open("/tmp/network_test.xy", O_RDWR, 0644);
        xm.config.onDeviceRead = [fd](u64 offset, u64 maxOffset) -> String {
            String buf;
            buf.allocate(maxOffset - offset);
            buf.fill(0xFF);
            pread(fd, buf.data(), buf.size(), offset);
            return buf;
        };
        xm.config.onDeviceWrite = [fd](u64 offset, String data) -> bool {
            return pwrite(fd, data.data(), data.size(), offset) == (ssize_t)data.size();
        };
        xm.config.onDeviceErase = [fd](u64 offset, u64 maxOffset) -> bool {
            String empty;
            empty.allocate(maxOffset - offset);
            empty.fill(0xFF);
            pwrite(fd, empty.data(), empty.size(), offset);
            return true;
        };

        if (!xm.mount()) {
            Error("Child failed to mount XylemEngine!");
            exit(1);
        }

        Lines::Bind srvBind("127.0.0.1:9099");
        XylemServer server(xm);
        server.hook(srvBind);

        int loopCount = 0;
        while (true) {
            srvBind.update();
            server.update();
            usleep(1000);
            loopCount++;
            if (loopCount % 1000 == 0) {
                printf("[SERVER HEARTBEAT] loops = %d, clients size = %zu\n", loopCount, server.server.clients.size());
            }
        }
        exit(0);
    }

    // Parent Process: Run the Client tests
    // Wait a moment for server child to bind
    usleep(200000);

    Lines::Bind cliBindA("127.0.0.1:0");
    XylemClient clientA;
    
    // Connect client A
    if (!clientA.connect(cliBindA, "127.0.0.1:9099", keysA)) {
        Error("Client A failed to connect!");
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Client A connected and authenticated.");

    // ─── Test 2: Create & Ownership Registration ──────────────────────────────
    Array<Clause> writeCols;
    String file1Id = clientA.generateId("id");
    writeCols.push({"id", "=", file1Id});
    writeCols.push({"name", "=", "doc1"});
    writeCols.push({"content", "=", "secret doc"});
    writeCols.push({"parent_id", "=", ""});
    
    int wCode = clientA.write(writeCols);
    if (wCode != 0) {
        Error("Client A write failed with code: " + String::from((long long)wCode));
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Client A successfully wrote 'doc1'.");

    // Check ownership row `/perms/owning/doc1` exists and is owned by hashA
    Array<String> readCols;
    readCols.push("owner");
    Array<Clauses> qOwn;
    qOwn.push(WHERE("name", "path", "/perms/owning/doc1"));
    auto ownRows = clientA.read(readCols, qOwn);
    if (ownRows.size() != 1 || *ownRows[0].get("owner") != hashA) {
        Error("Ownership row not registered correctly on write!");
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Test 2 Passed: Ownership auto-registered for creator.");

    // ─── Test 3: Read Permissions and Filtration ────────────────────────────
    printf("[PARENT] Connecting Client B...\n");
    int childStatus;
    pid_t wpid = waitpid(pid, &childStatus, WNOHANG);
    if (wpid == 0) {
        printf("[PARENT] Server child process is alive (pid = %d).\n", pid);
    } else if (wpid == pid) {
        printf("[PARENT] Server child process exited with status %d.\n", childStatus);
    } else {
        printf("[PARENT] waitpid returned error: %d\n", errno);
    }
    
    Lines::Bind cliBindB("127.0.0.1:0");
    XylemClient clientB;
    if (!clientB.connect(cliBindB, "127.0.0.1:9099", keysB)) {
        Error("Client B failed to connect!");
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Client B connected.");

    // Client B tries to read 'doc1' (no permission)
    Array<String> colsB;
    colsB.push("name");
    colsB.push("content");
    Array<Clauses> qReadB;
    qReadB.push(WHERE("name", "=", "doc1") && WHERE("parent_id", "=", ""));
    auto rowsB = clientB.read(colsB, qReadB);
    if (rowsB.size() != 0) {
        Error("Security violation: Client B read 'doc1' without read permission!");
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Client B read filtered out successfully (unauthorized).");

    // Client A reads 'doc1' (allowed via read/me and ownership, or read/*)
    Array<String> colsA;
    colsA.push("name");
    colsA.push("content");
    Array<Clauses> qReadA;
    qReadA.push(WHERE("name", "=", "doc1") && WHERE("parent_id", "=", ""));
    auto rowsA = clientA.read(colsA, qReadA);
    if (rowsA.size() != 1 || *rowsA[0].get("content") != "secret doc") {
        Error("Client A failed to read 'doc1'!");
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Test 3 Passed: Read permissions enforced and filtered correctly.");

    // ─── Test 4: Write Modification Disowns Others ───────────────────────────
    // Register hashB as an owner of doc1 as well via clientA (has own.* permission)
    Array<Clause> addOwnCols;
    addOwnCols.push({"name", "=", "/perms/owning/doc1"});
    addOwnCols.push({"owner", "=", hashB});
    int ownCode = clientA.write(addOwnCols);
    if (ownCode != 0) {
        Error("Registering second owner failed!");
        kill(pid, SIGKILL);
        return 1;
    }

    // Client A modifies doc1
    Array<Clause> modCols;
    modCols.push({"content", "=", "modified secret doc"});
    Array<Clauses> modClauses;
    modClauses.push(WHERE("name", "=", "doc1"));
    int modCode = clientA.write(modCols, modClauses);
    if (modCode != 0) {
        Error("Client A modification failed!");
        kill(pid, SIGKILL);
        return 1;
    }

    // Verify that hashB is disowned, and only hashA remains owner
    Array<String> ownerCols;
    ownerCols.push("owner");
    Array<Clauses> qCheckOwners;
    qCheckOwners.push(WHERE("name", "path", "/perms/owning/doc1"));
    auto currentOwners = clientA.read(ownerCols, qCheckOwners);
    if (currentOwners.size() != 1 || *currentOwners[0].get("owner") != hashA) {
        Error("Disowning other owners on modify failed! Owners count: " + String::from((u64)currentOwners.size()));
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Test 4 Passed: Modifications successfully disowned other owners.");

    // ─── Test 5: Network Transactions (MVCC) ──────────────────────────────────
    u64 txId = clientA.lock(Array<Clauses>(), 0, false);
    if (txId == 0) {
        Error("Client A failed to obtain lock/transaction ID!");
        kill(pid, SIGKILL);
        return 1;
    }
    Info("Started transaction: " + String::from((long long)txId));

    // Perform write within transaction
    Array<Clause> txWrite;
    txWrite.push({"id", "=", clientA.generateId("id")});
    txWrite.push({"name", "=", "tx_doc"});
    txWrite.push({"content", "=", "transactional content"});
    txWrite.push({"parent_id", "=", ""});
    int txWCode = clientA.write(txWrite, Array<Clauses>(), txId);
    if (txWCode != 0) {
        Error("Transactional write failed!");
        kill(pid, SIGKILL);
        return 1;
    }

    // Verify it is not visible to others before commit
    Array<String> txCols;
    txCols.push("name");
    Array<Clauses> qCheckTx;
    qCheckTx.push(WHERE("name", "=", "tx_doc") && WHERE("parent_id", "=", ""));
    auto checkBeforeCommit = clientB.read(txCols, qCheckTx);
    if (checkBeforeCommit.size() > 0) {
        Error("Transactional write visible before commit!");
        kill(pid, SIGKILL);
        return 1;
    }

    // Commit
    clientA.commit(Array<Clauses>(), txId);
    Success("Transaction committed.");

    // ─── Test 6: Network Watchers & Pull ─────────────────────────────────────
    // Client B watches data directory `data/**`
    Array<Clauses> watchC;
    watchC.push(WHERE("name", "path", "data/**"));
    u64 watchId = clientB.watch(watchC);
    if (watchId == 0) {
        Error("Client B watch registration failed!");
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Client B registered watch on 'data/**' (ID: " + String::from((long long)watchId) + ").");

    // Client B writes to `data/item1`
    Array<Clause> itemCols;
    itemCols.push({"id", "=", clientB.generateId("id")});
    itemCols.push({"name", "=", "data/item1"});
    itemCols.push({"content", "=", "hello world"});
    int itemCode = clientB.write(itemCols);
    if (itemCode != 0) {
        Error("Client B item write failed!");
        kill(pid, SIGKILL);
        return 1;
    }

    // Give some time for background propagation
    usleep(50000);

    // Pull changes on Client B
    auto pulled = clientB.pull(watchId);
    if (pulled.size() != 1 || *pulled[0].get("name") != "data/item1") {
        Error("Client B watch pull did not return the written item! Size: " + String::from((u64)pulled.size()));
        kill(pid, SIGKILL);
        return 1;
    }
    Success("Test 6 Passed: Network watchers and pull function perfectly.");

    // ─── Test 7: Column-Based Cryptographic Ownership ───
    {
        Info("Test 7: Column-based Cryptographic Ownership");
        
        Array<Clause> writeCols;
        String file2Id = clientA.generateId("id");
        writeCols.push({"content", "=", "signature protected doc"});
        writeCols.push({"id", "=", file2Id});
        writeCols.push({"name", "=", "crypto_doc"});
        writeCols.push({"parent_id", "=", ""});
        
        // Serialize the other columns to sign
        // Alphabetically sorted columns: content, id, name, parent_id
        String msg = "content=signature protected doc;id=" + file2Id + ";name=crypto_doc;parent_id=";
        
        String sig = Security::signX(keysA.secretKey, msg);
        
        String sigHex;
        for (usz i = 0; i < sig.size(); ++i) {
            char buf[3];
            sprintf(buf, "%02x", (unsigned char)sig[i]);
            sigHex += buf;
        }
        
        String owningVal = hexA + sigHex;
        writeCols.push({"owning", "=", owningVal});
        
        int wCode = clientA.write(writeCols);
        if (wCode != 0) {
            Error("Test 7 failed: Write with valid owning column signature failed with code: " + String::from((long long)wCode));
            kill(pid, SIGKILL);
            return 1;
        }
        Success("Test 7.1 Passed: Valid cryptographic signature written successfully.");
        
        // Try to modify it using Client A but with an invalid signature
        Array<Clause> modColsInvalidSig;
        modColsInvalidSig.push({"content", "=", "updated by owner"});
        modColsInvalidSig.push({"owning", "=", hexA + sigHex.substring(0, 126) + "00"});
        
        Array<Clauses> qMod;
        qMod.push(WHERE("id", "=", file2Id));
        int modCode = clientA.write(modColsInvalidSig, qMod);
        if (modCode == 0) {
            Error("Test 7 failed: Write with invalid signature succeeded!");
            kill(pid, SIGKILL);
            return 1;
        }
        Success("Test 7.2 Passed: Invalid signature was correctly rejected.");
    }

    // Clean up
    kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);

    clientA.client.destroy();
    clientB.client.destroy();
    unlink("/tmp/network_test.xy");

    Success("All network and authorization tests completed successfully!");
    return 0;
}
