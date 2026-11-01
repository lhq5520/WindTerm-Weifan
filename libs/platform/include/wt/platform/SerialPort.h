#pragma once

#include "wt/core/Status.h"
#include "wt/platform/Pty.h" // IoResult / IoState

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wt::platform {

enum class Parity { None, Odd, Even, Mark, Space };
enum class StopBits { One, OneAndHalf, Two };
enum class FlowControl { None, XonXoff, RtsCts, DsrDtr };

struct SerialConfig {
    std::string device; // "/dev/ttyUSB0", "/dev/cu.usbserial-1410", "COM3"
    int baudRate = 115200;
    int dataBits = 8; // 5..8
    Parity parity = Parity::None;
    StopBits stopBits = StopBits::One;
    FlowControl flowControl = FlowControl::None;

    friend bool operator==(const SerialConfig&, const SerialConfig&) = default;
};

// "115200 8N1" (plus the flow control when there is one).
std::string describeSerialConfig(const SerialConfig& config);
// Problems with the settings themselves (not whether the device exists).
std::vector<std::string> validateSerialConfig(const SerialConfig& config);

// Rates offered by the UI; others work where the OS supports custom rates (Linux, macOS, Windows).
const std::vector<int>& standardBaudRates();

struct SerialPortInfo {
    std::string device;      // what SerialConfig::device expects
    std::string description; // "FTDI FT232R USB UART", may be empty

    friend bool operator==(const SerialPortInfo&, const SerialPortInfo&) = default;
};

// Serial ports present right now: USB adapters, on-board UARTs, Bluetooth serial ports. Sorted by
// device name.
std::vector<SerialPortInfo> availableSerialPorts();

// An open serial device in raw mode. Reads and writes never block for long: on POSIX the
// descriptor is non-blocking (poll nativeHandle() for readability); on Windows a helper thread
// reads into a buffer and nativeHandle() is a manual-reset event that is signalled while data is
// buffered or the device has gone, like Pty. Not thread-safe.
class SerialPort {
public:
    virtual ~SerialPort() = default;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // Opens the device exclusively and applies `config`; nullptr with `status` set on failure.
    static std::unique_ptr<SerialPort> open(const SerialConfig& config, core::Status* status);

    virtual IoResult read(char* buffer, std::size_t size) = 0;
    // May transfer fewer bytes than requested (WouldBlock when the output buffer is full).
    virtual IoResult write(std::string_view data) = 0;
    virtual bool waitForReadable(std::chrono::milliseconds timeout) = 0;
    // Applies new line settings to the open device (the device name is ignored).
    virtual core::Status reconfigure(const SerialConfig& config) = 0;
    // Starts/ends a break condition on the transmit line (the caller times it, typically 250 ms).
    virtual core::Status setBreak(bool on) = 0;
    virtual core::Status setDtr(bool on) = 0;
    virtual core::Status setRts(bool on) = 0;
    virtual std::intptr_t nativeHandle() const = 0;
    virtual const SerialConfig& config() const = 0;
    virtual void close() = 0;

protected:
    SerialPort() = default;
};

} // namespace wt::platform
