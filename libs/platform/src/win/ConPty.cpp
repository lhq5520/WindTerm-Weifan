// Windows pseudo console backend (ConPTY, Windows 10 1809+). Output is read on a helper thread
// into a buffer; nativeHandle() is a manual-reset event that is signalled while output is buffered
// or the console has closed, so front ends can wait on it (e.g. QWinEventNotifier).
//
// The legacy src/Pty/ConPty.cpp (ptyqt) was the reference for the overall approach.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "wt/platform/Pty.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wt::platform {
namespace {

using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON);

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
    bool available() const { return create && resize && close; }
};

// Resolved at run time so the binary still starts on older Windows versions.
const ConPtyApi& conPtyApi() {
    static const ConPtyApi api = [] {
        ConPtyApi result;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            result.create = reinterpret_cast<CreatePseudoConsoleFn>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "CreatePseudoConsole")));
            result.resize = reinterpret_cast<ResizePseudoConsoleFn>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "ResizePseudoConsole")));
            result.close = reinterpret_cast<ClosePseudoConsoleFn>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "ClosePseudoConsole")));
        }
        return result;
    }();
    return api;
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), length);
    return out;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string systemMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text = buffer ? narrow(buffer) : "error " + std::to_string(code);
    if (buffer) {
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

core::Status lastError(const std::string& what) {
    const DWORD code = GetLastError();
    return core::Status::error(what + ": " + systemMessage(code), static_cast<int>(code));
}

// Quotes one argument following the rules of CommandLineToArgvW / the MSVC runtime.
void appendQuoted(std::wstring& commandLine, const std::wstring& argument) {
    if (!commandLine.empty()) {
        commandLine.push_back(L' ');
    }
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        commandLine += argument;
        return;
    }
    commandLine.push_back(L'"');
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            commandLine.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            commandLine.append(backslashes * 2 + 1, L'\\');
        } else {
            commandLine.append(backslashes, L'\\');
        }
        commandLine.push_back(*it);
    }
    commandLine.push_back(L'"');
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

