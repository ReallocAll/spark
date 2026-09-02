#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "core/util/base64.h"
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

// This PKCS#8 key has a 255-byte private exponent for a 256-byte modulus. DER
// correctly omits the leading zero octet; CNG's RSAFULLPRIVATE blob requires it
// to be restored to cbModulus before the following fixed-width fields are read.
constexpr std::string_view KShortPrivateExponentKey =
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCLPrN5fKns7zVwPlm42tUZK5pE8KrNzeEwoEnDlTMg6YAT1C+3"
    "LEydf/dW6iEr3tV+G1NWzqaiQvw0s3vj8JI6DHvIg+4bHNw/HWjhyW1mbRQ1QWZwMdmy/flNSypyRW9roYIeorl66MJeuwXJvqacv1EC2"
    "vf/5VmdnRopFKCI3AybryNSrHhlJ6kg6/TbCA5mfdDunTxAveW+oAJxAGkzSvusoWyW2aGEakMGy7HiV/VS72HDacT89MixXWFJAjxBP"
    "BRStF6JJyKEgqv1a6PWLGTdaU0KW0K4CxDcOHwxb0oVyvbywLN+bFve9Fttd34imsUB8aTMS90+O+s6G/wpAgMBAAECggEAAKaGcxGuixVW"
    "W69mhKyIfV0ik9aNWP2k7qUy3e1gzRvyJEtj8ZVS33wfcMrjzUfqDf/rc9yomNoMTbW5NryXcqZK9jLF8erjnsit3YxXr74mNVvvpozv"
    "nNmEqLLY5jYUa2v1e0Tt7fAhSlLdY3GW5yr3LfDwnODmKLUKhH6/mvey+ldKXrxJRqAVqUT/dpirQDsWY0E/VhVD4wMQdb6FrgTG50c"
    "3QkcxJ4LD29SGVcWPlUI2CAbB4glSHCME6WV/JNEUupAQJHW2ekxYgv8tFWv9KUWVmSnkykSbLwozgBlZhMH6eZNUe8XLKTLOPXrT5J"
    "AsH5a/nmYypW/nTDqDYQKBgQDAcC6IO/Qot9GLg49+4BC0fDrXyV6+4LlnSlRwYUkSfjmPujD8PSREbOeOs+qQW7xD3t1EPeXRvOx8u"
    "q12rXrXsrXy1/3dSE18AX5RTapyzZTukG3ACIIBlH2xaPQO152T3dD+DWXk2dqW3pr2KmTjZouu/PDf2eF/e1ubZn3QUQKBgQC5PLRU"
    "JRbTmWxM8Sr4a2Vll1mo30+rQkltSkfeimtn93JKxv3o+PaViNcr2ptc6Wt87QMRorraFyzLcEKuHO0f19LZWpSe4U5iysVYLvYp8HQ"
    "B81hvl00VS4JG6IWYco3w43gYjJSCp+kSS+ETr+hZ/CRnf5bqabQetTV6/OGQWQKBgQCyuz6sr6p86wWHY6CyQV+ikkOAyfnipQvuQF"
    "4epmzc7Tl/IXp/vDXkC5Yhtz7j5x/7lZHC4Q6D98lZq3SS5ltS3RwaubuCe7Xjt+tfjhgCWqi5zpDwq7Y7y3PWg9kxs9caUAnc/AqoP"
    "CLGv2gDvKpJfqO72hfKgS3sXmFd+xpdkQKBgQCn7BsBiNnnmtbt7Vbp+tnhvdG+4Cnl8+KCm+sJF+yERHKszTYSw9ct+e4tyDA9izEw"
    "/99fVmkTGh02k58vHfPsgQeYmJ/QZCleL7m01mW74UoZFpQeHUf4vQnt5A5wA7EfJeaSQqbCxxrnxjfVVAtLv+L0nFqgSJDLobRIuQ"
    "R2iQKBgBg4WgR/pYfuTc93uCun3fl+PQmT/rQ1dELQyvryf6DoF1TLjTwidhS5NErQynxq0UlCN02eVH4CQX62MkC9qPSxSGSqmPmdA"
    "pylJoERy+G03s0en9viBTkZnyvDM50VmzsS6n7mzjFFK2RoZiW3CQTtYv1lF8YgzniWs1Q78y03";
constexpr std::string_view KShortPrivateExponentSignature =
    "NWJFb5+roy+3Y6xsLqAIvFQ3oaXfk4gKlOQyqhDUDxHj+EZETDiRy1lLXi7ed+3/QA+B1b5ES54ppL3QMaIQ+TUcmWbqRBa+RyKsu4nC"
    "D43FkZoajhlHC01+D0MB9pVpZ/OAuQy8LFr6/37uPMEc9M+OuI6RoCTlbP1hsN48D/zGbEuH2VzKR6BtsARyRiSuHk/FYYuXw1Tatvo"
    "M1YyEhsxnApygdewf0K0+PXvAyuPAXB+XGkAdXGstU14VKyVR8Zwx0D6Bs3KukRQo/ZLHrBG7LgJyG5IWgRr9oaAYM8d2UDiBlD2d+"
    "75HH4lURUCJaUFFchmZVEtuC8WHeK5Nzw==";
constexpr std::string_view KShortPrivateExponentMessage = "spark-cng-leading-zero-regression";

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
        std::cerr << "FAIL: verify() accepted a signature for a tampered message\n";
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

    // 7. PKCS#8 private integers shorter than their CNG field widths are padded back.
    const auto short_private_key = spark::base64Decode(KShortPrivateExponentKey);
    const auto expected_short_signature = spark::base64Decode(KShortPrivateExponentSignature);
    const auto short_signature = spark::Crypto::sign(
        short_private_key, reinterpret_cast<const std::uint8_t *>(KShortPrivateExponentMessage.data()),
        KShortPrivateExponentMessage.size());
    if (short_signature != expected_short_signature) {
        std::cerr << "FAIL: sign() did not restore the fixed-width CNG private exponent\n";
        return 1;
    }
    std::cout << "PASS: sign() restores shortened PKCS#8 private integers for CNG\n";

    // 8. generateKeyPair produces usable keys.
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

    // 9. Cross-check: verify the generated key's signature with the test public key fails.
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
