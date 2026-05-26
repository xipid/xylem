# Security & Multi-Key Encryption

Xylem is built to secure embedded and local data against physical device extraction. It supports on-the-fly row-level encryption and decryption.

---

## Multi-Key Architecture
Rather than relying on a single "master password" or master key (which poses a single point of failure), Xylem supports a **multi-key security system**. 

You can add multiple decryption keys to the engine. When Xylem encounters an encrypted row or segment, it automatically rotates through its registered keys to attempt decryption.

```cpp
// Register decryption keys in the engine
xm.addKey("RIGOROUS_SECURE_TEST_KEY_32BYTES");
xm.addKey("MY_VECTOR_KEY_123456789012345678");

// Decrypting functions will automatically try both keys
```

---

## Row-Level Encryption
When writing data, you can specify an encryption key. The row data is encrypted before it is flushed to the physical device.

```cpp
Array<Clause> row = {{"secret_id", "=", "007"}, {"code", "=", "X-RAY"}};

// Write row encrypted using a specific key
xm.write(row, {}, 0, "RIGOROUS_SECURE_TEST_KEY_32BYTES");
```

When reading this row later, as long as the key is in the global keys array, the decryption is transparent:

```cpp
// Decrypts automatically
auto results = xm.read({"code"}, {WHERE("secret_id", "=", "007")});
```

If the database file is physically copied or analyzed, the row contents will appear as encrypted binary noise.
