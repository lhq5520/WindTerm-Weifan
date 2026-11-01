// Serial port tests. On POSIX a pseudo-terminal stands in for the device: the slave side is opened
// as the "serial port" (termios works the same), the master side plays the remote end.

#include "WtTest.h"

#include "wt/platform/SerialPort.h"

#include <chrono>
#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace wt::platform;
using namespace std::chrono_literals;

WT_TEST(serial_config_description_and_validation) {
    SerialConfig config;
    config.device = "/dev/ttyUSB0";
    WT_CHECK_EQ(describeSerialConfig(config), std::string("115200 8N1"));
    config.baudRate = 9600;
    config.dataBits = 7;
    config.parity = Parity::Even;
    config.stopBits = StopBits::Two;
    config.flowControl = FlowControl::RtsCts;
    WT_CHECK_EQ(describeSerialConfig(config), std::string("9600 7E2 RTS/CTS"));
    WT_CHECK(validateSerialConfig(config).empty());
    config.dataBits = 9;
    WT_CHECK(!validateSerialConfig(config).empty());
    config.dataBits = 8;
    config.stopBits = StopBits::OneAndHalf;
    WT_CHECK(!validateSerialConfig(config).empty()); // 1.5 stop bits only with 5 data bits
    config.stopBits = StopBits::One;
    config.baudRate = 10;
    WT_CHECK(!validateSerialConfig(config).empty());
    config.baudRate = 115200;
    config.device.clear();
    WT_CHECK(!validateSerialConfig(config).empty());
    WT_CHECK(!standardBaudRates().empty());
}

WT_TEST(serial_listing_ports_does_not_fail) {
    const auto ports = availableSerialPorts(); // usually empty on build machines
    for (const auto& port : ports) {
        WT_CHECK(!port.device.empty());
    }
}

WT_TEST(serial_open_missing_device_fails_cleanly) {
    SerialConfig config;
#ifdef _WIN32
    config.device = "COM250";
#else
    config.device = "/dev/does-not-exist-wt";
#endif
    wt::core::Status status;
    WT_CHECK(SerialPort::open(config, &status) == nullptr);
    WT_CHECK(!status.isOk());
    WT_CHECK(status.message().find("does-not-exist") != std::string::npos || status.message().find("COM250") != std::string::npos);
}

#ifndef _WIN32

namespace {

struct PtyPair {
    int master = -1;
    std::string slave;
    PtyPair() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0) {
            if (const char* name = ::ptsname(master)) {
                slave = name;
            }
        }
    }
    ~PtyPair() { closeMaster(); }
    void closeMaster() {
        if (master >= 0) {
            ::close(master);
            master = -1;
        }
    }
    std::string readMaster(std::size_t want, std::chrono::milliseconds timeout = 3000ms) const {
        std::string out;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
            pollfd pfd{master, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0) {
                continue;
            }
            char buffer[256];
            const ssize_t n = ::read(master, buffer, sizeof(buffer));
            if (n <= 0) {
                break;
            }
            out.append(buffer, static_cast<std::size_t>(n));
        }
        return out;
    }
};

std::string readPort(SerialPort& port, std::size_t want, std::chrono::milliseconds timeout = 3000ms) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[256];
    while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
        if (!port.waitForReadable(50ms)) {
            continue;
        }
        const IoResult r = port.read(buffer, sizeof(buffer));
        if (r.state == IoState::Ok) {
            out.append(buffer, r.bytes);
        } else if (r.state != IoState::WouldBlock) {
            break;
        }
    }
    return out;
}

} // namespace

WT_TEST(serial_raw_io_over_a_pty) {
    PtyPair pty;
    WT_REQUIRE(!pty.slave.empty());
    SerialConfig config;
    config.device = pty.slave;
    wt::core::Status status;
    auto port = SerialPort::open(config, &status);
    WT_REQUIRE(port != nullptr);
    WT_CHECK(port->config() == config);

    termios tio{};
    WT_REQUIRE(::tcgetattr(static_cast<int>(port->nativeHandle()), &tio) == 0);
    WT_CHECK_EQ(::cfgetospeed(&tio), static_cast<speed_t>(B115200));
    WT_CHECK_EQ(tio.c_cflag & CSIZE, static_cast<tcflag_t>(CS8));
    WT_CHECK((tio.c_cflag & PARENB) == 0);
    WT_CHECK((tio.c_cflag & CSTOPB) == 0);
    WT_CHECK((tio.c_lflag & (ICANON | ECHO)) == 0); // raw: no line editing, no echo
    WT_CHECK((tio.c_oflag & OPOST) == 0);           // raw: "\n" is not turned into "\r\n"

    // Nothing to read yet: non-blocking.
    char byte = 0;
    WT_CHECK(port->read(&byte, 1).state == IoState::WouldBlock);

    // Device -> us, including bytes a line discipline would interpret.
    const std::string incoming = std::string("hello\n\x03\x7f") + "\xff";
    WT_REQUIRE(::write(pty.master, incoming.data(), incoming.size()) == static_cast<ssize_t>(incoming.size()));
    WT_CHECK_EQ(readPort(*port, incoming.size()), incoming);
    // Us -> device.
    const IoResult written = port->write("ping\n");
    WT_CHECK(written.state == IoState::Ok && written.bytes == 5);
    WT_CHECK_EQ(pty.readMaster(5), std::string("ping\n"));
}

