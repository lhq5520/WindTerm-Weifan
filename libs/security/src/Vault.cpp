#include "wt/security/Vault.h"

#include "wt/core/Base64.h"
#include "wt/core/Json.h"
#include "wt/platform/Files.h"

#include <chrono>
#include <filesystem>

namespace wt::security {
namespace {

constexpr char kFormat[] = "windterm-weifan-vault";
constexpr char kCipher[] = "chacha20-poly1305";
constexpr std::size_t kSaltSize = 16;
// Refuse to open files that would make us allocate absurd amounts of memory or time.
constexpr std::uint32_t kMaxMemoryKiB = 1024u * 1024; // 1 GiB
constexpr std::uint32_t kMaxIterations = 32;
constexpr std::uint32_t kMaxParallelism = 16;

core::Json headerJson(const Argon2Params& params, const std::string& salt) {
    core::Json header;
    header["format"] = kFormat;
    header["version"] = CredentialVault::kVersion;
    header["cipher"] = kCipher;
    core::Json& kdf = header["kdf"];
    kdf["algorithm"] = "argon2id";
    kdf["memoryKiB"] = params.memoryKiB;
    kdf["iterations"] = params.iterations;
    kdf["parallelism"] = params.parallelism;
    kdf["salt"] = core::base64Encode(salt);
    return header;
}

std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

std::unique_ptr<CredentialVault> CredentialVault::create(std::string path, std::string_view masterPassword,
                                                         const Argon2Params& params, std::string* error) {
    if (masterPassword.empty()) {
        if (error) {
            *error = "the master password must not be empty";
        }
        return nullptr;
    }
    std::unique_ptr<CredentialVault> vault(new CredentialVault());
    vault->m_path = std::move(path);
    vault->m_params = params;
    vault->m_salt = randomBytes(kSaltSize);
    auto key = argon2id(masterPassword, vault->m_salt, params, kAeadKeySize, error);
    if (!key) {
        return nullptr;
    }
    vault->m_key = std::move(*key);
    return vault;
}

bool CredentialVault::exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(platform::pathFromUtf8(path), ec);
}

std::unique_ptr<CredentialVault> CredentialVault::open(std::string path, std::string_view masterPassword,
                                                       std::string* error) {
    auto fail = [error](std::string message) -> std::unique_ptr<CredentialVault> {
        if (error) {
            *error = std::move(message);
        }
        return nullptr;
    };
    std::string readError;
    const auto text = platform::readFile(path, &readError);
    if (!text) {
        return fail(readError);
    }
    const auto json = core::Json::parse(*text, &readError);
    if (!json) {
        return fail("the vault file is not valid JSON: " + readError);
    }
    const core::Json& j = *json;
    if (j["format"].toString() != kFormat) {
        return fail("not a credential vault file");
    }
    if (j["version"].toInt() != kVersion) {
        return fail("unsupported vault version " + std::to_string(j["version"].toInt()));
    }
    if (j["cipher"].toString() != kCipher || j["kdf"]["algorithm"].toString() != "argon2id") {
        return fail("unsupported vault encryption");
    }
    Argon2Params params;
    params.memoryKiB = static_cast<std::uint32_t>(j["kdf"]["memoryKiB"].toInt(0));
    params.iterations = static_cast<std::uint32_t>(j["kdf"]["iterations"].toInt(0));
    params.parallelism = static_cast<std::uint32_t>(j["kdf"]["parallelism"].toInt(0));
    if (params.memoryKiB == 0 || params.memoryKiB > kMaxMemoryKiB || params.iterations == 0 ||
        params.iterations > kMaxIterations || params.parallelism == 0 || params.parallelism > kMaxParallelism) {
        return fail("the vault has invalid key derivation parameters");
    }
    const auto salt = core::base64Decode(j["kdf"]["salt"].toString());
    const auto nonce = core::base64Decode(j["nonce"].toString());
    const auto ciphertext = core::base64Decode(j["ciphertext"].toString());
    if (!salt || salt->size() < kSaltSize || !nonce || nonce->size() != kAeadNonceSize || !ciphertext) {
        return fail("the vault file is damaged");
    }
    std::string kdfError;
    auto key = argon2id(masterPassword, *salt, params, kAeadKeySize, &kdfError);
    if (!key) {
        return fail(kdfError);
    }
    const std::string aad = headerJson(params, *salt).dump();
    const auto plaintext = aeadDecrypt(key->view(), *nonce, *ciphertext, aad);
    if (!plaintext) {
        return fail("wrong master password or damaged vault");
    }
    const auto entries = core::Json::parse(plaintext->view());
    if (!entries) {
        return fail("the vault contents are damaged");
    }
    std::unique_ptr<CredentialVault> vault(new CredentialVault());
    vault->m_path = std::move(path);
    vault->m_params = params;
    vault->m_salt = *salt;
    vault->m_key = std::move(*key);
    for (const auto& [id, value] : (*entries)["entries"].fields()) {
        Entry entry;
        entry.secret = Secret(value["secret"].toString());
        entry.label = value["label"].toString();
        entry.updated = value["updated"].toInt();
        vault->m_entries.emplace(id, std::move(entry));
    }
    return vault;
}

std::optional<std::string> CredentialVault::secret(const std::string& id) const {
    const auto it = m_entries.find(id);
    if (it == m_entries.end()) {
        return std::nullopt;
    }
    return it->second.secret.value();
}

std::string CredentialVault::label(const std::string& id) const {
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? std::string() : it->second.label;
}

void CredentialVault::set(const std::string& id, std::string_view secret, std::string label) {
    Entry& entry = m_entries[id];
    entry.secret = Secret(std::string(secret));
    if (!label.empty() || entry.label.empty()) {
        entry.label = std::move(label);
    }
    entry.updated = now();
}

bool CredentialVault::remove(const std::string& id) {
    return m_entries.erase(id) > 0;
}

std::vector<std::string> CredentialVault::ids() const {
    std::vector<std::string> list;
    for (const auto& [id, entry] : m_entries) {
        list.push_back(id);
    }
    return list;
}

core::Status CredentialVault::save() const {
    core::Json entries;
    core::Json& map = entries["entries"];
    map = core::Json::object();
    for (const auto& [id, entry] : m_entries) {
        core::Json item;
        item["secret"] = entry.secret.value();
        item["label"] = entry.label;
        item["updated"] = entry.updated;
        map[id] = std::move(item);
    }
    Secret plaintext(entries.dump());
    const core::Json header = headerJson(m_params, m_salt);
    const std::string nonce = randomBytes(kAeadNonceSize);
    std::string error;
    const auto ciphertext = aeadEncrypt(m_key.view(), nonce, plaintext.view(), header.dump(), &error);
    if (!ciphertext) {
        return core::Status::error(error);
    }
    core::Json file = header;
    file["nonce"] = core::base64Encode(nonce);
    file["ciphertext"] = core::base64Encode(*ciphertext);
    return platform::writeFileAtomic(m_path, file.dump(2) + "\n", true);
}

core::Status CredentialVault::changeMasterPassword(std::string_view newPassword, std::optional<Argon2Params> params) {
    if (newPassword.empty()) {
        return core::Status::error("the master password must not be empty");
    }
    const Argon2Params newParams = params.value_or(m_params);
    std::string salt = randomBytes(kSaltSize);
    std::string error;
    auto key = argon2id(newPassword, salt, newParams, kAeadKeySize, &error);
    if (!key) {
        return core::Status::error(error);
    }
    const Argon2Params oldParams = m_params;
    const std::string oldSalt = m_salt;
    Secret oldKey = m_key;
    m_params = newParams;
    m_salt = std::move(salt);
    m_key = std::move(*key);
    if (auto status = save(); !status) {
        m_params = oldParams;
        m_salt = oldSalt;
        m_key = std::move(oldKey);
        return status;
    }
    return core::Status::ok();
}

std::string CredentialVault::newId() {
    return core::hexEncode(randomBytes(12));
}

} // namespace wt::security
