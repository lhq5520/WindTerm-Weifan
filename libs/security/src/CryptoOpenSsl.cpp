#include "wt/security/Crypto.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>

namespace wt::security {
namespace {

std::string opensslError(const char* what) {
    const unsigned long code = ERR_get_error();
    char buffer[256] = {};
    if (code != 0) {
        ERR_error_string_n(code, buffer, sizeof(buffer));
    }
    ERR_clear_error();
    return code != 0 ? std::string(what) + ": " + buffer : std::string(what);
}

struct KdfDeleter {
    void operator()(EVP_KDF* kdf) const { EVP_KDF_free(kdf); }
    void operator()(EVP_KDF_CTX* ctx) const { EVP_KDF_CTX_free(ctx); }
    void operator()(EVP_CIPHER_CTX* ctx) const { EVP_CIPHER_CTX_free(ctx); }
};

} // namespace

void Secret::wipe() {
    if (!m_value.empty()) {
        OPENSSL_cleanse(m_value.data(), m_value.size());
        m_value.clear();
    }
}

std::optional<Secret> argon2id(std::string_view password, std::string_view salt, const Argon2Params& params,
                               std::size_t length, std::string* error, std::string_view secret,
                               std::string_view associatedData) {
    auto fail = [error](std::string message) -> std::optional<Secret> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };
    if (salt.size() < 8 || length < 4 || params.parallelism == 0 || params.iterations == 0 ||
        params.memoryKiB < 8 * params.parallelism) {
        return fail("invalid Argon2id parameters");
    }
    std::unique_ptr<EVP_KDF, KdfDeleter> kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr));
    if (!kdf) {
        return fail(opensslError("Argon2id is not available (OpenSSL 3.2 or later is required)"));
    }
    std::unique_ptr<EVP_KDF_CTX, KdfDeleter> ctx(EVP_KDF_CTX_new(kdf.get()));
    if (!ctx) {
        return fail(opensslError("cannot create the KDF context"));
    }
    std::uint32_t iterations = params.iterations;
    std::uint32_t memory = params.memoryKiB;
    std::uint32_t lanes = params.parallelism;
    std::uint32_t threads = 1; // no OpenSSL thread pool required
    // OSSL_PARAM takes non-const pointers; the KDF only reads them.
    auto bytes = [](std::string_view text) { return const_cast<char*>(text.data()); };
    OSSL_PARAM list[9];
    std::size_t n = 0;
    list[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD, bytes(password), password.size());
    list[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, bytes(salt), salt.size());
    list[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations);
    list[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory);
    list[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes);
    list[n++] = OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads);
    if (!secret.empty()) {
        list[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, bytes(secret), secret.size());
    }
    if (!associatedData.empty()) {
        list[n++] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_ARGON2_AD, bytes(associatedData),
                                                      associatedData.size());
    }
    list[n] = OSSL_PARAM_construct_end();
    std::string out(length, '\0');
    if (EVP_KDF_derive(ctx.get(), reinterpret_cast<unsigned char*>(out.data()), out.size(), list) != 1) {
        OPENSSL_cleanse(out.data(), out.size());
        return fail(opensslError("Argon2id derivation failed"));
    }
    return Secret(std::move(out));
}

std::optional<std::string> aeadEncrypt(std::string_view key, std::string_view nonce, std::string_view plaintext,
                                       std::string_view associatedData, std::string* error) {
    auto fail = [error](std::string message) -> std::optional<std::string> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };
    if (key.size() != kAeadKeySize || nonce.size() != kAeadNonceSize) {
        return fail("invalid key or nonce size");
    }
    std::unique_ptr<EVP_CIPHER_CTX, KdfDeleter> ctx(EVP_CIPHER_CTX_new());
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* iv = reinterpret_cast<const unsigned char*>(nonce.data());
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(kAeadNonceSize), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, k, iv) != 1) {
        return fail(opensslError("cannot initialise ChaCha20-Poly1305"));
    }
    int length = 0;
    if (!associatedData.empty() &&
        EVP_EncryptUpdate(ctx.get(), nullptr, &length, reinterpret_cast<const unsigned char*>(associatedData.data()),
                          static_cast<int>(associatedData.size())) != 1) {
        return fail(opensslError("AAD failed"));
    }
    std::string out(plaintext.size() + kAeadTagSize, '\0');
    auto* o = reinterpret_cast<unsigned char*>(out.data());
    if (EVP_EncryptUpdate(ctx.get(), o, &length, reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        return fail(opensslError("encryption failed"));
    }
    int final = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), o + length, &final) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kAeadTagSize),
                            o + plaintext.size()) != 1) {
        return fail(opensslError("encryption failed"));
    }
    return out;
}

std::optional<Secret> aeadDecrypt(std::string_view key, std::string_view nonce, std::string_view ciphertextAndTag,
                                  std::string_view associatedData) {
    if (key.size() != kAeadKeySize || nonce.size() != kAeadNonceSize || ciphertextAndTag.size() < kAeadTagSize) {
        return std::nullopt;
    }
    const std::size_t size = ciphertextAndTag.size() - kAeadTagSize;
    std::unique_ptr<EVP_CIPHER_CTX, KdfDeleter> ctx(EVP_CIPHER_CTX_new());
    const auto* k = reinterpret_cast<const unsigned char*>(key.data());
    const auto* iv = reinterpret_cast<const unsigned char*>(nonce.data());
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(kAeadNonceSize), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, k, iv) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    int length = 0;
    if (!associatedData.empty() &&
        EVP_DecryptUpdate(ctx.get(), nullptr, &length, reinterpret_cast<const unsigned char*>(associatedData.data()),
                          static_cast<int>(associatedData.size())) != 1) {
        ERR_clear_error();
        return std::nullopt;
    }
    std::string out(size, '\0');
    auto* o = reinterpret_cast<unsigned char*>(out.data());
    std::string tag(ciphertextAndTag.substr(size));
    int final = 0;
    const bool ok =
        EVP_DecryptUpdate(ctx.get(), o, &length, reinterpret_cast<const unsigned char*>(ciphertextAndTag.data()),
                          static_cast<int>(size)) == 1 &&
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kAeadTagSize), tag.data()) == 1 &&
        EVP_DecryptFinal_ex(ctx.get(), o + length, &final) == 1;
    ERR_clear_error();
    if (!ok) {
        OPENSSL_cleanse(out.data(), out.size());
        return std::nullopt;
    }
    return Secret(std::move(out));
}

std::string randomBytes(std::size_t count) {
    std::string out(count, '\0');
    if (count > 0 && RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(count)) != 1) {
        throw std::runtime_error("the system random number generator failed");
    }
    return out;
}

std::string cryptoLibraryVersion() {
    return OpenSSL_version(OPENSSL_VERSION);
}

} // namespace wt::security
