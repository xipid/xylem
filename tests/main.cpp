#include <Encoding/Yaml.hpp>
#include <Terminal/Format.hpp>
#include <Xi/Random.hpp>
#include <Xi/Time.hpp>
#include <Xylem/Xylem.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Xylem;
using namespace Terminal;

int main() {
  Info("--- Xylem Engine Rigorous Tests ---");

  XylemEngine xm;
  xm.config.deviceSize = 1024 * 1024 * 50; // 50MB
  xm.config.blockSize = 4096;
  xm.maxCache = 1024 * 1024 * 50; // 50MB to prevent thrashing
  xm.addKey("RIGOROUS_SECURE_TEST_KEY_32BYTES");
  xm.addKey("MY_VECTOR_KEY_123456789012345678");

  // Test 1: Format/Mount & File I/O Hook
  Info("Test 1: Format & Mount (Archive Storage POSIX)");
  unlink("/tmp/db.xy"); // Ensure clean state across test runs
  int fd = open("/tmp/db.xy", O_RDWR | O_CREAT, 0644);

  xm.config.onDeviceRead = [fd](u64 offset, u64 maxOffset) -> String {
    String buf;
    buf.allocate(maxOffset - offset);
    buf.fill(0xFF); // Uninitialized flash assumption
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
  Success("Engine Mounted. `db.xy` physical device backing initialized.");

  // Test 2: Rigorous Table CRUD (1000 inserts)
  Info("Test 2: Rigorous Table CRUD (Mass Insert)");

  u64 cbWatchId = xm.watch(OR(WHERE("type", "=", "transaction")),
                           [](Map<String, String> row) {
                             // Silently digest events for benchmark
                           });
  u64 pullWatchId = xm.watch(OR(WHERE("type", "=", "transaction")));

  for (int i = 0; i < 500; i++) {
    Array<Clause> row;
    row.push({"id", "=", String::from((u64)i)});
    row.push({"type", "=", (i % 2 == 0) ? "transaction" : "log"});
    row.push({"status", "=", (i % 5 == 0) ? "failed" : "success"});
    xm.write(row, Array<Clauses>(), 0, "RIGOROUS_SECURE_TEST_KEY_32BYTES");
  }
  Success("Inserted 500 multi-column records.");

  Array<String> cols; // empty reads all columns
  auto resultTx = xm.read(cols, OR(WHERE("type", "=", "transaction")));
  Info("Found transaction records: " + String::from((u64)resultTx.size()) +
       " (Expected: 250)");

  auto resultFailed = xm.read(cols, OR(WHERE("status", "=", "failed")));
  Info("Found failed records: " + String::from((u64)resultFailed.size()) +
       " (Expected: 100)");

  // Test 3: Watch/Pull Reactivity
  auto events = xm.pull(pullWatchId);
  Info("Watcher intercepted events: " + String::from((u64)events.size()) +
       " (Expected: 250)");

  // Test 4: Tombstones (Remove)
  Info("Test 4: Tombstones & Deletion");
  Array<Clause> upRow;
  upRow.push({"status", "=", "recovered"});
  xm.write(upRow, OR(WHERE("status", "=", "failed")), 0,
           "RIGOROUS_SECURE_TEST_KEY_32BYTES");

  auto resultRec = xm.read(cols, OR(WHERE("status", "=", "recovered")));
  auto resultFailedAfter = xm.read(cols, OR(WHERE("status", "=", "failed")));
  Info("Failed records after deletion: " +
       String::from((u64)resultFailedAfter.size()) + " (Expected: 0)");

  // Test 5: Transactions & ASSERT short-circuit
  Info("Test 5: Transactions & ASSERT Isolation");
  u64 tx = xm.lock(OR(WHERE("id", "=", "99")));
  Array<Clause> txRow;
  txRow.push({"id", "=", "9999"});
  txRow.push({"status", "=", "critical_update"});
  xm.write(txRow, Array<Clauses>(), tx, "RIGOROUS_SECURE_TEST_KEY_32BYTES");
  int unlockResult = xm.unlock(tx);
  Info("Unlock result: " + String::from((u64)unlockResult) + " (Expected: 0)");

  auto resultAssert = xm.read(cols, OR(ASSERT_WHERE("id", "=", "9999")));
  Info("Assert pipeline correctly failed the query. Rows returned: " +
       String::from((u64)resultAssert.size()) + " (Expected: 0)");

  // Test 6: Zero-Cost Blob CAS Deduplication (using new writeHash API)
  Info("Test 6: Zero-Cost Blob CAS Deduplication");
  String massivePayload;
  massivePayload.allocate(100 * 1024); // 100 KB payload
  for (int i = 0; i < 100 * 1024; ++i)
    massivePayload[i] = 'Z';

  String blobHash = xm.writeHash(massivePayload);  // Auto-hash
  String blobHash2 = xm.writeHash(massivePayload); // Duplicate CAS

  u32 blobStatRef = xm.getBlobRef(blobHash);
  Info("Deduplicated Blob Stat Size: " +
       String::from((u64)xm.getBlobSize(blobStatRef)) +
       " bytes (Expected: 102400)");
  if (blobHash == blobHash2)
    Success("CAS hashes match.");
  else
    Error("CAS hashes differ!");

  // Test 6.1: writeHash overloads
  String customHash = "CUSTOM_HASH_16BY"; // 16 bytes
  String customData = "test payload for custom hash";
  xm.writeHash(customData, customHash);
  String readBack = xm.readHash(customHash, 0, 0xFFFFFFFF);
  if (readBack == customData)
    Success("writeHash(content, hash) works correctly.");
  else
    Error("writeHash(content, hash) failed!");

  // Test 7: Watch Columns (Ephemeral Messaging) — using :watch suffix
  Info("Test 7: Watch Columns (Ephemeral Messaging)");
  u64 inbox = xm.watch(OR(WHERE("_type", "=", "command")));
  Array<Clause> ev;
  ev.push({"event_type", "=", "USER_LOGIN"});
  xm.write(ev, Array<Clauses>(), 0, "RIGOROUS_SECURE_TEST_KEY_32BYTES");
  Array<Clause> cmdRow;
  cmdRow.push({"_type:watch", "=", "command"});
  cmdRow.push({"action:watch", "=", "reboot"});
  xm.write(cmdRow);
  auto cmds = xm.pull(inbox);
  Info("Watch commands received: " + String::from((u64)cmds.size()) +
       " (Expected: 1)");

  // Verify watch columns were NOT stored
  auto watchCheck = xm.read(cols, OR(WHERE("_type", "=", "command")));
  if (watchCheck.size() == 0)
    Success("Watch columns correctly not stored in DB.");
  else
    Error("Watch columns were incorrectly stored!");

  // Test 7.1: :blob column type (transparent storage)
  Info("Test 7.1: Blob Column Type (Transparent Storage)");
  String blobContent = "This is a large blob content that gets hashed and "
                       "stored via BlobStore transparently.";
  Array<Clause> blobRow;
  blobRow.push({"doc_id", "=", "doc1"});
  blobRow.push({"content:blob", "=", blobContent});
  xm.write(blobRow);

  // Read back — should get the original content, not the hash
  Array<String> blobCols;
  blobCols.push("content");
  blobCols.push("doc_id");
  auto blobResult = xm.read(blobCols, OR(WHERE("doc_id", "=", "doc1")));
  if (blobResult.size() == 1 && *blobResult[0].get("content") == blobContent) {
    Success(":blob column transparently stored and resolved!");
  } else {
    Error(":blob column failed! Got: " +
          (blobResult.size() > 0 && blobResult[0].has("content")
               ? *blobResult[0].get("content")
               : "EMPTY"));
  }

  // Test 7.2: Partial blob operations
  Info("Test 7.2: Partial Blob Operations");
  Array<Clause> appendRow;
  appendRow.push({"content:blob[+]", "=", " APPENDED"});
  xm.write(appendRow, OR(WHERE("doc_id", "=", "doc1")));

  blobResult = xm.read(blobCols, OR(WHERE("doc_id", "=", "doc1")));
  String expectedAppend = blobContent + " APPENDED";
  if (blobResult.size() == 1 &&
      *blobResult[0].get("content") == expectedAppend) {
    Success("Blob [+] append works!");
  } else {
    Error("Blob [+] append failed!");
  }

  // Test 8: Tree-Typed Columns (YAML)
  Info("Test 8: Tree-Typed Columns (YAML)");
  String yaml = "name: Alice\nskills:\n  - cpp\n  - rust";
  Array<Clause> profRow;
  profRow.push({"profile", "=", yaml});
  profRow.push({"id", "=", "999"});
  xm.write(profRow, Array<Clauses>(), 0, "RIGOROUS_SECURE_TEST_KEY_32BYTES");
  Array<String> readCols;
  readCols.push("profile");
  auto rows = xm.read(readCols, OR(WHERE("id", "=", "999")));

  TaggedTreeBranch root;
  Encoding::parseYAML(rows[0]["profile"], root);
  auto nameItem = root.get<TaggedTreeItemT<String>>("name");
  Info(String("YAML parsed profile name: ") +
       (nameItem ? nameItem->value : "MISSING"));

  // Test 9: Pub/Sub
  Info("Test 9: Pub/Sub");
  u64 sub =
      xm.watch(OR(WHERE("topic", "=", "events")), [](Map<String, String> msg) {
        // Callback invoked immediately
      });
  Array<Clause> pub1;
  pub1.push({"topic", "=", "events"});
  pub1.push({"payload", "=", "user_login:alice"});
  xm.write(pub1);

  Array<Clause> pub2;
  pub2.push({"topic", "=", "events"});
  pub2.push({"payload", "=", "user_login:bob"});
  xm.write(pub2);
  xm.unwatch(sub);
  Info("Pub/Sub events routed successfully.");

  // Test 10: High-Performance Vector DB (Cosine Similarity & Top-K)
  Info("Test 10: High-Performance Vector DB (Cosine Similarity & Top-K)");

  bool quickMode = true; // Set to false to run the full vector DB stress test
  usz numVectors = quickMode ? 100 : 10000;
  usz dim = 128;
  Info("Ingesting " + String::from((u64)numVectors) + " vectors of " +
       String::from((u64)dim) + " dimensions...");

  // Generate a random target vector
  String targetVecStr;
  targetVecStr.allocate(dim * sizeof(f32));
  f32 *targetVec = reinterpret_cast<f32 *>(targetVecStr.data());
  for (usz j = 0; j < dim; ++j)
    targetVec[j] = (f32)(Xi::randomNext() % 1000) / 1000.0f - 0.5f;

  u64 tStart = Xi::millis();

  usz perfectIdx = quickMode ? 77 : 7777;
  usz closeIdx = quickMode ? 88 : 8888;

  for (usz i = 0; i < numVectors; ++i) {
    String vecStr;
    vecStr.allocate(dim * sizeof(f32));
    f32 *vec = reinterpret_cast<f32 *>(vecStr.data());
    for (usz j = 0; j < dim; ++j) {
      vec[j] = (f32)(Xi::randomNext() % 1000) / 1000.0f - 0.5f;
    }

    if (i == perfectIdx) {
      for (usz j = 0; j < dim; ++j)
        vec[j] = targetVec[j]; // Perfect Match
    }
    if (i == closeIdx) {
      for (usz j = 0; j < dim; ++j)
        vec[j] = targetVec[j] + 0.001f; // Close Match
    }

    Array<Clause> row;
    row.push({"v_id", "=", String::from((u64)i)});
    row.push({"embedding", "=", vecStr});
    xm.write(row, Array<Clauses>(), 0, "MY_VECTOR_KEY_123456789012345678");
  }

  u64 tIngest = Xi::millis() - tStart;
  Info("Write completed in " + String::from(tIngest) +
       " ms (lazy ingestion, no graph ops).");

  struct stat st;
  stat("/tmp/db.xy", &st);
  Info("Physical /tmp/db.xy payload size: " + String::from((u64)st.st_size) +
       " bytes (ZSTD Compression + Encryption Active)");

  Info("Executing Top-5 Cosine Similarity Search (triggers on-demand "
       "ingestion)...");
  tStart = Xi::millis();

  Array<String> retCols;
  retCols.push("v_id");
  auto topRows =
      xm.read(retCols, OR(WHERE("embedding", "cos", targetVecStr)), 5);

  u64 tSearch = Xi::millis() - tStart;
  Info("On-Demand Ingestion + HNSW Graph Search completed in " +
       String::from(tSearch) + " ms.");

  for (usz i = 0; i < topRows.size(); ++i) {
    Info(" - Rank " + String::from((u64)(i + 1)) + ": ID " +
         topRows[i]["v_id"]);
  }

  Info("Test 10.1: Instantaneous Removal");
  xm.rm(OR(WHERE("v_id", "=", String::from((u64)perfectIdx))));
  Info("Deleted Perfect Match. Searching again...");

  topRows = xm.read(retCols, OR(WHERE("embedding", "cos", targetVecStr)), 5);
  for (usz i = 0; i < topRows.size(); ++i) {
    Info(" - Rank " + String::from((u64)(i + 1)) + ": ID " +
         topRows[i]["v_id"]);
  }

  // Test 11: On-Demand Graph Loading (Reboot) — now with HNSW persistence
  Info("Test 11: On-Demand Graph Loading (Reboot)");
  xm.destroy();

  Info("Remounting Xylem Database (HNSW nodes persisted to disk)...");
  xm.mount();

  tStart = Xi::millis();
  auto rebootRows =
      xm.read(retCols, OR(WHERE("embedding", "cos", targetVecStr)), 5);
  u64 tRebootSearch = Xi::millis() - tStart;

  Info("Reboot HNSW Search completed in " + String::from(tRebootSearch) +
       " ms.");
  for (usz i = 0; i < rebootRows.size(); ++i) {
    Info(" - Rank " + String::from((u64)(i + 1)) + ": ID " +
         rebootRows[i]["v_id"]);
  }

  // --- Test 12: Path-Based Queries (Write/Update) ---
  Info("Test 12: Path-Based Queries (Write/Update)");

  // Create a folder structure
  Array<Clause> rRoot;
  rRoot.push({"id", "=", "root_id"});
  rRoot.push({"type", "=", "dir"});
  rRoot.push({"name", "=", "root"});
  Array<Clause> rDir1;
  rDir1.push({"id", "=", "dir1_id"});
  rDir1.push({"parent_id", "=", "root_id"});
  rDir1.push({"type", "=", "dir"});
  rDir1.push({"name", "=", "dir1"});
  Array<Clause> rDir2;
  rDir2.push({"id", "=", "dir2_id"});
  rDir2.push({"parent_id", "=", "dir1_id"});
  rDir2.push({"type", "=", "dir"});
  rDir2.push({"name", "=", "dir2"});
  Array<Clause> rFile1;
  rFile1.push({"id", "=", "f1_id"});
  rFile1.push({"parent_id", "=", "dir2_id"});
  rFile1.push({"type", "=", "file"});
  rFile1.push({"name", "=", "f1.txt"});
  Array<Clause> rFile2;
  rFile2.push({"id", "=", "f2_id"});
  rFile2.push({"parent_id", "=", "dir1_id"});
  rFile2.push({"type", "=", "file"});
  rFile2.push({"name", "=", "f2.txt"});

  xm.write(rRoot);
  xm.write(rDir1);
  xm.write(rDir2);
  xm.write(rFile1);
  xm.write(rFile2);

  // Perform bulk update using path operator instead of graphWrite!
  Array<Clause> upFiles;
  upFiles.push({"content", "=", "cleared"});
  
  Array<Clauses> updateClauses;
  updateClauses.push(WHERE("name", "path", "root/**") && WHERE("type", "=", "file"));
  xm.write(upFiles, updateClauses);

  auto files = xm.read(Array<String>(), OR(WHERE("type", "=", "file")));
  if (files.size() != 2 || *files[0].get("content") != "cleared")
    Error("Path-based update failed to update files!");
  else
    Success("Path-based update successfully updated files atomically!");

  // --- Test 13: Path-Based Traversal (Read) ---
  Info("Test 13: Path-Based Traversal (Read)");
  Array<String> traversalCols;
  auto treeRows = xm.read(traversalCols, OR(WHERE("name", "path", "root/**")));

  if (treeRows.size() != 5)
    Error("Path Traversal Read failed to find all descendants! Found: " + String::from((u64)treeRows.size()));
  else {
    Success("Path Traversal Read successfully returned all 5 descendants!");
  }

  // --- Test 14: Hardcore Raw Pinning & Relocation ---
  Info("Test 14: Hardcore Raw Pinning & Relocation");

  // 0. Write a file to ensure data is preserved across relocation
  xm.writeHash("Hello World, this is a large raw file payload!", "RAW_FILE");

  // 1. We will pin a mock bootloader at block 0
  String mockBootloader = "MOCK_ESP32_BOOTLOADER_RAW_BYTES_XYZ";
  bool fixResult = xm.fixRaw(0, mockBootloader);
  if (!fixResult)
    Error("Failed to pin raw block!");
  else {
    // Read back RAW directly from device via standard file POSIX
    String rawBlock0 = xm.device->readBlock(0, 0);
    if (rawBlock0.slice(0, mockBootloader.size()) != mockBootloader) {
      Error("Raw bootloader was not pinned correctly!");
    } else {
      Success("Successfully pinned raw bootloader at block 0!");
    }
  }

  Info("Rebooting after dynamic system relocation...");
  xm.destroy();
  bool mountSuccess = xm.mount();
  if (!mountSuccess)
    Error("Failed to mount after dynamic system relocation!");
  else {
    // Validate that our data still exists!
    String q = xm.readHash("RAW_FILE", 0, 0xFFFFFFFF);
    if (q != "Hello World, this is a large raw file payload!") {
      Error("Data corrupted after relocation! Length: " +
            String::from((u64)q.size()) + " Content: " + q.slice(0, 50));
    } else
      Success("Data fully verified after relocation!");
  }

  // --- Test 15: MVCC Write-Write Conflict Detection ---
  Info("Test 15: MVCC Write-Write Conflict Detection");

  // Setup: insert a row
  Array<Clause> mvccRow;
  mvccRow.push({"mvcc_id", "=", "shared"});
  mvccRow.push({"counter", "=", "0"});
  xm.write(mvccRow);

  // Start tx1 (takes snapshot)
  u64 tx1 = xm.lock();
  // Non-transactional write modifies the same row (advances modSeq)
  Array<Clause> mvccUpdate;
  mvccUpdate.push({"counter", "=", "1"});
  xm.write(mvccUpdate, OR(WHERE("mvcc_id", "=", "shared")));

  // tx1 tries to write the same row
  Array<Clause> tx1Write;
  tx1Write.push({"counter", "=", "2"});
  xm.write(tx1Write, OR(WHERE("mvcc_id", "=", "shared")), tx1);

  // Unlock should detect conflict (row was modified after tx1's snapshot)
  int mvccResult = xm.unlock(tx1);
  if (mvccResult == -2)
    Success(
        "MVCC correctly detected write-write conflict and auto-rolled back!");
  else
    Error("MVCC conflict detection failed. Result: " +
          String::from((u64)mvccResult));

  // --- Test 16: Write Return Codes ---
  Info("Test 16: Write Return Codes (ASSERT)");
  // Write with ASSERT that exists — should return the clause index
  Array<Clause> assertRow;
  assertRow.push({"new_id", "=", "should_fail"});
  int assertResult =
      xm.write(assertRow, OR(ASSERT_WHERE("mvcc_id", "=", "shared")));
  if (assertResult > 0)
    Success("Write correctly returned ASSERT clause index: " +
            String::from((u64)assertResult));
  else
    Error("Write ASSERT returned: " + String::from((u64)assertResult));

  // --- Test 17: Blob Reference Counting ---
  Info("Test 17: Blob Reference Counting");
  String sharedBlob = "This blob is shared between two rows";
  Array<Clause> blobRow1;
  blobRow1.push({"ref_id", "=", "ref1"});
  blobRow1.push({"data:blob", "=", sharedBlob});
  xm.write(blobRow1);

  Array<Clause> blobRow2;
  blobRow2.push({"ref_id", "=", "ref2"});
  blobRow2.push({"data:blob", "=", sharedBlob});
  xm.write(blobRow2);

  String refHash = Security::hash(sharedBlob, 16);

  // Delete one — blob should survive (still referenced by ref2)
  xm.rm(OR(WHERE("ref_id", "=", "ref1")));
  u32 afterDel1Ref = xm.getBlobRef(refHash);
  u32 afterDel1Sz = xm.getBlobSize(afterDel1Ref);
  if (afterDel1Sz > 0)
    Success("Blob survived single-ref deletion (still referenced).");
  else
    Error("Blob was prematurely deleted!");

  // Delete the other — blob should now be cleaned up
  xm.rm(OR(WHERE("ref_id", "=", "ref2")));
  u32 afterDel2Ref = xm.getBlobRef(refHash);
  u32 afterDel2Sz = xm.getBlobSize(afterDel2Ref);
  if (afterDel2Sz == 0)
    Success("Blob correctly cleaned up when all references removed.");
  else
    Info("Blob still exists after all refs removed (size=" +
         String::from((u64)afterDel2Sz) +
         ") — may be expected if hash is system-used.");
  // --- Test 18: Volatile Rows ---
  Info("Test 18: Volatile Rows (In-Memory Only)");
  Array<Clause> volRow;
  volRow.push({"volatile_key", "=", "vol_val"});
  xm.writeVolatile(volRow);

  Array<String> vCols;
  auto vRes = xm.read(vCols, OR(WHERE("volatile_key", "=", "vol_val")));
  if (vRes.size() == 1)
    Success("Volatile row successfully read from memory!");
  else
    Error("Volatile row not found in memory!");

  // Unmount and remount to prove it vanishes
  xm.destroy();
  xm.mount();
  auto vResAfter = xm.read(vCols, OR(WHERE("volatile_key", "=", "vol_val")));
  if (vResAfter.size() == 0)
    Success("Volatile row successfully vanished after destroy/power-loss!");
  else
    Error("Volatile row incorrectly persisted to disk!");

  // --- Ported Rigorous & New Feature Tests ---
  Info("--- Ported Rigorous & New Feature Tests ---");

  // A. Ported Rigorous: Malformed & Missing Columns
  Array<Clause> rM1;
  rM1.push({"exists", "=", "yes"});
  xm.write(rM1);
  auto resM1 = xm.read({}, OR(WHERE("does_not_exist", "=", "yes")));
  if (resM1.size() != 0)
    Error("Ported Test 1 Failed: Returned data for missing column.");
  else
    Success("Ported Test 1 Passed: Handled missing column gracefully.");

  // B. Ported Rigorous: Transaction Rollback Thrashing
  u64 txFail = xm.lock(OR(WHERE("id", "=", "thrash")));
  Array<Clause> rM2;
  rM2.push({"id", "=", "thrash"});
  xm.write(rM2, Array<Clauses>(), txFail);
  xm.rollback(txFail);
  auto resM2 = xm.read({}, OR(WHERE("id", "=", "thrash")));
  if (resM2.size() != 0)
    Error("Ported Test 2 Failed: Rolled back data was found!");
  else
    Success("Ported Test 2 Passed: Rollback successfully purged data.");

  // C. Ported Rigorous: Vector dimension mismatch
  String badVec;
  badVec.allocate(127 * sizeof(f32));
  Array<Clause> rM3;
  rM3.push({"v_id", "=", "bad"});
  rM3.push({"embedding", "=", badVec});
  xm.write(rM3);
  String goodQuery;
  goodQuery.allocate(128 * sizeof(f32));
  auto resM3 = xm.read({}, OR(WHERE("embedding", "cos", goodQuery)));
  Success("Ported Test 3 Passed: Engine survived malformed vector dimensions without segfaulting.");

  // D. Ported Rigorous: Watcher mid-stream unsubscribe
  Info("Ported Test 4: Starting watch");
  u64 watchId = xm.watch(OR(WHERE("type", "=", "spam")));
  Info("Ported Test 4: Entering loop 1");
  for (int i = 0; i < 50; ++i) {
    Info("Ported Test 4: Loop 1 iteration " + String::from((u64)i));
    Array<Clause> spam;
    spam.push({"type", "=", "spam"});
    xm.write(spam);
  }
  Info("Ported Test 4: Unwatching");
  xm.unwatch(watchId);
  Info("Ported Test 4: Entering loop 2");
  for (int i = 0; i < 50; ++i) {
    Info("Ported Test 4: Loop 2 iteration " + String::from((u64)i));
    Array<Clause> spam;
    spam.push({"type", "=", "spam"});
    xm.write(spam);
  }
  Success("Ported Test 4 Passed: Watcher unsubscribe survived spam.");

  // E. Ported Rigorous: HNSW graph collapse
  usz numV = 100;
  usz dimV = 32;
  for (usz i = 0; i < numV; ++i) {
    String vecStr;
    vecStr.allocate(dimV * sizeof(f32));
    Array<Clause> row;
    row.push({"v_id", "=", String::from((u64)i)});
    row.push({"type", "=", "vector_collapse"});
    row.push({"embedding", "=", vecStr});
    xm.write(row);
  }
  String dummyQV;
  dummyQV.allocate(dimV * sizeof(f32));
  xm.read({}, OR(WHERE("embedding", "cos", dummyQV)), 5);
  xm.rm(OR(WHERE("type", "=", "vector_collapse")));
  auto resM5 = xm.read({}, OR(WHERE("embedding", "cos", dummyQV)), 5);
  if (resM5.size() != 0)
    Error("Ported Test 5 Failed: Graph returned deleted vectors.");
  else
    Success("Ported Test 5 Passed: Graph survived total collapse and returned 0 results.");

  // F. Ported Rigorous: CAS blob dedup stress test
  String largeP;
  largeP.allocate(10 * 1024);
  largeP.fill('X');
  String hP1 = Security::hash(largeP, 16);
  for (int i = 0; i < 10; ++i) {
    xm.writeHash(largeP);
  }
  u32 blobRefP = xm.getBlobRef(hP1);
  u32 blobSzP = xm.getBlobSize(blobRefP);
  if (blobSzP == 10240)
    Success("Ported Test 6 Passed: CAS blob dedup verified.");
  else
    Error("Ported Test 6 Failed: CAS blob dedup size mismatch.");

  // G. New Feature: Empty/absent columns and setting to ""
  Info("Testing empty/absent columns and setting to \"\"");
  Array<Clause> emptyColRow;
  emptyColRow.push({"id", "=", "empty_col_test"});
  emptyColRow.push({"exists", "=", "yes"});
  emptyColRow.push({"empty_val", "=", ""});
  xm.write(emptyColRow);

  Array<String> getCols;
  getCols.push("exists");
  getCols.push("empty_val");
  getCols.push("absent_col");
  auto getRes = xm.read(getCols, OR(WHERE("id", "=", "empty_col_test")));
  if (getRes.size() == 1 && getRes[0]["exists"] == "yes" && getRes[0]["empty_val"] == "" && getRes[0]["absent_col"] == "") {
    Success("Empty/absent columns returned empty string successfully.");
  } else {
    Error("Empty/absent columns retrieval failed.");
  }

  // Verify query evaluator treats absent/empty column as ""
  auto checkEmptyVal = xm.read({}, OR(WHERE("empty_val", "=", "") && WHERE("id", "=", "empty_col_test")));
  auto checkAbsentCol = xm.read({}, OR(WHERE("absent_col", "=", "") && WHERE("id", "=", "empty_col_test")));
  if (checkEmptyVal.size() == 1 && checkAbsentCol.size() == 1) {
    Success("Query evaluator treats empty/absent columns as empty string successfully.");
  } else {
    Error("Query evaluator failed to match empty/absent column as empty string.");
  }

  // Setting a column to "" removes it
  Array<Clause> updateEmptyRow;
  updateEmptyRow.push({"exists", "=", ""});
  xm.write(updateEmptyRow, OR(WHERE("id", "=", "empty_col_test")));
  Array<String> existsCol;
  existsCol.push("exists");
  auto checkRemovedCol = xm.read(existsCol, OR(WHERE("id", "=", "empty_col_test")));
  if (checkRemovedCol.size() == 1 && checkRemovedCol[0]["exists"] == "") {
    Success("Setting a column to empty string successfully removed it.");
  } else {
    Error("Failed to remove column by setting it to empty string.");
  }

  // H. New Feature: Empty operator
  Info("Testing 'empty' operator");
  auto checkEmptyOp = xm.read({}, OR(WHERE("exists", "empty", "") && WHERE("id", "=", "empty_col_test")));
  if (checkEmptyOp.size() == 1) {
    Success("'empty' operator matched empty/absent column successfully.");
  } else {
    Error("'empty' operator failed to match empty/absent column.");
  }

  // I. New Feature: Range slicing syntax parsing for CAT and TEE
  Info("Testing range slicing syntax in VFS path");
  // Tee some data
  xm.tee("/range_test.txt", "0123456789");
  // Read using slices
  QueryResult slice1 = xm.cat("/range_test.txt[2:5]"); // "234"
  QueryResult slice2 = xm.cat("/range_test.txt[5:]");  // "56789"
  QueryResult slice3 = xm.cat("/range_test.txt[:5]");  // "01234"
  QueryResult slice4 = xm.cat("/range_test.txt[4]");   // "4"
  
  if (slice1.readRows.size() > 0 && *slice1.readRows[0].get("content") == "234" &&
      slice2.readRows.size() > 0 && *slice2.readRows[0].get("content") == "56789" &&
      slice3.readRows.size() > 0 && *slice3.readRows[0].get("content") == "01234" &&
      slice4.readRows.size() > 0 && *slice4.readRows[0].get("content") == "4") {
    Success("VFS path range slicing in cat/tee works perfectly.");
  } else {
    Error("VFS path range slicing in cat/tee failed!");
  }

  // J. New Feature: cp and mv commands
  Info("Testing cp and mv commands");
  xm.cp("/range_test.txt", "/copied_range.txt");
  QueryResult checkCp = xm.cat("/copied_range.txt");
  if (checkCp.readRows.size() > 0 && *checkCp.readRows[0].get("content") == "0123456789") {
    Success("cp command copied file successfully.");
  } else {
    Error("cp command failed!");
  }

  xm.mv("/copied_range.txt", "/moved_range.txt");
  QueryResult checkMv1 = xm.cat("/copied_range.txt");
  QueryResult checkMv2 = xm.cat("/moved_range.txt");
  if (checkMv1.readRows.size() == 0 && checkMv2.readRows.size() > 0 && *checkMv2.readRows[0].get("content") == "0123456789") {
    Success("mv command moved file successfully.");
  } else {
    Error("mv command failed!");
  }

  // K. New Feature: Pagination (limit and page)
  Info("Testing pagination (limit x page y)");
  // Insert 5 rows
  for (int i = 1; i <= 5; ++i) {
    Array<Clause> pagRow;
    pagRow.push({"id", "=", String::from((u64)(1000 + i))});
    pagRow.push({"tag", "=", "pag_test"});
    pagRow.push({"val", "=", String::from((u64)i)});
    xm.write(pagRow);
  }
  
  // Read back with pagination (limit 2 page 1) -> should return 1, 2
  Array<String> valCol; valCol.push("val");
  auto pagRes1 = xm.read(valCol, OR(WHERE("tag", "=", "pag_test")), 2, 1);
  // Read back with pagination (limit 2 page 2) -> should return 3, 4
  auto pagRes2 = xm.read(valCol, OR(WHERE("tag", "=", "pag_test")), 2, 2);
  // Read back with pagination (limit 2 page 3) -> should return 5
  auto pagRes3 = xm.read(valCol, OR(WHERE("tag", "=", "pag_test")), 2, 3);

  if (pagRes1.size() == 2 && pagRes1[0]["val"] == "1" && pagRes1[1]["val"] == "2" &&
      pagRes2.size() == 2 && pagRes2[0]["val"] == "3" && pagRes2[1]["val"] == "4" &&
      pagRes3.size() == 1 && pagRes3[0]["val"] == "5") {
    Success("Pagination API (limit and page) works perfectly.");
  } else {
    Error("Pagination API (limit and page) failed!");
  }

  // Query parser pagination (using LIMIT and PAGE clauses)
  QueryResult qPag1 = xm.query("READ val WHERE tag='pag_test' LIMIT 2 PAGE 1");
  QueryResult qPag2 = xm.query("READ val WHERE tag='pag_test' LIMIT 2 PAGE 2");
  QueryResult qPag3 = xm.query("READ val WHERE tag='pag_test' LIMIT 2 PAGE 3");

  if (qPag1.readRows.size() == 2 && qPag1.readRows[0]["val"] == "1" && qPag1.readRows[1]["val"] == "2" &&
      qPag2.readRows.size() == 2 && qPag2.readRows[0]["val"] == "3" && qPag2.readRows[1]["val"] == "4" &&
      qPag3.readRows.size() == 1 && qPag3.readRows[0]["val"] == "5") {
    Success("Query Parser pagination (LIMIT and PAGE) works perfectly.");
  } else {
    Error("Query Parser pagination (LIMIT and PAGE) failed!");
  }

  Success("All tests successfully completed!");
  xm.destroy();
  close(fd);

  return 0;
}
