# CAS Blob Store

Xylem features a specialized Content-Addressable Storage (CAS) Blob Store. Instead of storing file payloads directly within table cells, payloads are stored in the Blob Store and referenced by their 16-byte cryptographic hash (BLAKE2b-128).

---

## Zero-Cost CAS Deduplication
If multiple rows store identical blob content, Xylem only writes the physical data to disk once. Any subsequent write of the same payload returns the same hash and creates a reference, requiring zero additional space.

```cpp
String imageBytes = loadBytes("photo.jpg");

// writeHash automatically hashes the content and stores it
String hash1 = xm.writeHash(imageBytes);

// Attempting to write the same content again is a zero-cost, deduplicated operation
String hash2 = xm.writeHash(imageBytes);

assert(hash1 == hash2); // Guaranteed to match
```

### Overloads:
*   `writeHash(content)`: Automatically hashes content and stores it.
*   `writeHash(content, hash)`: Write content with a custom-provided hash.
*   `writeHash(content, position)`: Overload to write the blob directly to a specific physical byte offset (useful for layout layouts or bootloader pinning).

---

## Transparent Blob Columns
Xylem supports a special column type modifier: `:blob`. When a column is configured as `:blob`, Xylem automatically hashes the payload, writes the content to the Blob Store, and stores the 16-byte reference hash inside the table.

When querying, Xylem resolves the hash and serves the raw content transparently:

```cpp
String massiveText = "A very long document...";

// Write using :blob column modifier
xm.write({
    {"doc_id", "=", "file_01"},
    {"content:blob", "=", massiveText}
});

// Reading the column automatically fetches the content from the Blob Store
auto result = xm.read({"content"}, {WHERE("doc_id", "=", "file_01")});
String retrieved = result[0]["content"]; // Returns "A very long document..."
```

---

## Partial Blob Operations
Xylem allows querying and updating portions of a blob on-disk without loading the entire payload into RAM.

### 1. Partial Reads (Byte Ranges)
```cpp
// Reads only the first 256 bytes of the blob
String header = xm.readHash(blobHash, 0, 256);
```

### 2. Append Mode `[+]`
You can append bytes directly to a stored blob using the `[+]` operator:
```cpp
// Append suffix to the existing blob transparently
xm.write(
    {{"content:blob[+]", "=", " APPENDED_SUFFIX"}},
    {WHERE("doc_id", "=", "file_01")}
);
```

---

## Automatic Garbage Collection (Reference Counting)
Xylem tracks references to blobs stored inside tables.
*   When a row containing a `:blob` is deleted (tombstoned), Xylem checks the entire database to see if any other row references the same blob hash.
*   If no other references exist, and the blob is not pinned to a physical address (`fixHash`), the physical blob data is **deleted automatically** to reclaim space.
