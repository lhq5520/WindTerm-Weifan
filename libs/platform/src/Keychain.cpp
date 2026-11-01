// OS credential stores. macOS: Security.framework (SecItem API). Windows: Credential Manager
// (wincred). Elsewhere: not available (see Keychain.h).

#include "wt/platform/Keychain.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#endif

#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

namespace wt::platform::keychain {

#if defined(__APPLE__)

namespace {

struct CfReleaser {
    void operator()(CFTypeRef ref) const {
        if (ref) {
            CFRelease(ref);
        }
    }
};
template <typename T>
using CfPtr = std::unique_ptr<std::remove_pointer_t<T>, CfReleaser>;

CfPtr<CFStringRef> cfString(const std::string& text) {
    return CfPtr<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(text.data()),
                                                      static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false));
}

std::string osStatusText(OSStatus status) {
    CfPtr<CFStringRef> message(SecCopyErrorMessageString(status, nullptr));
    if (!message) {
        return "error " + std::to_string(status);
    }
    char buffer[512] = {};
    if (!CFStringGetCString(message.get(), buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return "error " + std::to_string(status);
    }
    return buffer;
}

// Base query: generic password item of `service`/`account`.
CfPtr<CFMutableDictionaryRef> baseQuery(CFStringRef service, CFStringRef account) {
    CfPtr<CFMutableDictionaryRef> query(
        CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, service);
    CFDictionarySetValue(query.get(), kSecAttrAccount, account);
    return query;
}

} // namespace

bool available() {
    return true;
}

std::string storeName() {
    return "macOS Keychain";
}

core::Status store(const std::string& service, const std::string& account, const std::string& label,
                   const std::string& secret) {
    auto serviceRef = cfString(service);
    auto accountRef = cfString(account);
    auto labelRef = cfString(label);
    CfPtr<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(secret.data()),
                                       static_cast<CFIndex>(secret.size())));
    if (!serviceRef || !accountRef || !labelRef || !data) {
        return core::Status::error("cannot prepare the keychain item");
    }
    auto query = baseQuery(serviceRef.get(), accountRef.get());
    CfPtr<CFMutableDictionaryRef> update(
        CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(update.get(), kSecValueData, data.get());
    CFDictionarySetValue(update.get(), kSecAttrLabel, labelRef.get());
    OSStatus status = SecItemUpdate(query.get(), update.get());
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query.get(), kSecValueData, data.get());
        CFDictionarySetValue(query.get(), kSecAttrLabel, labelRef.get());
        CFDictionarySetValue(query.get(), kSecAttrAccessible, kSecAttrAccessibleWhenUnlocked);
        status = SecItemAdd(query.get(), nullptr);
    }
    if (status != errSecSuccess) {
        return core::Status::error("macOS Keychain: " + osStatusText(status), static_cast<int>(status));
    }
    return core::Status::ok();
}

std::optional<std::string> load(const std::string& service, const std::string& account, core::Status* status) {
    auto serviceRef = cfString(service);
    auto accountRef = cfString(account);
    if (!serviceRef || !accountRef) {
        if (status) {
            *status = core::Status::error("cannot prepare the keychain query");
        }
        return std::nullopt;
    }
    auto query = baseQuery(serviceRef.get(), accountRef.get());
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus rc = SecItemCopyMatching(query.get(), &result);
    CfPtr<CFTypeRef> owned(result);
    if (rc == errSecItemNotFound) {
        if (status) {
            *status = core::Status::ok();
        }
        return std::nullopt;
    }
    if (rc != errSecSuccess || !result || CFGetTypeID(result) != CFDataGetTypeID()) {
        if (status) {
            *status = core::Status::error("macOS Keychain: " + osStatusText(rc), static_cast<int>(rc));
        }
        return std::nullopt;
    }
    const auto data = static_cast<CFDataRef>(result);
    std::string secret(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(CFDataGetLength(data)));
    if (status) {
        *status = core::Status::ok();
    }
    return secret;
}

core::Status remove(const std::string& service, const std::string& account) {
    auto serviceRef = cfString(service);
    auto accountRef = cfString(account);
    if (!serviceRef || !accountRef) {
        return core::Status::error("cannot prepare the keychain query");
    }
    auto query = baseQuery(serviceRef.get(), accountRef.get());
    const OSStatus rc = SecItemDelete(query.get());
    if (rc != errSecSuccess && rc != errSecItemNotFound) {
        return core::Status::error("macOS Keychain: " + osStatusText(rc), static_cast<int>(rc));
    }
    return core::Status::ok();
}

#elif defined(_WIN32)

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::wstring target(const std::string& service, const std::string& account) {
    return widen(service + "/" + account);
}

core::Status lastError(const char* what) {
    const DWORD code = GetLastError();
    return core::Status::error(std::string("Windows Credential Manager: ") + what + " (error " + std::to_string(code) + ")",
                               static_cast<int>(code));
}

} // namespace

bool available() {
    return true;
}

std::string storeName() {
    return "Windows Credential Manager";
}

core::Status store(const std::string& service, const std::string& account, const std::string& label,
                   const std::string& secret) {
    if (secret.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return core::Status::error("the secret is too long for the Windows Credential Manager");
    }
    std::wstring name = target(service, account);
    std::wstring comment = widen(label);
    std::wstring user = widen(account);
    std::vector<BYTE> blob(secret.begin(), secret.end());
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = name.data();
    credential.Comment = comment.empty() ? nullptr : comment.data();
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = blob.empty() ? nullptr : blob.data();
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = user.data();
    const BOOL ok = CredWriteW(&credential, 0);
    SecureZeroMemory(blob.data(), blob.size());
    return ok ? core::Status::ok() : lastError("cannot store the secret");
}

std::optional<std::string> load(const std::string& service, const std::string& account, core::Status* status) {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target(service, account).c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        const DWORD code = GetLastError();
        if (status) {
            *status = code == ERROR_NOT_FOUND ? core::Status::ok() : lastError("cannot read the secret");
        }
        return std::nullopt;
    }
    std::string secret(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    if (status) {
        *status = core::Status::ok();
    }
    return secret;
}

core::Status remove(const std::string& service, const std::string& account) {
    if (!CredDeleteW(target(service, account).c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        return lastError("cannot delete the secret");
    }
    return core::Status::ok();
}

#else

bool available() {
    return false;
}

std::string storeName() {
    return {};
}

core::Status store(const std::string&, const std::string&, const std::string&, const std::string&) {
    return core::Status::error("no system keychain on this platform");
}

std::optional<std::string> load(const std::string&, const std::string&, core::Status* status) {
    if (status) {
        *status = core::Status::error("no system keychain on this platform");
    }
    return std::nullopt;
}

core::Status remove(const std::string&, const std::string&) {
    return core::Status::error("no system keychain on this platform");
}

#endif

} // namespace wt::platform::keychain
