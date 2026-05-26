#ifndef XYLEM_CRYPTITEM_HPP
#define XYLEM_CRYPTITEM_HPP

#include <Collection/String.hpp>
#include <Collection/Array.hpp>
#include <Sec/Crypto.hpp>
#include <Xi/Random.hpp>
#include <Xylem/Format.hpp>
#include <stdio.h>

namespace Xylem {

using namespace Collection;

// NOTE: CRC32 is used for accidental corruption detection, NOT for 
// authenticated encryption. For adversarial tamper resistance, this 
// should be replaced with Poly1305 or HMAC-BLAKE2b. The current 
// design assumes a trusted-storage threat model (power-loss, bit-rot).
struct CryptItem {
    static String encrypt(const String& plaintext, const String& key) {
        if (key.isEmpty()) return plaintext;
        u64 nonce = ((u64)Xi::randomNext() << 32) | Xi::randomNext();
        u32 crc = crc32(plaintext);
        String cipher = Sec::streamXor(key, nonce, plaintext);
        
        String res;
        res.allocate(6 + 8 + 4 + cipher.size());
        res[0] = 0; res[1] = '_'; res[2] = 'E'; res[3] = 'N'; res[4] = 'C'; res[5] = '_';
        for(int i=0; i<8; ++i) res[6+i] = (nonce >> (i*8)) & 0xFF;
        for(int i=0; i<4; ++i) res[14+i] = (crc >> (i*8)) & 0xFF;
        for(usz i=0; i<cipher.size(); ++i) res[18+i] = cipher[i];
        return res;
    }

    static bool isEncrypted(const String& data) {
        return data.size() >= 18 && data[0] == 0 && data[1] == '_' && data[2] == 'E' && data[3] == 'N' && data[4] == 'C' && data[5] == '_';
    }

    static String decrypt(const String& data, const Array<String>& keys, String* successfulKey = nullptr) {
        if (!isEncrypted(data)) return data;
        u64 nonce = 0;
        for(int i=0; i<8; ++i) nonce |= ((u64)(u8)data[6+i]) << (i*8);
        u32 crc = 0;
        for(int i=0; i<4; ++i) crc |= ((u32)(u8)data[14+i]) << (i*8);
        String cipher = data.slice(18);
        
        for(const auto& key : keys) {
            String plain = Sec::streamXor(key, nonce, cipher);
            if (crc32(plain) == crc) {
                if (successfulKey) *successfulKey = key;
                return plain;
            }
        }
        return data; 
    }
};

} // namespace Xylem

#endif // XYLEM_CRYPTITEM_HPP
