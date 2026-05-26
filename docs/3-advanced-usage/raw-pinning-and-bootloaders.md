# Raw Pinning & Bootloaders

Xylem's physical layer has a powerful capability: it allows bypassing the file/table management systems and pinning raw data directly to specific byte addresses. 

This makes it possible to discard rigid flash partition tables (like Espressif's partition table or master boot records) and use Xylem for overall space management—including bootloader and application storage.

---

## The Dynamic relocation Concept
Traditional flash databases (like littlefs) write files to sectors but will wear-level blocks, changing their physical addresses on flash. This makes it impossible to store bootloaders or application binaries inside them, because the hardware CPU ROM expects the bootloader at a hardcoded address (e.g. `0x1000` on ESP32).

Xylem solves this with **Relocation Pinning**:
*   You allocate the entire flash to Xylem.
*   You use `.fixRaw(byteOffset, data)` to write your bootloader or app binary to a specific address.
*   Xylem detects if any database data or superblocks were sitting in that sector.
*   If so, it dynamically moves the conflicting database blocks to other free sectors, updates its indices, and marks the sector as reserved/immutable.
*   Wear-leveling and table operations will route around the pinned space.

---

## Code Example: Pinning a Bootloader
Here is how you would configure Xylem to pin bootloader bytes directly to the start of an ESP32 SPI flash:

```cpp
#include <Xylem/Xylem.hpp>

void installFirmware(Xylem::XylemEngine& xm, const String& bootloaderBytes, const String& appBytes) {
    // 1. Mount xylem on the whole flash device
    xm.mount();

    // 2. Pin the bootloader at physical offset 0x1000
    bool resBoot = xm.fixRaw(0x1000, bootloaderBytes);
    
    // 3. Pin the application binary at physical offset 0x10000
    bool resApp = xm.fixRaw(0x10000, appBytes);

    if (resBoot && resApp) {
        printf("Firmware boot components pinned successfully!\n");
    }
}
```

---

## Dynamic OTA (Over-the-Air) Updates
By using Xylem for firmware updates, you avoid dividing your flash into fixed `ota_0` and `ota_1` slots.
1.  **Download Update:** Download the new app binary and save it as a standard CAS blob.
2.  **Pin App:** Pin the blob dynamically to a free region of flash via `fixRaw`.
3.  **Boot Config:** Modify a database setting mapping the active boot partition.
4.  **Reboot:** The custom bootloader reads this config and jumps to the new offset.
5.  **Reclaim Space:** Once booted, delete the old app binary blob from Xylem. Its space is instantly reclaimed and returned to the general storage pool.
