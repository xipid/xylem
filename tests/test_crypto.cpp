#include <Xylem/CryptItem.hpp>
#include <Terminal/Format.hpp>

using namespace Xylem;
using namespace Terminal;

int main() {
    String key = "RIGOROUS_SECURE_TEST_KEY_32BYTES";
    String plain = "transaction";
    
    String enc = CryptItem::encrypt(plain, key);
    Info("Encrypted size: " + String::from((u64)enc.size()));
    
    Array<String> keys;
    keys.push(key);
    
    String dec = CryptItem::decrypt(enc, keys);
    if (dec == plain) {
        Success("Decryption SUCCESS!");
    } else {
        Error("Decryption FAILED!");
    }
    return 0;
}
