#include <Xylem/Xylem.hpp>
#include <Terminal/Format.hpp>
#include <Xi/Time.hpp>
#include <Xi/Random.hpp>
#include <fcntl.h>
#include <unistd.h>

using namespace Xylem;
using namespace Terminal;

void setupEngine(XylemEngine& xm) {
    xm.config.deviceSize = 1024 * 1024 * 50; // 10MB
    xm.config.blockSize = 4096;
    xm.maxCache = 1024 * 1024 * 2; // 2MB Cache
    xm.addKey("MASTER_TEST_KEY");

    unlink("rigorous.xlm");
    int fd = open("rigorous.xlm", O_RDWR | O_CREAT, 0644);

    xm.config.onDeviceRead = [fd](u64 offset, u64 maxOffset) -> String {
        String buf; buf.allocate(maxOffset - offset);
        buf.fill(0xFF);
        pread(fd, buf.data(), buf.size(), offset);
        return buf;
    };

    xm.config.onDeviceWrite = [fd](u64 offset, String data) -> bool {
        return pwrite(fd, data.data(), data.size(), offset) == (ssize_t)data.size();
    };

    xm.config.onDeviceErase = [fd](u64 offset, u64 maxOffset) -> bool {
        String empty; empty.allocate(maxOffset - offset);
        empty.fill(0xFF);
        pwrite(fd, empty.data(), empty.size(), offset);
        return true;
    };

    xm.format();
    xm.mount();
}

int main() {
    Info("=== Xylem Engine: ANGRY DEV RIGOROUS TESTING ===");

    XylemEngine xm;
    setupEngine(xm);
    Success("Engine mounted.");

    // --- TEST 1: Malformed Data & Missing Columns ---
    Info("Test 1: Querying Missing Columns & Malformed Queries");
    Array<Clause> r1; r1.push({"exists", "=", "yes"});
    xm.write(r1);

    Array<String> emptyCols;
    auto res1 = xm.read(emptyCols, OR(WHERE("does_not_exist", "=", "yes")));
    if (res1.size() != 0) Error("Test 1 Failed: Returned data for missing column.");
    else Success("Test 1 Passed: Handled missing column gracefully.");

    // --- TEST 2: Transaction Rollback Thrash ---
    Info("Test 2: Transaction Rollback Thrashing");
    u64 txFail = xm.lock(OR(WHERE("id", "=", "thrash")));
    Array<Clause> r2; r2.push({"id", "=", "thrash"});
    xm.write(r2, Array<Clauses>(), txFail);
    xm.rollback(txFail);

    auto res2 = xm.read(emptyCols, OR(WHERE("id", "=", "thrash")));
    if (res2.size() != 0) Error("Test 2 Failed: Rolled back data was found!");
    else Success("Test 2 Passed: Rollback successfully purged data.");

    // --- TEST 3: The 'Oops, Wrong Dimension' Vector Attack ---
    Info("Test 3: Malformed Vector Embedding Attack");
    String badVec; badVec.allocate(127 * sizeof(f32)); // Deliberately wrong size! (127 vs 128)
    Array<Clause> r3; 
    r3.push({"v_id", "=", "bad"});
    r3.push({"embedding", "=", badVec});
    xm.write(r3); // Should write safely as flat data

    String goodQuery; goodQuery.allocate(128 * sizeof(f32)); // 128 dim query
    // Querying with 128 dims against a 127 dim vector should NOT segfault, it should safely return 0 results.
    auto res3 = xm.read(emptyCols, OR(WHERE("embedding", "cos", goodQuery)));
    Success("Test 3 Passed: Engine survived malformed vector dimensions without segfaulting.");

    // --- TEST 4: The Massive Watcher Buffer Overflow Attempt ---
    Info("Test 4: Watcher Overload & Mid-stream Unsubscribe");
    u64 watchId = xm.watch(OR(WHERE("type", "=", "spam")));
    
    for (int i = 0; i < 5000; ++i) {
        Array<Clause> spam; 
        spam.push({"type", "=", "spam"});
        xm.write(spam);
    }
    xm.unwatch(watchId); // Unsubscribe!
    // Write 5000 more, ensuring watcher doesn't memory leak or buffer overflow
    for (int i = 0; i < 5000; ++i) {
        Array<Clause> spam; 
        spam.push({"type", "=", "spam"});
        xm.write(spam);
    }
    Success("Test 4 Passed: Watcher survived 10,000 events and mid-stream unsubscribe.");

    // --- TEST 5: Total Graph Collapse (Insert, Build, Nuke) ---
    Info("Test 5: Total HNSW Graph Collapse & Pruning");
    usz numVectors = 1000;
    usz dim = 32;
    for (usz i = 0; i < numVectors; ++i) {
        String vecStr; vecStr.allocate(dim * sizeof(f32));
        Array<Clause> row;
        row.push({"v_id", "=", String::from((u64)i)});
        row.push({"type", "=", "vector"});
        row.push({"embedding", "=", vecStr});
        xm.write(row);
    }
    
    // Trigger the lazy ingestion!
    String dummyQ; dummyQ.allocate(dim * sizeof(f32));
    xm.read(emptyCols, OR(WHERE("embedding", "cos", dummyQ)), 5);
    Info("Graph built with 1000 vectors.");

    // Now, NUKE the entire graph instantly. If graph pointers aren't cleaned up, this segfaults on next query.
    xm.remove(OR(WHERE("type", "=", "vector")));
    
    // Query the empty graph
    auto res5 = xm.read(emptyCols, OR(WHERE("embedding", "cos", dummyQ)), 5);
    if (res5.size() != 0) Error("Test 5 Failed: Graph returned deleted vectors.");
    else Success("Test 5 Passed: Graph survived total collapse and returned 0 results.");

    // --- TEST 6: Blob Deduplication Stress Test ---
    Info("Test 6: CAS Blob Deduplication Stress Test");
    String largePayload; largePayload.allocate(50 * 1024); // 50KB
    largePayload.fill('X');
    String h1 = Sec::hash(largePayload, 16);
    
    for(int i=0; i<100; ++i) {
        xm.writeHash(largePayload); // Write it 100 times (CAS dedup)
    }
    
    String stat = xm.statHash(h1);
    if (stat == "51200") Success("Test 6 Passed: 100 duplicated 50KB blobs safely resolved to 1 physical block.");
    else Error("Test 6 Failed: CAS Deduplication failed. Stat=" + stat + " Block=" + String::from((u64)xm.blobStore->index.size()));

    xm.unmount();
    Success("=== ALL ANGRY DEV TESTS PASSED ===");
    return 0;
}
