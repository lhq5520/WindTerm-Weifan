#include "WtTest.h"

#include "wt/core/Base64.h"
#include "wt/core/Json.h"
#include "wt/platform/Files.h"
#include "wt/security/Crypto.h"
#include "wt/security/Vault.h"

#include <filesystem>
#include <random>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace wt;
using namespace wt::security;

namespace {

// Cheap parameters keep the tests fast; production defaults are 64 MiB / 3 passes.
const Argon2Params kTestParams{1024, 1, 1};
// Cross-checked with `openssl kdf -keylen 32 -kdfopt pass:password -kdfopt salt:somesaltsomesalt
// -kdfopt iter:2 -kdfopt memcost:1024 -kdfopt lanes:1 ARGON2ID`.
const std::string kSingleLaneVector = "08a19ee7f6d7f589c2ab6af18d6e724172b19f7d6fd462b38430ab31ceabeaf0";

std::string hex(std::string_view text) {
    return *core::hexDecode(text);
}

std::string tempPath(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("wt-vault-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

} // namespace

WT_TEST(argon2id_rfc9106_test_vector) {
#ifdef WT_CRYPTO_SODIUM
    // libsodium has no lanes > 1 or secret/associated data; check that it says so.
    std::string unsupported;
    WT_CHECK(!argon2id("p", std::string(16, 's'), Argon2Params{32, 3, 4}, 32, &unsupported));
    WT_CHECK(!unsupported.empty());
    return;
#endif
    // RFC 9106, section 5.3.
    std::string error;
    const auto tag = argon2id(std::string(32, '\x01'), std::string(16, '\x02'), Argon2Params{32, 3, 4}, 32, &error,
                              std::string(8, '\x03'), std::string(12, '\x04'));
    WT_REQUIRE(tag.has_value());
    WT_CHECK_EQ(core::hexEncode(tag->view()),
                std::string("0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659"));
}

WT_TEST(argon2id_single_lane_known_answer) {
    // Same inputs on both backends must give the same key (vault files are portable).
    const auto key = argon2id("password", "somesaltsomesalt", Argon2Params{1024, 2, 1}, 32);
    WT_REQUIRE(key.has_value());
    WT_CHECK_EQ(core::hexEncode(key->view()), kSingleLaneVector);
}

WT_TEST(argon2id_is_deterministic_and_salted) {
    const auto a = argon2id("password", "saltsaltsalt1234", kTestParams, 32);
    const auto b = argon2id("password", "saltsaltsalt1234", kTestParams, 32);
    const auto c = argon2id("password", "saltsaltsalt5678", kTestParams, 32);
    const auto d = argon2id("Password", "saltsaltsalt1234", kTestParams, 32);
    WT_REQUIRE(a && b && c && d);
    WT_CHECK_EQ(a->value(), b->value());
    WT_CHECK(a->value() != c->value());
    WT_CHECK(a->value() != d->value());
    std::string error;
    WT_CHECK(!argon2id("password", "short", kTestParams, 32, &error));
    WT_CHECK(!error.empty());
}

WT_TEST(chacha20_poly1305_rfc8439_test_vector) {
    // RFC 8439, section 2.8.2.
    const std::string key = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const std::string nonce = hex("070000004041424344454647");
    const std::string aad = hex("50515253c0c1c2c3c4c5c6c7");
    const std::string plaintext = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for "
                                  "the future, sunscreen would be it.";
    const auto sealed = aeadEncrypt(key, nonce, plaintext, aad);
    WT_REQUIRE(sealed.has_value());
    WT_CHECK_EQ(core::hexEncode(*sealed),
                std::string("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69"
                            "da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad67594"
                            "5585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"
                            "1ae10b594f09e26a7e902ecbd0600691"));
    const auto opened = aeadDecrypt(key, nonce, *sealed, aad);
    WT_REQUIRE(opened.has_value());
    WT_CHECK_EQ(opened->value(), plaintext);

    // Any change is detected.
    std::string tampered = *sealed;
    tampered[5] ^= 1;
    WT_CHECK(!aeadDecrypt(key, nonce, tampered, aad));
    WT_CHECK(!aeadDecrypt(key, nonce, *sealed, aad + "x"));
    std::string otherKey = key;
    otherKey[0] ^= 1;
    WT_CHECK(!aeadDecrypt(otherKey, nonce, *sealed, aad));
    WT_CHECK(!aeadDecrypt(key, nonce, sealed->substr(0, 10), aad));
    WT_CHECK(!aeadEncrypt("short", nonce, plaintext, aad));
}

WT_TEST(random_bytes_and_secret_wipe) {
    const std::string a = randomBytes(32);
    const std::string b = randomBytes(32);
    WT_CHECK_EQ(a.size(), std::size_t(32));
    WT_CHECK(a != b);
    Secret secret(std::string("hunter2"));
    Secret copy = secret;
    secret.wipe();
    WT_CHECK(secret.empty());
    WT_CHECK_EQ(copy.value(), std::string("hunter2"));
    Secret moved = std::move(copy);
    WT_CHECK(copy.empty());
    WT_CHECK_EQ(moved.value(), std::string("hunter2"));
    const std::string version = cryptoLibraryVersion();
    WT_CHECK(version.find("OpenSSL") != std::string::npos || version.find("libsodium") != std::string::npos);
}

WT_TEST(vault_fixture_from_another_backend_opens) {
    // tests/data/vault-v1.json was written by the OpenSSL backend.
    std::string error;
    auto vault = CredentialVault::open(WT_TEST_DATA_DIR "/vault-v1.json", "fixture password", &error);
    WT_REQUIRE(vault != nullptr);
    WT_CHECK(vault->secret("ssh-password") == std::optional<std::string>("s3cret pässwörd"));
    WT_CHECK_EQ(vault->label("ssh-password"), std::string("alice@example.com"));
    WT_CHECK(vault->secret("key-passphrase") == std::optional<std::string>("correct horse battery staple"));
    WT_CHECK(!CredentialVault::open(WT_TEST_DATA_DIR "/vault-v1.json", "fixture passwort", &error));
}

WT_TEST(vault_round_trip_and_wrong_password) {
    const std::string path = tempPath("vault.json");
    std::string error;
    auto vault = CredentialVault::create(path, "master pw", kTestParams, &error);
    WT_REQUIRE(vault != nullptr);
    WT_CHECK(!CredentialVault::exists(path)); // nothing written yet
    const std::string id = CredentialVault::newId();
    vault->set(id, "s3cret-ssh-password", "alice@example.com");
    vault->set("passphrase", std::string("bin\0ary", 7));
    WT_REQUIRE(vault->save().isOk());
    WT_CHECK(CredentialVault::exists(path));
#ifndef _WIN32
    struct stat info {};
    WT_REQUIRE(::stat(path.c_str(), &info) == 0);
    WT_CHECK_EQ(info.st_mode & 0777, 0600u);
#endif
    // Secrets never appear in the file.
    const std::string file = *platform::readFile(path);
    WT_CHECK(file.find("s3cret") == std::string::npos);
    WT_CHECK(file.find("alice") == std::string::npos);

    auto reopened = CredentialVault::open(path, "master pw", &error);
    WT_REQUIRE(reopened != nullptr);
    WT_CHECK(reopened->secret(id) == std::optional<std::string>("s3cret-ssh-password"));
    WT_CHECK_EQ(reopened->label(id), std::string("alice@example.com"));
    WT_CHECK(reopened->secret("passphrase") == std::optional<std::string>(std::string("bin\0ary", 7)));
    WT_CHECK(reopened->params() == kTestParams);
    WT_CHECK_EQ(reopened->ids().size(), std::size_t(2));

    WT_CHECK(!CredentialVault::open(path, "wrong", &error));
    WT_CHECK_EQ(error, std::string("wrong master password or damaged vault"));
    WT_CHECK(!CredentialVault::create(path, "", kTestParams, &error));

    // Remove + save + reopen.
    WT_CHECK(reopened->remove(id));
    WT_CHECK(!reopened->remove(id));
    WT_REQUIRE(reopened->save().isOk());
    auto again = CredentialVault::open(path, "master pw", &error);
    WT_REQUIRE(again != nullptr);
    WT_CHECK(!again->contains(id));
    std::filesystem::remove_all(std::filesystem::path(path).parent_path());
}

WT_TEST(vault_detects_tampering_and_bad_files) {
    const std::string path = tempPath("vault.json");
    std::string error;
    auto vault = CredentialVault::create(path, "pw", kTestParams, &error);
    WT_REQUIRE(vault != nullptr);
    vault->set("a", "secret");
    WT_REQUIRE(vault->save().isOk());
    auto json = *core::Json::parse(*platform::readFile(path));

    // Weakening the KDF parameters in the header breaks authentication (AAD).
    auto weakened = json;
    weakened["kdf"]["iterations"] = 2;
    WT_REQUIRE(platform::writeFileAtomic(path, weakened.dump()).isOk());
    WT_CHECK(!CredentialVault::open(path, "pw", &error));
    WT_CHECK_EQ(error, std::string("wrong master password or damaged vault"));

    auto flipped = json;
    std::string ciphertext = *core::base64Decode(json["ciphertext"].toString());
    ciphertext[0] ^= 0x40;
    flipped["ciphertext"] = core::base64Encode(ciphertext);
    WT_REQUIRE(platform::writeFileAtomic(path, flipped.dump()).isOk());
    WT_CHECK(!CredentialVault::open(path, "pw", &error));

    auto absurd = json;
    absurd["kdf"]["memoryKiB"] = 64.0 * 1024 * 1024 * 1024; // would need 64 TiB
    WT_REQUIRE(platform::writeFileAtomic(path, absurd.dump()).isOk());
    WT_CHECK(!CredentialVault::open(path, "pw", &error));
    WT_CHECK(error.find("invalid key derivation parameters") != std::string::npos);

    WT_REQUIRE(platform::writeFileAtomic(path, "{\"format\":\"something else\"}").isOk());
    WT_CHECK(!CredentialVault::open(path, "pw", &error));
    WT_CHECK_EQ(error, std::string("not a credential vault file"));
    WT_REQUIRE(platform::writeFileAtomic(path, "not json").isOk());
    WT_CHECK(!CredentialVault::open(path, "pw", &error));
    WT_CHECK(!CredentialVault::open(path + ".missing", "pw", &error));
    std::filesystem::remove_all(std::filesystem::path(path).parent_path());
}

WT_TEST(vault_change_master_password_rekeys) {
    const std::string path = tempPath("vault.json");
    std::string error;
    auto vault = CredentialVault::create(path, "old", kTestParams, &error);
    WT_REQUIRE(vault != nullptr);
    vault->set("k", "v");
    WT_REQUIRE(vault->save().isOk());
    const auto before = *core::Json::parse(*platform::readFile(path));
    const Argon2Params stronger{2048, 2, 1};
    WT_REQUIRE(vault->changeMasterPassword("new", stronger).isOk());
    const auto after = *core::Json::parse(*platform::readFile(path));
    WT_CHECK(before["kdf"]["salt"].toString() != after["kdf"]["salt"].toString());
    WT_CHECK(before["nonce"].toString() != after["nonce"].toString());
    WT_CHECK(!CredentialVault::open(path, "old", &error));
    auto reopened = CredentialVault::open(path, "new", &error);
    WT_REQUIRE(reopened != nullptr);
    WT_CHECK(reopened->params() == stronger);
    WT_CHECK(reopened->secret("k") == std::optional<std::string>("v"));
    WT_CHECK(!vault->changeMasterPassword("").isOk());

    // Each save uses a fresh nonce.
    WT_REQUIRE(reopened->save().isOk());
    const auto resaved = *core::Json::parse(*platform::readFile(path));
    WT_CHECK(resaved["nonce"].toString() != after["nonce"].toString());
    std::filesystem::remove_all(std::filesystem::path(path).parent_path());
}
