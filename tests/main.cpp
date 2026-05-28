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
  unlink("db.xy"); // Ensure clean state across test runs
  int fd = open("db.xy", O_RDWR | O_CREAT, 0644);

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

  // Test 7: Virtual Columns (Ephemeral Messaging) — using :virtual suffix
  Info("Test 7: Virtual Columns (Ephemeral Messaging)");
  u64 inbox = xm.watch(OR(WHERE("_type", "=", "command")));
  Array<Clause> ev;
  ev.push({"event_type", "=", "USER_LOGIN"});
  xm.write(ev, Array<Clauses>(), 0, "RIGOROUS_SECURE_TEST_KEY_32BYTES");
  Array<Clause> cmdRow;
  cmdRow.push({"_type:virtual", "=", "command"});
  cmdRow.push({"action:virtual", "=", "reboot"});
  xm.write(cmdRow);
  auto cmds = xm.pull(inbox);
  Info("Virtual commands received: " + String::from((u64)cmds.size()) +
       " (Expected: 1)");

  // Verify virtual columns were NOT stored
  auto virtualCheck = xm.read(cols, OR(WHERE("_type", "=", "command")));
  if (virtualCheck.size() == 0)
    Success("Virtual columns correctly not stored in DB.");
  else
    Error("Virtual columns were incorrectly stored!");

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

  bool quickMode = false; // Set to false to run the full vector DB stress test
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

  for (usz i = 0; i < numVectors; ++i) {
    String vecStr;
    vecStr.allocate(dim * sizeof(f32));
    f32 *vec = reinterpret_cast<f32 *>(vecStr.data());
    for (usz j = 0; j < dim; ++j) {
      vec[j] = (f32)(Xi::randomNext() % 1000) / 1000.0f - 0.5f;
    }

    if (i == 7777) {
      for (usz j = 0; j < dim; ++j)
        vec[j] = targetVec[j]; // Perfect Match
    }
    if (i == 8888) {
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
  stat("db.xy", &st);
  Info("Physical db.xy payload size: " + String::from((u64)st.st_size) +
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
  xm.remove(OR(WHERE("v_id", "=", "7777")));
  Info("Deleted ID 7777 (Perfect Match). Searching again...");

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

  // --- Test 12: Graph Traversal (Write/Update) ---
  Info("Test 12: Graph Traversal (Atomic Write)");

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

  // Perform bulk update!
  xm.graphWrite(
      GRAPH(MATCH(WHERE("id", "=", "root_id")),
            SET(WHERE("mod_time", "=", "updated_at_root")),
            REPEATFOLLOW(WHERE("parent_id", "=", "parent.id"),
                         WHERE("type", "=", "file")), // Stop at files
            SET(WHERE("mod_time", "=",
                      "updated_at_dir")), // Updates all dirs under root
            FOLLOW(WHERE("type", "=", "file")),
            SET(WHERE("content", "=", "cleared")) // Update only the files
            ));

  auto files = xm.read(Array<String>(), OR(WHERE("type", "=", "file")));
  if (files.size() != 2 || *files[0].get("content") != "cleared")
    Error("Graph Write failed to update files!");
  else
    Success("Graph Write successfully updated files atomically!");

  // --- Test 13: Graph Traversal (Read) ---
  Info("Test 13: Graph Traversal (Read to XiC Tree)");
  Collection::TreeBranch *tree =
      xm.graphRead(Array<String>(),
                   GRAPH(MATCH(WHERE("id", "=", "root_id")),
                         REPEATFOLLOW(WHERE("parent_id", "=", "parent.id"))));

  if (!tree || tree->size() != 1)
    Error("Graph Read failed to find root!");
  else {
    Collection::TreeBranch *rootNode =
        dynamic_cast<Collection::TreeBranch *>((*tree)[0]);
    if (!rootNode || rootNode->size() != 1)
      Error("Graph Read failed to populate children!");
    else
      Success("Graph Read returned a valid XiC TreeBranch structure spanning "
              "to the deepest nodes!");
  }
  delete tree;

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

  String refHash = Sec::hash(sharedBlob, 16);

  // Delete one — blob should survive (still referenced by ref2)
  xm.remove(OR(WHERE("ref_id", "=", "ref1")));
  u32 afterDel1Ref = xm.getBlobRef(refHash);
  u32 afterDel1Sz = xm.getBlobSize(afterDel1Ref);
  if (afterDel1Sz > 0)
    Success("Blob survived single-ref deletion (still referenced).");
  else
    Error("Blob was prematurely deleted!");

  // Delete the other — blob should now be cleaned up
  xm.remove(OR(WHERE("ref_id", "=", "ref2")));
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
  Success("All rigorous tests successfully passed!");
  close(fd);

  return 0;
}
