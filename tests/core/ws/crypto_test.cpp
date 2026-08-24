#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "core/ws/crypto.h"
#include "crypto_test_keys.h"

namespace {

const std::uint8_t KTestSignature[] = {
    0x0d, 0x69, 0xd4, 0x37, 0x49, 0x9b, 0x90, 0xc7, 0xeb, 0xbc, 0x3d, 0xbf, 0x53, 0xe5, 0xdb, 0x05, 0x8f, 0xf3, 0x4c,
    0xbc, 0x0a, 0xaa, 0x64, 0xb3, 0x6f, 0xd9, 0xa0, 0xa8, 0x27, 0x76, 0x1f, 0xe3, 0x87, 0xc3, 0xa9, 0x55, 0x26, 0x03,
    0xb1, 0xcc, 0xae, 0x01, 0x43, 0xd2, 0xb8, 0xe7, 0xe8, 0xee, 0x9f, 0xa3, 0xf8, 0xfa, 0x92, 0x2e, 0xd9, 0x53, 0x5f,
    0x76, 0xc6, 0xe6, 0xb8, 0xfa, 0xb4, 0xd8, 0x8d, 0x57, 0x37, 0xf6, 0xa0, 0x45, 0x86, 0x43, 0x70, 0x78, 0x12, 0x12,
    0x98, 0xb9, 0x96, 0x6f, 0x64, 0x8c, 0x17, 0x62, 0xc5, 0xe7, 0x82, 0xec, 0xfa, 0xc3, 0xc8, 0x63, 0xc9, 0xa1, 0x54,
    0xab, 0xee, 0xaf, 0x7a, 0x0b, 0x9b, 0xc2, 0x90, 0xda, 0xe5, 0xcc, 0x1c, 0x07, 0xd6, 0xac, 0x35, 0xe9, 0xa8, 0x23,
    0x0d, 0xb4, 0xad, 0xdd, 0x41, 0xa7, 0x40, 0x1d, 0x4a, 0xe6, 0xbb, 0x19, 0xcc, 0x7b, 0xcc, 0x2c, 0x4e, 0xcc, 0x24,
    0xf1, 0x7f, 0xca, 0xe6, 0x00, 0x8a, 0x4a, 0x73, 0xf3, 0x67, 0x07, 0xfc, 0xbe, 0x25, 0xfb, 0xa2, 0x07, 0x5c, 0xe3,
    0x64, 0xb9, 0xae, 0x18, 0xb0, 0x56, 0xa5, 0x5f, 0xac, 0xfc, 0x44, 0xa1, 0x53, 0x5b, 0x06, 0x47, 0xac, 0xa0, 0x20,
    0x9e, 0x29, 0x15, 0x6d, 0x39, 0x0e, 0x42, 0x76, 0x6d, 0x18, 0x43, 0xee, 0x73, 0xc4, 0x41, 0x53, 0xa9, 0x2d, 0x3d,
    0x56, 0x42, 0xa1, 0x0f, 0xb1, 0xfb, 0xe5, 0x8b, 0xf1, 0x1f, 0x0b, 0xf4, 0x4b, 0x3e, 0xe6, 0x2a, 0xdf, 0x7c, 0x40,
    0x70, 0xb0, 0x80, 0xd1, 0xda, 0x51, 0x20, 0x33, 0x28, 0x47, 0xa7, 0xda, 0x68, 0x89, 0xbd, 0xb5, 0x75, 0x64, 0xe2,
    0xed, 0x48, 0x14, 0x8b, 0x20, 0xc3, 0xea, 0xbf, 0xa0, 0x5f, 0xce, 0x71, 0x67, 0x01, 0xd1, 0x07, 0xcb, 0x4c, 0x2a,
    0x0c, 0x5e, 0xde, 0xa2, 0x13, 0x11, 0x02, 0x63, 0x3a,
};

constexpr std::size_t KTestSignatureLen = sizeof(KTestSignature);
const char KTestMessage[] = "spark-crypto-test-vector";
constexpr std::size_t KTestMessageLen = sizeof(KTestMessage) - 1;

int runCryptoTests()
{
    const auto &key_pair = spark::test::testKeyPair();
    const std::vector<std::uint8_t> &priv = key_pair.private_key_pkcs8;
    const std::vector<std::uint8_t> &pub = key_pair.public_key_x509;

    // 1. Sign with the test private key and compare to the expected signature.
    auto sig = spark::Crypto::sign(priv, reinterpret_cast<const std::uint8_t *>(KTestMessage), KTestMessageLen);
    if (sig.size() != KTestSignatureLen) {
        std::cerr << "FAIL: signature length " << sig.size() << " != " << KTestSignatureLen << "\n";
        return 1;
    }
    if (std::memcmp(sig.data(), KTestSignature, KTestSignatureLen) != 0) {
        std::cerr << "FAIL: signature does not match expected test vector\n";
        return 1;
    }
    std::cout << "PASS: sign() produces the expected deterministic signature\n";

    // 2. Verify the test signature with the test public key.
    bool ok = spark::Crypto::verify(pub, reinterpret_cast<const std::uint8_t *>(KTestMessage), KTestMessageLen,
                                    KTestSignature, KTestSignatureLen);
    if (!ok) {
        std::cerr << "FAIL: verify() rejected a valid signature\n";
        return 1;
    }
    std::cout << "PASS: verify() accepts the valid signature\n";

    // 3. Verify rejects a tampered message.
    const char bad_msg[] = "spark-crypto-test-vectoX";
    ok = spark::Crypto::verify(pub, reinterpret_cast<const std::uint8_t *>(bad_msg), sizeof(bad_msg) - 1,
                               KTestSignature, KTestSignatureLen);
    if (ok) {
        std::cerr << "FAIL: verify() accepted a signature for a tampered message\n";
        return 1;
    }
    std::cout << "PASS: verify() rejects a tampered message\n";

    // 4. Verify rejects a tampered signature.
    std::vector<std::uint8_t> bad_sig(KTestSignature, KTestSignature + KTestSignatureLen);
    bad_sig[0] ^= 0xff;
    ok = spark::Crypto::verify(pub, reinterpret_cast<const std::uint8_t *>(KTestMessage), KTestMessageLen,
                               bad_sig.data(), bad_sig.size());
    if (ok) {
        std::cerr << "FAIL: verify() accepted a tampered signature\n";
        return 1;
    }
    std::cout << "PASS: verify() rejects a tampered signature\n";

    // 5. decodePublicKey accepts valid key.
    auto decoded = spark::Crypto::decodePublicKey(pub);
    if (decoded.empty()) {
        std::cerr << "FAIL: decodePublicKey() rejected a valid key\n";
        return 1;
    }
    std::cout << "PASS: decodePublicKey() accepts a valid X.509 public key\n";

    // 6. decodePublicKey rejects garbage.
    std::vector<std::uint8_t> garbage = {0x00, 0x01, 0x02};
    decoded = spark::Crypto::decodePublicKey(garbage);
    if (!decoded.empty()) {
        std::cerr << "FAIL: decodePublicKey() accepted garbage\n";
        return 1;
    }
    std::cout << "PASS: decodePublicKey() rejects garbage input\n";

    // 7. generateKeyPair produces usable keys.
    auto kp = spark::Crypto::generateKeyPair();
    if (kp.public_key_x509.empty() || kp.private_key_pkcs8.empty()) {
        std::cerr << "FAIL: generateKeyPair() returned empty keys\n";
        return 1;
    }
    const char gen_msg[] = "generated-key-test";
    auto gen_sig =
        spark::Crypto::sign(kp.private_key_pkcs8, reinterpret_cast<const std::uint8_t *>(gen_msg), sizeof(gen_msg) - 1);
    if (gen_sig.empty()) {
        std::cerr << "FAIL: sign() with generated key produced empty signature\n";
        return 1;
    }
    ok = spark::Crypto::verify(kp.public_key_x509, reinterpret_cast<const std::uint8_t *>(gen_msg), sizeof(gen_msg) - 1,
                               gen_sig.data(), gen_sig.size());
    if (!ok) {
        std::cerr << "FAIL: verify() rejected signature from generated key pair\n";
        return 1;
    }
    std::cout << "PASS: generateKeyPair() produces a valid sign/verify key pair\n";

    // 8. Cross-check: verify the generated key's signature with the test public key fails.
    ok = spark::Crypto::verify(pub, reinterpret_cast<const std::uint8_t *>(gen_msg), sizeof(gen_msg) - 1,
                               gen_sig.data(), gen_sig.size());
    if (ok) {
        std::cerr << "FAIL: verify() accepted a signature from a different key\n";
        return 1;
    }
    std::cout << "PASS: verify() rejects signature from a different key\n";

    std::cout << "All crypto tests passed.\n";
    return 0;
}

}  // namespace

int main()
{
    return runCryptoTests();
}
