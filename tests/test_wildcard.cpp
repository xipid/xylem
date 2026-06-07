#include <Xylem/Xylem.hpp>
#include <Terminal/Format.hpp>
#include <fcntl.h>
#include <unistd.h>

using namespace Xylem;
using namespace Terminal;

int main() {
    Info("--- Wildcard Path and Commit Query Unit Tests ---");

    XylemEngine xm;
    xm.config.deviceSize = 1024 * 1024 * 10; // 10MB
    xm.config.blockSize = 4096;
    
    unlink("/tmp/test_wildcard.xy");
    int fd = open("/tmp/test_wildcard.xy", O_RDWR | O_CREAT, 0644);
    
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

    xm.format();
    xm.mount();
    
    // Create folders/files for wildcard matching tests
    // Create folders/files for wildcard matching tests
    // root -> src -> Security -> Crypto.cpp, Writ.cpp, * (literal asterisk)
    // root -> src -> main.cpp
    
    auto makeDir = [&](const String& id, const String& pId, const String& name) {
        Array<Clause> r;
        r.push({"id", "=", id});
        r.push({"parent_id", "=", pId});
        r.push({"type", "=", "dir"});
        r.push({"name", "=", name});
        xm.write(r);
    };
    
    auto makeFile = [&](const String& id, const String& pId, const String& name, const String& content) {
        Array<Clause> r;
        r.push({"id", "=", id});
        r.push({"parent_id", "=", pId});
        r.push({"type", "=", "file"});
        r.push({"name", "=", name});
        r.push({"content", "=", content});
        xm.write(r);
    };

    makeDir("root_id", "", "root");
    makeDir("src_id", "root_id", "src");
    makeDir("sec_id", "src_id", "Security");
    
    makeFile("crypto_id", "sec_id", "Crypto.cpp", "crypto content");
    makeFile("writ_id", "sec_id", "Writ.cpp", "writ content");
    makeFile("asterisk_id", "sec_id", "*", "literal asterisk content");
    makeFile("main_id", "src_id", "main.cpp", "main content");

    Array<String> cols;
    cols.push("name");
    
    // Test 1: Wildcard matching: root/src/Security/*.cpp
    {
        Info("Test 1: Match root/src/Security/*.cpp");
        Array<Clauses> q;
        q.push(WHERE("name", "path", "root/src/Security/*.cpp"));
        auto res = xm.read(cols, q);
        if (res.size() != 2) {
            Error("Test 1 failed: Expected 2 matching files, got " + String::from((u64)res.size()));
            return 1;
        }
        Success("Test 1 passed!");
    }

    // Test 2: Wildcard matching with escaped wildcard: root/src/Security/\*
    {
        Info("Test 2: Match root/src/Security/\\* (literal)");
        Array<Clauses> q;
        q.push(WHERE("name", "path", "root/src/Security/\\*"));
        auto res = xm.read(cols, q);
        if (res.size() != 1 || *res[0].get("name") != "*") {
            Error("Test 2 failed: Expected 1 file named '*', got " + String::from((u64)res.size()));
            return 1;
        }
        Success("Test 2 passed!");
    }

    // Test 3: Path query regex syntax: root/src/Security/.*\.cpp
    {
        Info("Test 3: Match root/src/Security/.*\\.cpp (regex)");
        Array<Clauses> q;
        q.push(WHERE("name", "path", "root/src/Security/.*\\.cpp"));
        auto res = xm.read(cols, q);
        if (res.size() != 2) {
            Error("Test 3 failed: Expected 2 matching files via regex, got " + String::from((u64)res.size()));
            return 1;
        }
        Success("Test 3 passed!");
    }

    // Test 4: Commit transactions via query parser
    {
        Info("Test 4: Non-locking transactions via COMMIT query");
        auto rCommit = xm.query("COMMIT");
        u64 tx = rCommit.code;
        if (tx == 0) {
            Error("Test 4 failed: COMMIT query did not return a valid transaction ID");
            return 1;
        }
        
        // Write within commit transaction
        xm.query("WRITE topic='events' WHERE topic='events' AS " + String::from(tx));
        
        auto rUnlock = xm.query("UNLOCK " + String::from(tx));
        if (rUnlock.code != 0) {
            Error("Test 4 failed: UNLOCK failed with code " + String::from((u64)rUnlock.code));
            return 1;
        }
        Success("Test 4 passed!");
    }

    xm.destroy();
    close(fd);
    unlink("/tmp/test_wildcard.xy");
    
    Success("All path wildcard and transaction commit tests passed!");
    return 0;
}
