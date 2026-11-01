#include "Agent.h"

#include <cstdlib>
#include <memory>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace wt::ssh::detail {
namespace {

constexpr std::uint8_t kAgentFailure = 5;
constexpr std::uint8_t kSignRequest = 13;
constexpr std::uint8_t kSignResponse = 14;
constexpr std::uint32_t kMaxMessage = 256 * 1024;

void putU32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value >> 24));
    out.push_back(static_cast<char>(value >> 16));
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

void putString(std::string& out, std::string_view value) {
    putU32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value);
}

std::uint32_t getU32(const char* p) {
    return (std::uint32_t(std::uint8_t(p[0])) << 24) | (std::uint32_t(std::uint8_t(p[1])) << 16) |
           (std::uint32_t(std::uint8_t(p[2])) << 8) | std::uint8_t(p[3]);
}

} // namespace

#ifdef _WIN32
// Windows' OpenSSH agent listens on a named pipe, which poll() cannot wait for: a helper thread
// relays agent messages (uint32 length + body, strictly request/response) between the pipe and
// one end of a socket pair.
core::Status connectAgent(net::Socket& out) {
    HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\openssh-ssh-agent", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return core::Status::error("cannot connect to the OpenSSH agent (is the ssh-agent service running?)");
    }
    auto bridge = std::make_shared<net::Socket>();
    net::Socket local;
    if (auto status = net::socketPair(local, *bridge); !status) {
        CloseHandle(pipe);
        return status;
    }
    std::thread([pipe, bridge] {
        auto pipeExact = [pipe](char* data, DWORD size, bool reading) {
            for (DWORD done = 0; done < size;) {
                DWORD n = 0;
                const BOOL ok = reading ? ReadFile(pipe, data + done, size - done, &n, nullptr)
                                        : WriteFile(pipe, data + done, size - done, &n, nullptr);
                if (!ok || n == 0) {
                    return false;
                }
                done += n;
            }
            return true;
        };
        std::string message;
        for (;;) {
            char header[4];
            if (!bridge->receiveExact(header, 4)) {
                break;
            }
            const std::uint32_t length = getU32(header);
            if (length > kMaxMessage) {
                break;
            }
            message.assign(header, 4);
            message.resize(4 + length);
            if (!bridge->receiveExact(message.data() + 4, length) ||
                !pipeExact(message.data(), static_cast<DWORD>(message.size()), false) || !pipeExact(header, 4, true)) {
                break;
            }
            const std::uint32_t replyLength = getU32(header);
            if (replyLength > kMaxMessage) {
                break;
            }
            message.assign(header, 4);
            message.resize(4 + replyLength);
            if (!pipeExact(message.data() + 4, replyLength, true) || !bridge->sendAll(message)) {
                break;
            }
        }
        CloseHandle(pipe);
        bridge->shutdown();
    }).detach();
    out = std::move(local);
    return core::Status::ok();
}
#else
core::Status connectAgent(net::Socket& out) {
    const char* path = std::getenv("SSH_AUTH_SOCK");
    if (!path || !*path) {
        return core::Status::error("SSH_AUTH_SOCK is not set");
    }
    return net::connectUnix(path, out);
}
#endif

bool isSecurityKeyType(std::string_view keyType) {
    return keyType.substr(0, 3) == "sk-" && keyType.find("@openssh.com") != std::string_view::npos;
}

bool readSshString(std::string_view data, std::size_t& pos, std::string_view& out) {
    if (pos > data.size() || data.size() - pos < 4) {
        return false;
    }
    const std::uint32_t length = getU32(data.data() + pos);
    if (data.size() - pos - 4 < length) {
        return false;
    }
    out = data.substr(pos + 4, length);
    pos += 4 + static_cast<std::size_t>(length);
    return true;
}

std::string keyBlobType(std::string_view blob) {
    std::size_t pos = 0;
    std::string_view type;
    return readSshString(blob, pos, type) ? std::string(type) : std::string();
}

std::optional<std::string> agentSign(std::string_view keyBlob, std::string_view data, std::uint32_t flags,
                                     std::chrono::milliseconds timeout, std::string* error) {
    auto fail = [error](std::string message) -> std::optional<std::string> {
        if (error) {
            *error = std::move(message);
        }
        return std::nullopt;
    };
    net::Socket socket;
    if (auto status = connectAgent(socket); !status) {
        return fail(status.message());
    }
    std::string body;
    body.push_back(static_cast<char>(kSignRequest));
    putString(body, keyBlob);
    putString(body, data);
    putU32(body, flags);
    std::string message;
    putU32(message, static_cast<std::uint32_t>(body.size()));
    message += body;
    if (auto status = socket.sendAll(message, timeout); !status) {
        return fail("cannot talk to the agent: " + status.message());
    }
    char header[4];
    if (auto status = socket.receiveExact(header, 4, timeout); !status) {
        return fail("no answer from the agent: " + status.message());
    }
    const std::uint32_t length = getU32(header);
    if (length == 0 || length > kMaxMessage) {
        return fail("invalid answer from the agent");
    }
    std::string reply(length, '\0');
    if (auto status = socket.receiveExact(reply.data(), reply.size(), timeout); !status) {
        return fail("no answer from the agent: " + status.message());
    }
    if (static_cast<std::uint8_t>(reply[0]) == kAgentFailure) {
        return fail("the agent did not sign (for a security key: is it plugged in, and was it touched?)");
    }
    std::size_t pos = 1;
    std::string_view signature;
    if (static_cast<std::uint8_t>(reply[0]) != kSignResponse || !readSshString(reply, pos, signature)) {
        return fail("invalid answer from the agent");
    }
    return std::string(signature);
}

} // namespace wt::ssh::detail