// UTF-16 environment block: inherited variables (optional) plus overrides, sorted as Windows
// expects, double-NUL terminated.
std::wstring environmentBlock(const PtyOptions& options) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> variables;
    if (options.inheritEnvironment) {
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
                const std::wstring text(entry);
                const auto equals = text.find(L'=', 1); // "=C:=C:\\" style entries start with '='
                if (equals != std::wstring::npos) {
                    variables[text.substr(0, equals)] = text.substr(equals + 1);
                }
            }
            FreeEnvironmentStringsW(block);
        }
    }
    for (const auto& [name, value] : options.environment) {
        variables[widen(name)] = widen(value);
    }
    std::wstring result;
    for (const auto& [name, value] : variables) {
        result += name + L"=" + value;
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

class ConPty final : public Pty {
public:
    ConPty() = default;
    ~ConPty() override { terminate(std::chrono::milliseconds(200)); }

    core::Status start(const PtyOptions& options) override {
        if (m_process.hProcess) {
            return core::Status::error("PTY already started");
        }
        const ConPtyApi& api = conPtyApi();
        if (!api.available()) {
            return core::Status::error("ConPTY needs Windows 10 version 1809 or later");
        }
        std::string program = options.program.empty() ? defaultShell() : options.program;
        const wchar_t* pathVariable = _wgetenv(L"PATH");
        const std::string resolved = findExecutable(program, pathVariable ? narrow(pathVariable) : std::string());
        if (resolved.empty()) {
            return core::Status::error(program + ": program not found", ERROR_FILE_NOT_FOUND);
        }
        program = resolved;

        HANDLE inputRead = nullptr;
        HANDLE outputWrite = nullptr;
        if (!CreatePipe(&inputRead, &m_input, nullptr, 0) || !CreatePipe(&m_output, &outputWrite, nullptr, 0)) {
            return lastError("CreatePipe failed");
        }
        m_size = options.size;
        const COORD size{static_cast<SHORT>(std::max<int>(1, m_size.columns)),
                         static_cast<SHORT>(std::max<int>(1, m_size.rows))};
        const HRESULT hr = api.create(size, inputRead, outputWrite, 0, &m_console);
        // The pseudo console duplicated its ends of the pipes.
        CloseHandle(inputRead);
        CloseHandle(outputWrite);
        if (FAILED(hr)) {
            return core::Status::error("CreatePseudoConsole failed: " + systemMessage(static_cast<DWORD>(hr)),
                                       static_cast<int>(hr));
        }

        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<unsigned char> attributes(attributeSize);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, m_console,
                                       sizeof(m_console), nullptr, nullptr)) {
            return lastError("cannot attach the pseudo console");
        }

        std::wstring commandLine;
        appendQuoted(commandLine, widen(program));
        for (const auto& argument : options.arguments) {
            appendQuoted(commandLine, widen(argument));
        }
        std::wstring environment = environmentBlock(options);
        const std::wstring directory = widen(options.workingDirectory);
        const BOOL created = CreateProcessW(
            nullptr, commandLine.data(), nullptr, nullptr, FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, environment.data(),
            directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &m_process);
        const DWORD createError = GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!created) {
            SetLastError(createError);
            api.close(m_console);
            m_console = nullptr;
            return lastError("cannot start " + program);
        }
        CloseHandle(m_process.hThread);
        m_process.hThread = nullptr;

        m_readable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_reader = std::thread([this] { readLoop(); });
        return core::Status::ok();
    }

    IoResult read(char* buffer, std::size_t size) override {
        std::lock_guard lock(m_mutex);
        if (!m_buffer.empty()) {
            const std::size_t count = std::min(size, m_buffer.size());
            std::copy_n(m_buffer.begin(), count, buffer);
            m_buffer.erase(0, count);
            if (m_buffer.empty() && !m_eof) {
                ResetEvent(m_readable);
            }
            return {IoState::Ok, count, 0};
        }
        if (m_eof) {
            return {IoState::Closed, 0, 0};
        }
        return {IoState::WouldBlock, 0, 0};
    }

    IoResult write(std::string_view data) override {
        if (!m_input) {
            return {IoState::Closed, 0, 0};
        }
        DWORD written = 0;
        if (!WriteFile(m_input, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
            const DWORD code = GetLastError();
            if (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA) {
                return {IoState::Closed, 0, static_cast<int>(code)};
            }
            return {IoState::Error, 0, static_cast<int>(code)};
        }
        return {IoState::Ok, written, 0};
    }

    core::Status resize(const PtySize& size) override {
        if (!m_console) {
            return core::Status::error("PTY not started");
        }
        m_size = size;
        const COORD coord{static_cast<SHORT>(std::max<int>(1, size.columns)),
                          static_cast<SHORT>(std::max<int>(1, size.rows))};
        const HRESULT hr = conPtyApi().resize(m_console, coord);
        return FAILED(hr) ? core::Status::error("ResizePseudoConsole failed", static_cast<int>(hr))
                          : core::Status::ok();
    }

    bool waitForReadable(std::chrono::milliseconds timeout) override {
        return m_readable && WaitForSingleObject(m_readable, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
    }

    bool isRunning() override { return m_process.hProcess && !pollExit(); }

    std::optional<int> pollExit() override { return waitForExit(std::chrono::milliseconds(0)); }

    std::optional<int> waitForExit(std::chrono::milliseconds timeout) override {
        if (m_exitCode) {
            return m_exitCode;
        }
        if (!m_process.hProcess ||
            WaitForSingleObject(m_process.hProcess, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        DWORD code = 0;
        GetExitCodeProcess(m_process.hProcess, &code);
        m_exitCode = static_cast<int>(code);
        return m_exitCode;
    }

    void terminate(std::chrono::milliseconds grace) override {
        if (m_console) {
            // Closing the console sends CTRL_CLOSE_EVENT to its clients and ends the output pipe.
            conPtyApi().close(m_console);
            m_console = nullptr;
        }
        if (m_process.hProcess && !waitForExit(grace)) {
            TerminateProcess(m_process.hProcess, 1);
            waitForExit(std::chrono::milliseconds(1000));
        }
        if (m_input) {
            CloseHandle(m_input);
            m_input = nullptr;
        }
        if (m_reader.joinable()) {
            // Normally closing the console ends the read; cancel it in case conhost lingers.
            if (HANDLE thread = m_readerThread.exchange(nullptr)) {
                CancelSynchronousIo(thread);
                CloseHandle(thread);
            }
            m_reader.join();
        }
        if (HANDLE thread = m_readerThread.exchange(nullptr)) {
            CloseHandle(thread);
        }
        if (m_output) {
            CloseHandle(m_output);
            m_output = nullptr;
        }
        if (m_process.hProcess) {
            CloseHandle(m_process.hProcess);
            m_process.hProcess = nullptr;
        }
        if (m_readable) {
            CloseHandle(m_readable);
            m_readable = nullptr;
        }
    }

    std::intptr_t nativeHandle() const override { return reinterpret_cast<std::intptr_t>(m_readable); }
    std::int64_t processId() const override { return m_process.hProcess ? m_process.dwProcessId : -1; }
    PtySize size() const override { return m_size; }

private:
    void readLoop() {
        HANDLE self = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        m_readerThread = self;
        char chunk[65536];
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(m_output, chunk, sizeof(chunk), &count, nullptr) || count == 0) {
                std::lock_guard lock(m_mutex);
                m_eof = true;
                SetEvent(m_readable);
                return;
            }
            std::lock_guard lock(m_mutex);
            m_buffer.append(chunk, count);
            SetEvent(m_readable);
        }
    }

    HPCON m_console = nullptr;
    HANDLE m_input = nullptr;  // our end of the console's input
    HANDLE m_output = nullptr; // our end of the console's output
    HANDLE m_readable = nullptr;
    PROCESS_INFORMATION m_process{};
    PtySize m_size;
    std::optional<int> m_exitCode;
    std::thread m_reader;
    std::atomic<HANDLE> m_readerThread{nullptr};
    std::mutex m_mutex;
    std::string m_buffer;
    bool m_eof = false;
};

} // namespace

std::unique_ptr<Pty> createPty() {
    return std::make_unique<ConPty>();
}

std::string defaultShell() {
    if (const char* comspec = std::getenv("COMSPEC"); comspec && *comspec) {
        return comspec;
    }
    return "cmd.exe";
}

bool runningInFlatpak() {
    return false;
}

std::vector<std::string> flatpakSpawnCommand(const PtyOptions&) {
    return {};
}

std::string findExecutable(std::string_view program, std::string_view pathVariable) {
    if (program.empty()) {
        return {};
    }
    const std::wstring wide = widen(program);
    if (wide.find_first_of(L"\\/:") != std::wstring::npos) {
        const DWORD attributes = GetFileAttributesW(wide.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY) ? std::string(program)
                                                                                                    : std::string();
    }
    const std::wstring path = widen(pathVariable);
    for (const wchar_t* extension : {static_cast<const wchar_t*>(nullptr), L".exe", L".com", L".bat", L".cmd"}) {
        wchar_t buffer[MAX_PATH * 4];
        const DWORD length = SearchPathW(path.empty() ? nullptr : path.c_str(), wide.c_str(), extension,
                                         static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
        if (length > 0 && length < std::size(buffer)) {
            return narrow(std::wstring_view(buffer, length));
        }
    }
    return {};
}

} // namespace wt::platform