WT_TEST(serial_reconfigure_line_settings) {
    PtyPair pty;
    WT_REQUIRE(!pty.slave.empty());
    SerialConfig config;
    config.device = pty.slave;
    config.baudRate = 9600;
    auto port = SerialPort::open(config, nullptr);
    WT_REQUIRE(port != nullptr);
    SerialConfig changed = config;
    changed.baudRate = 38400;
    changed.dataBits = 7;
    changed.parity = Parity::Odd;
    changed.stopBits = StopBits::Two;
    changed.flowControl = FlowControl::XonXoff;
    changed.device = "ignored";
    WT_REQUIRE(port->reconfigure(changed).isOk());
    WT_CHECK_EQ(port->config().device, pty.slave);
    termios tio{};
    WT_REQUIRE(::tcgetattr(static_cast<int>(port->nativeHandle()), &tio) == 0);
    WT_CHECK_EQ(::cfgetospeed(&tio), static_cast<speed_t>(B38400));
#ifndef __linux__
    // Linux pseudo-terminals force CS8 without parity (pty_set_termios); real UARTs keep them.
    WT_CHECK_EQ(tio.c_cflag & CSIZE, static_cast<tcflag_t>(CS7));
    WT_CHECK((tio.c_cflag & PARENB) != 0);
#endif
    WT_CHECK((tio.c_cflag & PARODD) != 0);
    WT_CHECK((tio.c_iflag & INPCK) != 0);
    WT_CHECK((tio.c_cflag & CSTOPB) != 0);
    WT_CHECK((tio.c_iflag & IXON) != 0);
    // Invalid settings are rejected and leave the port as it was.
    changed.dataBits = 4;
    WT_CHECK(!port->reconfigure(changed).isOk());
    WT_CHECK_EQ(port->config().dataBits, 7);
}

WT_TEST(serial_custom_baud_rate) {
    PtyPair pty;
    WT_REQUIRE(!pty.slave.empty());
    SerialConfig config;
    config.device = pty.slave;
    config.baudRate = 250000; // 3D printers; not a POSIX constant
    wt::core::Status status;
    auto port = SerialPort::open(config, &status);
#if defined(__linux__) || defined(__APPLE__)
    WT_CHECK(port != nullptr);
    if (!port) {
        std::fprintf(stderr, "    custom baud rate: %s\n", status.message().c_str());
    }
#else
    WT_CHECK(port == nullptr);
#endif
}

WT_TEST(serial_port_is_exclusive) {
    PtyPair pty;
    WT_REQUIRE(!pty.slave.empty());
    SerialConfig config;
    config.device = pty.slave;
    auto first = SerialPort::open(config, nullptr);
    WT_REQUIRE(first != nullptr);
    wt::core::Status status;
    auto second = SerialPort::open(config, &status);
    WT_CHECK(second == nullptr);
    WT_CHECK(!status.isOk());
    first.reset();
    auto third = SerialPort::open(config, nullptr); // free again
    WT_CHECK(third != nullptr);
}

WT_TEST(serial_hangup_is_reported_as_closed) {
    PtyPair pty;
    WT_REQUIRE(!pty.slave.empty());
    SerialConfig config;
    config.device = pty.slave;
    auto port = SerialPort::open(config, nullptr);
    WT_REQUIRE(port != nullptr);
    pty.closeMaster(); // like unplugging a USB adapter
    IoState state = IoState::WouldBlock;
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    char buffer[16];
    while (state == IoState::WouldBlock && std::chrono::steady_clock::now() < deadline) {
        port->waitForReadable(50ms);
        state = port->read(buffer, sizeof(buffer)).state;
    }
    WT_CHECK(state == IoState::Closed);
}

WT_TEST(serial_rejects_non_terminals) {
    SerialConfig config;
    config.device = "/dev/null";
    wt::core::Status status;
    WT_CHECK(SerialPort::open(config, &status) == nullptr);
    WT_CHECK(status.message().find("not a serial device") != std::string::npos);
}

#endif
