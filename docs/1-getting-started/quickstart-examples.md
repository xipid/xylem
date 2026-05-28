# Quickstart Examples

Xylem operates on the concept of a **Virtual Block Device**. Rather than implementing filesystem-specific file descriptors directly, Xylem calls user-provided read, write, and erase lambda hooks. This makes it fully portable.

## 1. File-Backed Storage (POSIX Environment)
For desktop apps or embedded systems with a POSIX layer, you can easily bind Xylem to a single file using `open`, `pread`, and `pwrite`:

```cpp
#include <Xylem/Xylem.hpp>
#include <fcntl.h>
#include <unistd.h>

int main() {
    Xylem::XylemEngine xm;
    
    // Configure device dimensions
    xm.config.deviceSize = 1024 * 1024 * 50; // 50MB Database
    xm.config.blockSize = 4096;              // 4KB blocks
    xm.maxCache = 1024 * 1024 * 2;           // 2MB Cache

    // Open file descriptor
    int fd = open("my_database.xy", O_RDWR | O_CREAT, 0644);

    // Bind low-level storage callbacks
    xm.config.onDeviceRead = [fd](u64 offset, u64 maxOffset) -> String {
        String buf;
        buf.allocate(maxOffset - offset);
        pread(fd, buf.data(), buf.size(), offset);
        return buf;
    };

    xm.config.onDeviceWrite = [fd](u64 offset, String data) -> bool {
        return pwrite(fd, data.data(), data.size(), offset) == (ssize_t)data.size();
    };

    xm.config.onDeviceErase = [fd](u64 offset, u64 maxOffset) -> bool {
        String empty; empty.allocate(maxOffset - offset);
        empty.fill(0xFF); // Flash-style erasure simulation
        pwrite(fd, empty.data(), empty.size(), offset);
        return true;
    };

    // Format if database is new; mount otherwise
    if (!xm.mount()) {
        xm.format();
        xm.mount();
    }

    // Now write some data!
    xm.write({{"user", "=", "John"}, {"role", "=", "admin"}});

    // Clean shutdown (flushes cache to file)
    xm.unmount();
    close(fd);
    return 0;
}
```

## 2. In-Memory Mode (For Testing / Transient Data)
You can configure Xylem to run fully in-memory by caching blocks in an array. This is perfect for unit tests:

```cpp
#include <Xylem/Xylem.hpp>

void runMemoryTest() {
    Xylem::XylemEngine xm;
    xm.config.deviceSize = 1024 * 1024; // 1MB
    xm.config.blockSize = 4096;
    xm.maxCache = 1024 * 1024;          // Keep everything cached

    Array<String> blocks;
    blocks.allocate(xm.config.deviceSize / xm.config.blockSize);

    xm.config.onDeviceRead = [&](u64 off, u64 max) -> String {
        return blocks[off / xm.config.blockSize].slice(off % xm.config.blockSize, max - off);
    };

    xm.config.onDeviceWrite = [&](u64 off, String data) -> bool {
        u32 idx = off / xm.config.blockSize;
        for (size_t i = 0; i < data.size(); ++i) {
            blocks[idx][off % xm.config.blockSize + i] = data[i];
        }
        return true;
    };

    xm.config.onDeviceErase = [&](u64 off, u64 max) -> bool {
        u32 idx = off / xm.config.blockSize;
        blocks[idx].allocate(xm.config.blockSize);
        blocks[idx].fill(0xFF);
        return true;
    };

    xm.format();
    xm.mount();

    // The database is now ready for fully volatile operations!
}
```

## 3. Raw NOR Flash (ESP32 Example)
On an ESP32, you can write directly to SPI flash without partitioning or placing a filesystem like SPIFFS over it. Just bind Xylem directly to the Espressif flash APIs:

```cpp
#include <Xylem/Xylem.hpp>
#include "esp_partition.h"

Xylem::XylemEngine xm;

void setupXylemFlash() {
    xm.config.deviceSize = 4 * 1024 * 1024; // 4MB flash chip
    xm.config.blockSize = 4096;             // ESP32 sector size
    
    xm.config.onDeviceRead = [](u64 offset, u64 maxOffset) -> String {
        String buf;
        buf.allocate(maxOffset - offset);
        spi_flash_read(offset, buf.data(), buf.size());
        return buf;
    };

    xm.config.onDeviceWrite = [](u64 offset, String data) -> bool {
        return spi_flash_write(offset, data.data(), data.size()) == ESP_OK;
    };

    xm.config.onDeviceErase = [](u64 offset, u64 maxOffset) -> bool {
        // ESP32 requires sector-aligned erases
        for (u64 addr = offset; addr < maxOffset; addr += 4096) {
            if (spi_flash_erase_sector(addr / 4096) != ESP_OK) return false;
        }
        return true;
    };

    xm.format();
    xm.mount();
}
```

## 4. The Interactive CLI (`xy`)
Xylem ships with a powerful interactive CLI tool for exploring and managing databases natively from your terminal.

### Building the CLI
To build the `xy` binary, simply compile the project:
```bash
./build.sh
```

### Running the CLI
Mount any database file to start an interactive session:
```bash
./build/xy dev/my_database.xy
```
The CLI provides **native arrow key history navigation**, **inline text editing**, and **syntax-highlighted colorized YAML output**.

### Useful CLI Commands
- `ls [path]`: View directory contents and file metadata.
- `cat [path]`: Print the exact binary/text blob content of a file.
- `cd [path]`: Move through the virtual hierarchy (automatically creating directories with `mkdir -p` behavior if they don't exist).
- `IO <linux_path> <xylem_path>`: Recursively bulk-import an entire Linux directory tree into Xylem. This operates using highly optimized transactional batching (200 ops/lock) for blazingly fast ingestion.
- `UNLINK <xylem_path>`: Safely and recursively wipe an entire directory and all its descendants from the database.
- `READ ...`: Execute raw query language statements directly.

## 5. Unique ID Generation
Xylem provides a built-in cryptographically safe random unique ID generator. To generate a 64-bit ID and guarantee it does not currently exist in a specific column in the database, use:

```cpp
String newId = xm.generateId("user_id");
```

### The `:generate` Syntax
To make ID assignment trivial in queries, Xylem supports a special `:generate` column suffix for `WRITE` operations. Xylem will automatically replace the assignment value with a freshly generated, unique 64-bit ID for that base column:

```
WRITE user_id:generate=0 name=Alice role=admin
```

This guarantees `user_id` will be a unique numeric ID for the newly created row.
