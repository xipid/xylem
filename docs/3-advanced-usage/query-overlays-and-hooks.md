# Query Overlays & Custom Hooks

Xylem provides a powerful callback and hook system called **Query Overlays**. This enables developers to intercept, redirect, or extend database operations and queries matching specific criteria. Overlays are ideal for mock data generation, federated/external database routing, custom operator implementations, and query parsing extensibility.

---

## 1. Overlay Reads (`onOverlayRead`)

You can register read overlays that match specific query clauses. When a matching query is run, your callback intercepts it and returns mock or external row results, which are merged and deduplicated alongside database results.

### Registration & Callback Signature
```cpp
void onOverlayRead(const Array<Clauses>& filterClauses, bool supportsPath,
                   Function<Array<Map<String, String>>(const Array<Clauses>& queryClauses,
                                                      const Array<Clauses>& assertClauses)> cb);
```

### Example: Returning Mock/External Data
```cpp
// Intercept reads where tag = "mock_data"
Array<Clauses> readFilter;
readFilter.push(WHERE("tag", "=", "mock_data"));

xm.onOverlayRead(readFilter, false, [](auto& queryClauses, auto& assertClauses) {
    Array<Map<String, String>> mockRows;
    Map<String, String> row;
    row.set("id", "mock_100");
    row.set("tag", "mock_data");
    row.set("value", "intercepted_data");
    mockRows.push(row);
    return mockRows;
});
```

---

## 2. Overlay Writes (`onOverlayWrite`)

Write overlays intercept inserts and updates matching specific clauses. If the callback returns `true`, the write is considered handled and Xylem skips writing it to the local store, effectively enabling virtual/redirected writes.

### Registration & Callback Signature
```cpp
void onOverlayWrite(const Array<Clauses>& filterClauses,
                    Function<bool(const Array<Clause>& columns, const Array<Clauses>& clauses)> cb);
```

### Example: Redirecting Writes to a External Store/Logger
```cpp
Array<Clauses> writeFilter;
writeFilter.push(WHERE("tag", "=", "log_data"));

xm.onOverlayWrite(writeFilter, [](auto& columns, auto& clauses) {
    // Process/save columns to external system...
    printf("Intercepted write to log_data!\n");
    return true; // Return true to skip local Xylem storage
});
```

---

## 3. Custom Clause Operations (`onClauseOperation`)

Xylem allows you to extend the query evaluator with custom operators (e.g. extending comparison logic or adding custom calculations). When a clause matches your registered operator string, your callback is invoked.

### Registration & Callback Signature
```cpp
void onClauseOperation(const String& op, Function<f32(const String& val, const String& targetVal)> cb);
```

### Example: Implementing a Custom Operator
```cpp
// Register a custom operator "approx_match" (returns matching score)
xm.onClauseOperation("approx_match", [](const String& val, const String& targetVal) -> f32 {
    if (val.startsWith(targetVal)) return 1.0f;
    return -1.0f;
});
```
Usage in query clauses:
```cpp
Array<Clauses> clauses;
clauses.push(Clause("name", "approx_match", "Alice"));
auto results = xm.read({"id"}, clauses);
```

---

## 4. Query Overrides (`onQuery`)

You can register callbacks to completely override custom command tokens in Xylem's query interface. When the query processor sees your custom command token as the first word of a query string, it routes execution to your callback instead of the standard SQL query parser.

### Registration & Callback Signature
```cpp
void onQuery(const String& cmdToken, Function<QueryResult(const String& queryStr, XylemEngine* engine)> cb);
```

### Example: Handling Custom CLI Commands
```cpp
xm.onQuery("STATS", [](const String& queryStr, XylemEngine* engine) -> QueryResult {
    QueryResult qr;
    Map<String, String> row;
    row.set("active_locks", String::from((u64)engine->getActiveLocksCount()));
    qr.readRows.push(row);
    return qr;
});
```
Executing this in the CLI or through `xm.query()`:
```sql
STATS
```
will immediately return the custom statistics row.
