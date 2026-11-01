// Serial (COM) ports on Windows. A helper thread performs overlapped reads into a buffer and
// signals a manual-reset event while data is buffered or the device has gone, so the port fits the
// same event-loop integration as ConPty (QWinEventNotifier).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "wt/platform/SerialPort.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>

namespace wt::platform {
namespace {

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

core::Status lastError(const std::string& what) {
    const DWORD code = GetLastError();
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
    return core::Status::error(what + ": " + text, static_cast<int>(code));
}

core::Status applyConfig(HANDLE handle, const SerialConfig& config) {
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {
        return lastError("GetCommState");
    }
    dcb.BaudRate = static_cast<DWORD>(config.baudRate);
    dcb.fBinary = TRUE;
    dcb.ByteSize = static_cast<BYTE>(config.dataBits);
    dcb.fParity = config.parity != Parity::None;
    switch (config.parity) {
    case Parity::None: dcb.Parity = NOPARITY; break;
    case Parity::Odd: dcb.Parity = ODDPARITY; break;
    case Parity::Even: dcb.Parity = EVENPARITY; break;
    case Parity::Mark: dcb.Parity = MARKPARITY; break;
    case Parity::Space: dcb.Parity = SPACEPARITY; break;
    }
    switch (config.stopBits) {
    case StopBits::One: dcb.StopBits = ONESTOPBIT; break;
    case StopBits::OneAndHalf: dcb.StopBits = ONE5STOPBITS; break;
    case StopBits::Two: dcb.StopBits = TWOSTOPBITS; break;
    }
    dcb.fOutxCtsFlow = config.flowControl == FlowControl::RtsCts;
    dcb.fRtsControl = config.flowControl == FlowControl::RtsCts ? RTS_CONTROL_HANDSHAKE : RTS_CONTROL_ENABLE;
    dcb.fOutxDsrFlow = config.flowControl == FlowControl::DsrDtr;
    dcb.fDtrControl = config.flowControl == FlowControl::DsrDtr ? DTR_CONTROL_HANDSHAKE : DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = config.flowControl == FlowControl::XonXoff;
    dcb.fInX = config.flowControl == FlowControl::XonXoff;
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    dcb.XonLim = 2048;
    dcb.XoffLim = 512;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(handle, &dcb)) {
        return lastError("cannot configure " + config.device);
    }
    // Reads return what is there as soon as one byte arrived, or after 100 ms without data.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutConstant = 2000;
    if (!SetCommTimeouts(handle, &timeouts)) {
        return lastError("SetCommTimeouts");
    }
    return core::Status::ok();
}

class WinSerialPort final : public SerialPort {
public:
    WinSerialPort(HANDLE handle, SerialConfig config) : m_handle(handle), m_config(std::move(config)) {
        m_readable = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_reader = std::thread([this] { readLoop(); });
    }
    ~WinSerialPort() override {
        close();
        if (m_readable) {
            CloseHandle(m_readable);
        }
        if (m_stop) {
            CloseHandle(m_stop);
        }
    }

    IoResult read(char* buffer, std::size_t size) override {
        std::lock_guard lock(m_mutex);
        if (!m_buffer.empty()) {
            const std::size_t count = std::min(size, m_buffer.size());
            std::copy_n(m_buffer.begin(), count, buffer);
            m_buffer.erase(0, count);
            if (m_buffer.empty() && !m_closed) {
                ResetEvent(m_readable);
            }
            return {IoState::Ok, count, 0};
        }
        if (m_closed) {
            return {IoState::Closed, 0, static_cast<int>(m_error)};
        }
        ResetEvent(m_readable);
        return {IoState::WouldBlock, 0, 0};
    }

    IoResult write(std::string_view data) override {
        if (m_handle == INVALID_HANDLE_VALUE) {
            return {IoState::Closed, 0, 0};
        }
        if (data.empty()) {
            return {IoState::Ok, 0, 0};
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(data.size(), 64 * 1024));
        BOOL ok = WriteFile(m_handle, data.data(), size, &written, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(m_handle, &overlapped, &written, TRUE); // bounded by the write timeout
        }
        const DWORD error = ok ? 0 : GetLastError();
        CloseHandle(overlapped.hEvent);
        if (!ok) {
            return {error == ERROR_OPERATION_ABORTED || error == ERROR_BAD_COMMAND ? IoState::Closed : IoState::Error,
                    0, static_cast<int>(error)};
        }
        return {written == 0 ? IoState::WouldBlock : IoState::Ok, written, 0};
    }

