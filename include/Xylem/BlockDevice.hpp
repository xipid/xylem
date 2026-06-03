#ifndef XYLEM_BLOCKDEVICE_HPP
#define XYLEM_BLOCKDEVICE_HPP

#include <Security/Crypto.hpp>
#include <Xi/Func.hpp>
#include <Xylem/Format.hpp>

namespace Xylem {

using namespace ::Xi;
using namespace ::Collection;

struct DeviceConfig {
  u64 deviceSize = 0;
  u32 blockSize = 4096;
  u32 readSize = 256;
  u32 writeSize = 256;
  u32 blockCycles = 0;
  bool blockErase = false;
  bool deviceExpands = false;

  Func<bool(u64, u64)> onDeviceErase;
  Func<bool(u64, String)> onDeviceWrite;
  Func<String(u64, u64)> onDeviceRead;
};

class BlockDevice {
public:
  DeviceConfig config;

  BlockDevice() = default;

  // Erases a block
  bool eraseBlock(u32 blockIdx);

  // Writes data to a block, encrypting if masterEncryptionKey is set.
  // eraseCount is used to ensure a unique cryptographic nonce per block write.
  bool writeBlock(u32 blockIdx, u16 eraseCount, const String &data);

  // Reads data from a block, decrypting if masterEncryptionKey is set.
  String readBlock(u32 blockIdx, u16 eraseCount);

private:
  u64 makeNonce(u32 blockIdx, u16 eraseCount) const;
};

} // namespace Xylem

#endif // XYLEM_BLOCKDEVICE_HPP
