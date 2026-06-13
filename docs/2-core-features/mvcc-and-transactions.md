# MVCC & Transactions

Xylem implements **Multi-Version Concurrency Control (MVCC)** alongside predicate-based locking to provide safe, highly-concurrent, and ACID-compliant transactions. Under MVCC:
- Readers do not block writers, and writers do not block readers.
- Each transaction operates on an isolated snapshot of the database state captured at a specific sequence number (`snapshotSeq`).
- Historical versions of updated or deleted (tombstone) rows are kept in memory and on disk as long as active transactions reference their sequence numbers.

---

## Clause-Based Predicate Locking

Xylem features **Predicate Locks (Clause-Based Locking)**. Instead of locking specific row IDs, transactions lock database queries by specifying filter clauses.

These locks are dynamic and **support future items**: any write or remove operation attempting to insert or update a row that would match the locked clauses is blocked until the transaction is committed or rolled back.

### API Usage
To begin a transaction and acquire a predicate lock, call `lock` with an array of query clauses:

```cpp
// Start a transaction that locks all items where tag = "locked_tag"
Array<Clauses> lockClauses;
lockClauses.push(WHERE("tag", "=", "locked_tag"));

u64 txLock = xm.lock(lockClauses, 0, true /* requiresExplicitAs */);
```

### Future Insert Prevention
If another process attempts to insert or update a row that satisfies the predicate (`tag = "locked_tag"`), it will be blocked:

```cpp
// Attempting to write a matching row in a different transaction context
Array<Clause> item;
item.push({"id", "=", "9000"});
item.push({"tag", "=", "locked_tag"});
item.push({"content", "=", "blocked"});

int res = xm.write(item, {}, 0); // Returns -1 (Blocked by active clause lock)
```

Non-matching writes (e.g., `tag = "free_tag"`) can proceed concurrently without interruption:

```cpp
Array<Clause> freeItem;
freeItem.push({"id", "=", "9001"});
freeItem.push({"tag", "=", "free_tag"});

int res = xm.write(freeItem, {}, 0); // Returns 0 (Success)
```

---

## MVCC Snapshot Versioning & Reads

Xylem preserves historical row versions to isolate readers from concurrent writes. When a transaction starts, it receives a `snapshotSeq` representing the current sequence number of the database.

### Version History
When a row is updated, a new `RowVersion` is appended to its history rather than overwriting it in-place.
When a row is removed, a tombstone (`isTombstone = true`) version is appended.

### Reading from Snapshots
Queries performed inside a transaction automatically fetch the version of the row that was active at the transaction's `snapshotSeq`.

```cpp
// Set up a row
Array<Clause> row;
row.push({"id", "=", "9100"});
row.push({"value", "=", "v1"});
xm.write(row);

// Start transaction to capture snapshotSeq
u64 tx = xm.lock(Array<Clauses>(), 0, false /* requiresExplicitAs = false allows concurrent writes */);

// Update row value outside transaction
Array<Clause> update;
update.push({"value", "=", "v2"});
xm.write(update, OR(WHERE("id", "=", "9100")), 0);

// Read inside the transaction snapshot -> returns "v1"
auto txResults = xm.read({"value"}, OR(WHERE("id", "=", "9100")), 0, 0, false, tx);
assert(txResults[0]["value"] == "v1");

// Read outside transaction -> returns the latest version "v2"
auto normalResults = xm.read({"value"}, OR(WHERE("id", "=", "9100")), 0, 0);
assert(normalResults[0]["value"] == "v2");
```

### Version Garbage Collection
To prevent the database from growing indefinitely, Xylem implements version garbage collection (`gcVersions`). Superseded historical versions and tombstoned rows are automatically purged once there are no active snapshot transactions referencing sequence numbers older than the version sequence.

---

## Write-Write Conflict Detection

If two concurrent transactions modify overlapping sets of rows, Xylem automatically detects a write-write conflict:
- The transaction that commits (`unlock`) first succeeds.
- Any conflicting transaction will fail to commit, returning `-2` (MVCC conflict) on `unlock()`, and its changes are automatically rolled back.

---

## Pessimistic locks (Blocking writes)

If you need to guarantee that no other client or query writes to the database or to a matching subset of the database, you can specify `requiresExplicitAs = true` when acquiring a lock:

```cpp
// Global Write Lock: blocks all modifications outside this transaction
u64 snapId = xm.lock(Array<Clauses>(), 0, true /* requiresExplicitAs */);
```

When `requiresExplicitAs` is `true`:
- All modifications (`write`, `rm`, etc.) must pass the transaction ID (e.g. `as = snapId`).
- Modifications attempting to bypass the lock (passing `txId = 0` or another `txId`) are blocked and return `-1`.
- If clauses are specified (e.g., `lock(clauses, 0, true)`), the block is limited only to writes matching the filter clauses.

---

## ASSERT Clauses

You can include `ASSERT` clauses in your queries to perform conditional check-and-write operations. If any database item matches the criteria specified by an `ASSERT`, the query fails immediately.

- **Writes**: If any row matches the assert clause, the write fails and returns the 1-based index of the failed `ASSERT` clause.
- **Reads**: If any row matches the assert clause, the read is short-circuited and returns `0` rows.

### Usage in C++:
```cpp
// Write, but fail if there is any row matching 'status = failed'
int result = xm.write(
    {{"id", "=", "item_99"}, {"status", "=", "running"}},
    {ASSERT_WHERE("status", "=", "failed")}
);

if (result > 0) {
    // Assert triggered! 'result' is the index of the failed clause.
}
```