    bool waitForReadable(std::chrono::milliseconds timeout) override {
        return WaitForSingleObject(m_readable, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
    }

    core::Status reconfigure(const SerialConfig& config) override {
        SerialConfig updated = config;
        updated.device = m_config.device;
        if (const auto problems = validateSerialConfig(updated); !problems.empty()) {
            return core::Status::error(problems.front());
        }
        if (auto status = applyConfig(m_handle, updated); !status) {
            return status;
        }
        m_config = std::move(updated);
        return core::Status::ok();
    }

    core::Status setBreak(bool on) override {
        if (!(on ? SetCommBreak(m_handle) : ClearCommBreak(m_handle))) {
            return lastError("cannot set the break condition");
        }
        return core::Status::ok();
    }
    core::Status setDtr(bool on) override {
        return EscapeCommFunction(m_handle, on ? SETDTR : CLRDTR) ? core::Status::ok() : lastError("cannot change DTR");
    }
    core::Status setRts(bool on) override {
        return EscapeCommFunction(m_handle, on ? SETRTS : CLRRTS) ? core::Status::ok() : lastError("cannot change RTS");
    }

    std::intptr_t nativeHandle() const override { return reinterpret_cast<std::intptr_t>(m_readable); }
    const SerialConfig& config() const override { return m_config; }

    void close() override {
        if (m_handle == INVALID_HANDLE_VALUE) {
            return;
        }
        SetEvent(m_stop);
        CancelIoEx(m_handle, nullptr);
        if (m_reader.joinable()) {
            m_reader.join();
        }
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        std::lock_guard lock(m_mutex);
        m_closed = true;
        SetEvent(m_readable);
    }

private:
    void readLoop() {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        char chunk[4096];
        DWORD error = 0;
        for (;;) {
            ResetEvent(overlapped.hEvent);
            DWORD count = 0;
            BOOL ok = ReadFile(m_handle, chunk, sizeof(chunk), &count, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                HANDLE events[2] = {overlapped.hEvent, m_stop};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0) {
                    CancelIoEx(m_handle, &overlapped);
                    GetOverlappedResult(m_handle, &overlapped, &count, TRUE);
                    break; // close() asked us to stop
                }
                ok = GetOverlappedResult(m_handle, &overlapped, &count, FALSE);
            }
            if (!ok) {
                error = GetLastError(); // e.g. the USB adapter was unplugged
                break;
            }
            if (count > 0) {
                std::lock_guard lock(m_mutex);
                m_buffer.append(chunk, count);
                SetEvent(m_readable);
            }
            if (WaitForSingleObject(m_stop, 0) == WAIT_OBJECT_0) {
                break;
            }
        }
        CloseHandle(overlapped.hEvent);
        std::lock_guard lock(m_mutex);
        m_closed = true;
        m_error = error;
        SetEvent(m_readable);
    }

    HANDLE m_handle = INVALID_HANDLE_VALUE;
    HANDLE m_readable = nullptr;
    HANDLE m_stop = nullptr;
    SerialConfig m_config;
    std::thread m_reader;
    std::mutex m_mutex;
    std::string m_buffer;
    bool m_closed = false;
    DWORD m_error = 0;
};

int comNumber(const std::string& name) {
    std::size_t i = 0;
    while (i < name.size() && !std::iswdigit(static_cast<wint_t>(name[i]))) {
        ++i;
    }
    return i < name.size() ? std::atoi(name.c_str() + i) : 0;
}

} // namespace

std::unique_ptr<SerialPort> SerialPort::open(const SerialConfig& config, core::Status* status) {
    auto fail = [status](core::Status error) -> std::unique_ptr<SerialPort> {
        if (status) {
            *status = std::move(error);
        }
        return nullptr;
    };
    if (const auto problems = validateSerialConfig(config); !problems.empty()) {
        return fail(core::Status::error(problems.front()));
    }
    // "\\.\COM10": ports above COM9 are only reachable through the device namespace.
    std::wstring path = widen(config.device);
    if (path.rfind(L"\\\\.\\", 0) != 0) {
        path = L"\\\\.\\" + path;
    }
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0 /* exclusive */, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) {
            return fail(core::Status::error(config.device + " is in use by another program"));
        }
        return fail(lastError("cannot open " + config.device));
    }
    if (auto applied = applyConfig(handle, config); !applied) {
        CloseHandle(handle);
        return fail(std::move(applied));
    }
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return std::make_unique<WinSerialPort>(handle, config);
}

std::vector<SerialPortInfo> availableSerialPorts() {
    std::vector<SerialPortInfo> ports;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return ports;
    }
    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD nameLength = 256;
        wchar_t value[256];
        DWORD valueSize = sizeof(value);
        DWORD type = 0;
        const LONG rc = RegEnumValueW(key, index, name, &nameLength, nullptr, &type, reinterpret_cast<BYTE*>(value),
                                      &valueSize);
        if (rc == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (rc != ERROR_SUCCESS || type != REG_SZ) {
            continue;
        }
        const std::size_t chars = std::min<std::size_t>(valueSize / sizeof(wchar_t), 255);
        std::wstring device(value, chars);
        while (!device.empty() && device.back() == L'\0') {
            device.pop_back();
        }
        // The value name is the kernel device (\Device\Serial0, \Device\VCP0, ...).
        std::wstring kernelName(name, nameLength);
        const auto slash = kernelName.find_last_of(L'\\');
        ports.push_back({narrow(device), narrow(slash == std::wstring::npos ? kernelName : kernelName.substr(slash + 1))});
    }
    RegCloseKey(key);
    std::sort(ports.begin(), ports.end(),
              [](const auto& a, const auto& b) { return comNumber(a.device) < comNumber(b.device); });
    return ports;
}

} // namespace wt::platform
