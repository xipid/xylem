# Query Language & CLI

Xylem includes a powerful, SQL-like query language natively processed by the `QueryParser` component. The `xy` binary acts as an interactive REPL shell and command-line runner for this language.

## Launching the CLI
To launch the CLI, provide the path to your database block file (it will be created if it doesn't exist):
```bash
./build/xy dev/db.xy
```
Inside the CLI, you can write any query. Type `EOF` or press `Ctrl+C`/`Ctrl+D` to exit. On exit, `xy` automatically executes a vacuum operation and truncates the file to its minimal required size, reclaiming disk space on Linux.

---

## 1. Core CRUD Operations

### `READ`
Read data from the database using exact matching, ranges, or regular expressions.
```sql
-- Read all columns from all rows
READ *

-- Read specific columns where name is "Alice"
READ name id WHERE name=Alice

-- Advanced comparisons
READ * WHERE age>20 OR name=Bob
```

### `WRITE`
Insert or update data. Use `:generate` for unique IDs.
```sql
WRITE name=Alice age=30 id:generate=0
```

### `WRITEVOLATILE`
Works exactly like `WRITE`, but writes rows directly to the block device's unused space (swap-like behavior) rather than journaling. Volatile rows are automatically purged when the database restarts.
```sql
WRITEVOLATILE session=abcdef temp_cache=data
```

### `REMOVE`
Remove rows matching a condition. Creates a soft-delete (tombstone) visible to snapshots.
```sql
REMOVE WHERE age<18
```

### `BURN`
Permanently and forcibly wipes matching rows and shreds their associated content-addressed blobs.
```sql
BURN WHERE classification=secret
```

---

## 2. Graph Traversals

Xylem natively supports graph traversals, allowing you to walk relational hierarchies instantly using `GRAPHREAD`, `GRAPHWRITE`, or `GRAPHWRITEVOLATILE`.

### `GRAPHREAD`
The `GRAPHREAD` command takes an initial set of matched nodes and navigates through the graph:
```sql
GRAPHREAD id name MATCH name=root FOLLOW parent_id=parent.id
```
**Graph Traversal Steps:**
*   `MATCH <clauses>`: Fetches the starting nodes.
*   `FOLLOW <clauses>`: Follows relational links one level deep (e.g. `parent_id=parent.id` compares the `parent_id` of the child with the `id` of the matched parent).
*   `REPEATFOLLOW <clauses>`: Recursively follows links until no more matches are found.
*   `UNTIL <clauses>`: Stops traversing a branch when the condition is met.
*   `EXTRACT <path>`: A macro for extracting directory-like tree structures.

### `GRAPHWRITE`
Update or mutate an entire relational tree at once:
```sql
GRAPHWRITE MATCH name=root REPEATFOLLOW parent_id=parent.id SET perms=777
```
**Mutations:**
*   `SET <clauses>`: Applies the specified columns to all active traversed nodes.
*   `REMOVE`: Deletes all active traversed nodes.

---

## 3. Database Administration

*   `VACUUM [start] [end]`: Shrinks the database and removes dead blocks. In the `xy` CLI, this command physically truncates the Linux file size.
*   `FORMAT`: Formats the device.
*   `MOUNT`: Mounts the device explicitly.
*   `DESTROY`: Completely wipes the database and resets its blocks. In `xy`, this also shrinks the file to zero bytes.
*   `MEMORY <size>`: Sets the memory cache limit (e.g., `MEMORY 50MB`).
*   `FREEZE <pos> <ref>`: Freezes a blob, bypassing copy-on-write mapping.
*   `THAW <ref>`: Allows a frozen blob to be freely overwritten/reallocated again.

---

## 4. MVCC Transactions & Locking

You can create isolated MVCC snapshot transactions and clause-based predicate locks.
*   `LOCK [AS TRUE] [WHERE <clauses>]`: Begins a transaction and acquires a lock.
    - If `AS TRUE` is specified, it acts as a pessimistic/blocking lock: any concurrent modifications to the matching items (or the whole database if no clauses are specified) outside this transaction are blocked.
    - If `WHERE <clauses>` is specified, the lock matches and protects only rows satisfying the predicate filter (both current rows and future inserts).
    - Returns a transaction/lock ID.
*   `UNLOCK <txId>`: Commits and unlocks the transaction.
*   `ROLLBACK <txId>`: Discards and rolls back the transaction.

---

## 5. Reactivity (Pub/Sub)

Xylem allows you to subscribe to database mutations in real-time. This is fully supported in the interactive CLI.

*   `WATCH WHERE <clauses>`: Sets up a watcher. Returns a `watchId`.
*   `PULL <watchId>`: Fetches all queued mutation events for the given watcher since the last pull.
*   `UNWATCH <watchId>`: Destroys the watcher.

---

## 6. Utilities & Filesystem

The `xy` CLI adds filesystem macro commands that execute under the hood:
*   `CD <path>`: Change the current working directory in the `xy` shell for all subsequent filesystem operations.
*   `CAT <path>`: Read file content from the Xylem VFS.
*   `TEE <path> <content>`: Write a file to the Xylem VFS.
*   `IO <linux_path> <xylem_path>`: Ingest a Linux folder into Xylem.
*   `OI <xylem_path> <linux_path>`: Export a Xylem folder into Linux.
*   `LS <path>`: List contents of a Xylem directory.
*   `UNLINK <path>`: Delete a Xylem file/folder.
