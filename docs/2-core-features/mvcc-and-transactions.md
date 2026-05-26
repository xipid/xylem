# MVCC & Transactions

Xylem implements **Multi-Version Concurrency Control (MVCC)** to provide safe, ACID transactions. In MVCC, writers do not block readers, and readers do not block writers. Each transaction operates on an isolated snapshot of the database state.

## Dynamic Row Locking
To begin a transaction, you lock a set of database entities (rows) by specifying identifying clauses. This returns a transaction ID:

```cpp
// Lock all rows associated with Alice's account
u64 txId = xm.lock({WHERE("account", "=", "Alice")});
```

All subsequent operations within the transaction must supply this `txId` to maintain safety.

## Conflict Detection (Write-Write)
If two transactions try to modify the same rows at the same time, Xylem detects a write-write conflict:
*   The first transaction to write and commit will succeed.
*   The second transaction will fail upon `unlock()` with a return code of `-2` (MVCC conflict) and its changes will be automatically rolled back.

```cpp
// Start transaction
u64 tx1 = xm.lock();

// Outside transaction, someone writes to the same rows (advances modSeq)
xm.write({{"counter", "=", "1"}}, {WHERE("id", "=", "shared")});

// Inside transaction, try to write to same row
xm.write({{"counter", "=", "2"}}, {WHERE("id", "=", "shared")}, tx1);

// Unlocking tx1 will return -2 (MVCC conflict)
int res = xm.unlock(tx1);
if (res == -2) {
    // Write-write conflict detected! Changes discarded safely.
}
```

## Atomic Commits & Rollbacks
If all operations inside the transaction are successful, commit the transaction using `unlock()`:
```cpp
xm.unlock(txId);
```
If you encounter an error or want to cancel the changes, discard the transaction using `rollback()`:
```cpp
xm.rollback(txId);
```

---

## ASSERT Queries
You can include an `ASSERT` condition in your read or write queries. If any database item matches the criteria specified by an `ASSERT`, the query fails immediately.
*   For writes, it prevents modification. The method returns the 1-based index of the ASSERT clause that failed the check.
*   For reads, it short-circuits the pipeline and returns 0 rows.

### Usage in Writes:
```cpp
// Try to write, but assert that no entry with 'status=failed' already exists
int result = xm.write(
    {{"new_id", "=", "item_99"}},
    {ASSERT_WHERE("status", "=", "failed")} // Fails write if found
);

if (result > 0) {
    // Assert failed! 'result' represents the index of the failed clause.
}
```
### Usage in Queries (Sanitized String API):
In the custom query language, this is written as:
```
WRITE name=test WHERE age > 25 ASSERT status=active
```
If an active item matches the target, the execution halts.
