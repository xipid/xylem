# Reactivity & Pub/Sub

Xylem is designed for reactive systems. Instead of constantly polling the database for updates, you can register listeners that trigger callbacks instantly when write operations occur.

---

## Query Watchers
You can register a watcher on a query using conditional clauses. The watcher receives updates whenever matching data is written.

### 1. Event Polling (Pull Watcher)
You can watch a query and manually pull new events when convenient:

```cpp
// Create a watcher for transaction records
u64 watchId = xm.watch(OR(WHERE("type", "=", "transaction")));

// ... later, after data has been written ...
auto events = xm.pull(watchId);

for (auto& row : events) {
    // Process new event rows
}

// Stop watching to free resources
xm.unwatch(watchId);
```

### 2. Callback Watcher (Push Watcher)
You can bind an asynchronous lambda to run immediately when a matching row is written:

```cpp
u64 watchId = xm.watch(
    OR(WHERE("topic", "=", "alarms")),
    [](Map<String, String> row) {
        printf("ALARM TRIGGERED: %s\n", row["message"].data());
    }
);

// Writing matching data triggers the callback instantly
xm.write({{"topic", "=", "alarms"}, {"message", "=", "High temperature!"}});
```

---

## Virtual Columns (Zero-Disk Messaging)
For transient events or fast messaging queues, you don't want to burn flash writes. Xylem provides a specialized data type suffix: `:virtual`.

Virtual columns are **never saved to disk or memory**. They exist only long enough to trigger matching query watchers.

```cpp
// Create an inbox for system command events
u64 inbox = xm.watch(OR(WHERE("_type", "=", "command")));

// Send an ephemeral reboot command
xm.write({
    {"_type:virtual", "=", "command"},
    {"action:virtual", "=", "reboot"}
});

// The receiver watcher pulls the command successfully
auto cmds = xm.pull(inbox); // Contains {"_type": "command", "action": "reboot"}

// Querying the DB directly yields 0 entries because nothing was written to flash!
auto dbCheck = xm.read({}, {WHERE("_type", "=", "command")});
assert(dbCheck.size() == 0); 
```
This is ideal for message passing and inter-task communications in embedded systems.

---

## Volatile Write Operations (Disk-Backed SWAP)
Unlike `:virtual` columns which are completely transient, sometimes you need structured rows to be fully queryable, indexable, and accessible throughout the runtime lifecycle of the application—but they **should not** be persisted permanently across device reboots.

For this, use `WRITEVOLATILE` (in queries) or `xm.writeVolatile(cols, clauses)`.

```cpp
// Write a session token that expires if the device reboots
xm.writeVolatile({
    {"type", "=", "session"},
    {"token", "=", "abc-123"}
});
```

### Automatic SWAP Offloading
To prevent RAM exhaustion, Xylem tracks volatile rows in an in-memory B+ Tree layer. If memory fills up, Xylem seamlessly writes the oldest volatile chunks into **temporary disk-backed SWAP files (`VOLATILE_BLOCK_`)** in the Blob Store.
When Xylem boots up (mounts), it intentionally drops these volatile structures and deletes any lingering `VOLATILE_BLOCK_` chunks from flash, restoring your device to a clean baseline state.
