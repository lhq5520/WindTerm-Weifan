// Serial ports on POSIX systems (Linux, macOS, *BSD) through termios.

#include "wt/platform/Files.h"
#include "wt/platform/SerialPort.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <IOKit/serial/ioss.h> // IOSSIOSPEED: arbitrary baud rates
#endif

namespace wt::platform {

#if defined(__linux__)
// LinuxCustomBaud.cpp: termios2/BOTHER (its header clashes with <termios.h>). Returns 0 or errno.
int setLinuxCustomBaudRate(int fd, int rate);
#endif

namespace {

using core::Status;

Status errnoStatus(const std::string& what, int err) {
    return Status::error(what + ": " + std::strerror(err), err);
}

speed_t standardSpeed(int rate) {
    switch (rate) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
#ifdef B7200
    case 7200: return B7200;
#endif
#ifdef B14400
    case 14400: return B14400;
#endif
#ifdef B28800
    case 28800: return B28800;
#endif
#ifdef B57600
    case 57600: return B57600;
#endif
#ifdef B76800
    case 76800: return B76800;
#endif
#ifdef B115200
    case 115200: return B115200;
#endif
#ifdef B230400
    case 230400: return B230400;
#endif
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B500000
    case 500000: return B500000;
#endif
#ifdef B576000
    case 576000: return B576000;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B1152000
    case 1152000: return B1152000;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
#ifdef B2500000
    case 2500000: return B2500000;
#endif
#ifdef B3000000
    case 3000000: return B3000000;
#endif
#ifdef B3500000
    case 3500000: return B3500000;
#endif
#ifdef B4000000
    case 4000000: return B4000000;
#endif
    default: return 0; // not a standard rate (B0 would mean "hang up", never used here)
    }
}

Status applyConfig(int fd, const SerialConfig& config) {
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0) {
        return errnoStatus("tcgetattr", errno);
    }
    ::cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE);
    switch (config.dataBits) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }
    tio.c_cflag &= ~static_cast<tcflag_t>(PARENB | PARODD);
#ifdef CMSPAR
    tio.c_cflag &= ~static_cast<tcflag_t>(CMSPAR);
#endif
    tio.c_iflag &= ~static_cast<tcflag_t>(INPCK | ISTRIP | IGNPAR | PARMRK);
    switch (config.parity) {
    case Parity::None: break;
    case Parity::Odd:
        tio.c_cflag |= PARENB | PARODD;
        tio.c_iflag |= INPCK;
        break;
    case Parity::Even:
        tio.c_cflag |= PARENB;
        tio.c_iflag |= INPCK;
        break;
    case Parity::Mark:
    case Parity::Space:
#ifdef CMSPAR
        tio.c_cflag |= PARENB | CMSPAR | (config.parity == Parity::Mark ? PARODD : 0);
        break;
#else
        return Status::error("mark/space parity is not supported on this system");
#endif
    }
    // CSTOPB selects 2 stop bits, which UARTs turn into 1.5 with 5 data bits.
    if (config.stopBits == StopBits::One) {
        tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
    } else {
        tio.c_cflag |= CSTOPB;
    }
    tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
#ifdef CRTSCTS
    tio.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
#endif
#if defined(CDTR_IFLOW) && defined(CDSR_OFLOW)
    tio.c_cflag &= ~static_cast<tcflag_t>(CDTR_IFLOW | CDSR_OFLOW);
#endif
    switch (config.flowControl) {
    case FlowControl::None: break;
    case FlowControl::XonXoff:
        tio.c_iflag |= IXON | IXOFF;
        tio.c_cc[VSTART] = 0x11;
        tio.c_cc[VSTOP] = 0x13;
        break;
    case FlowControl::RtsCts:
#ifdef CRTSCTS
        tio.c_cflag |= CRTSCTS;
        break;
#else
        return Status::error("RTS/CTS flow control is not supported on this system");
#endif
    case FlowControl::DsrDtr:
#if defined(CDTR_IFLOW) && defined(CDSR_OFLOW)
        tio.c_cflag |= CDTR_IFLOW | CDSR_OFLOW;
        break;
#else
        return Status::error("DSR/DTR flow control is not supported on this system");
#endif
    }
    // Reads return as soon as one byte is there (the descriptor is non-blocking anyway).
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    const speed_t speed = standardSpeed(config.baudRate);
    const bool custom = speed == 0;
    if (!custom) {
        ::cfsetispeed(&tio, speed);
        ::cfsetospeed(&tio, speed);
    } else {
#if defined(__APPLE__) || defined(__linux__)
        ::cfsetispeed(&tio, B9600); // replaced by the custom rate below
        ::cfsetospeed(&tio, B9600);
#else
        return Status::error("unsupported baud rate " + std::to_string(config.baudRate));
#endif
    }
    if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
        return errnoStatus("cannot configure " + config.device, errno);
    }
    if (custom) {
#if defined(__APPLE__)
        speed_t rate = static_cast<speed_t>(config.baudRate);
        if (::ioctl(fd, IOSSIOSPEED, &rate) != 0) {
            return errnoStatus("unsupported baud rate " + std::to_string(config.baudRate), errno);
        }
#elif defined(__linux__)
        if (const int err = setLinuxCustomBaudRate(fd, config.baudRate); err != 0) {
            return errnoStatus("unsupported baud rate " + std::to_string(config.baudRate), err);
        }
#endif
    }
    return Status::ok();
}

class PosixSerialPort final : public SerialPort {
public:
    PosixSerialPort(int fd, SerialConfig config) : m_fd(fd), m_config(std::move(config)) {}
    ~PosixSerialPort() override { close(); }

    IoResult read(char* buffer, std::size_t size) override {
        if (m_fd < 0) {
            return {IoState::Closed, 0, 0};
        }
        for (;;) {
            const ssize_t n = ::read(m_fd, buffer, size);
            if (n > 0) {
                return {IoState::Ok, static_cast<std::size_t>(n), 0};
            }
            if (n == 0) {
                return {IoState::Closed, 0, 0}; // hang-up, e.g. a USB adapter was unplugged
            }
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                return {IoState::WouldBlock, 0, 0};
            }
            if (err == EIO || err == ENXIO || err == ENODEV) {
                return {IoState::Closed, 0, err};
            }
            return {IoState::Error, 0, err};
        }
    }

    IoResult write(std::string_view data) override {
        if (m_fd < 0) {
            return {IoState::Closed, 0, 0};
        }
        if (data.empty()) {
            return {IoState::Ok, 0, 0};
        }
        for (;;) {
            const ssize_t n = ::write(m_fd, data.data(), data.size());
            if (n >= 0) {
                return {IoState::Ok, static_cast<std::size_t>(n), 0};
            }
            const int err = errno;
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN || err == EWOULDBLOCK) {
                return {IoState::WouldBlock, 0, 0};
            }
            if (err == EIO || err == ENXIO || err == ENODEV) {
                return {IoState::Closed, 0, err};
            }
            return {IoState::Error, 0, err};
        }
    }

    bool waitForReadable(std::chrono::milliseconds timeout) override {
        if (m_fd < 0) {
            return false;
        }
        pollfd pfd{m_fd, POLLIN, 0};
        int rc;
        do {
            rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        } while (rc < 0 && errno == EINTR);
        return rc > 0;
    }

    Status reconfigure(const SerialConfig& config) override {
        SerialConfig updated = config;
        updated.device = m_config.device;
        if (const auto problems = validateSerialConfig(updated); !problems.empty()) {
            return Status::error(problems.front());
        }
        if (auto status = applyConfig(m_fd, updated); !status) {
            return status;
        }
        m_config = std::move(updated);
        return Status::ok();
    }

    Status setBreak(bool on) override {
#if defined(TIOCSBRK) && defined(TIOCCBRK)
        if (::ioctl(m_fd, on ? TIOCSBRK : TIOCCBRK) != 0) {
            return errnoStatus("cannot set the break condition", errno);
        }
#else
        if (on && ::tcsendbreak(m_fd, 0) != 0) {
            return errnoStatus("cannot send a break", errno);
        }
#endif
        return Status::ok();
    }

    Status setDtr(bool on) override { return setModemBit(TIOCM_DTR, on, "DTR"); }
    Status setRts(bool on) override { return setModemBit(TIOCM_RTS, on, "RTS"); }

    std::intptr_t nativeHandle() const override { return m_fd; }
    const SerialConfig& config() const override { return m_config; }

    void close() override {
        if (m_fd >= 0) {
#ifdef TIOCNXCL
            // The exclusive flag lives on the tty, which may outlive this descriptor (ptys).
            ::ioctl(m_fd, TIOCNXCL);
#endif
            ::close(m_fd); // also releases the flock()
            m_fd = -1;
        }
    }

private:
    Status setModemBit(int bit, bool on, const char* name) {
        int bits = bit;
        if (::ioctl(m_fd, on ? TIOCMBIS : TIOCMBIC, &bits) != 0) {
            return errnoStatus(std::string("cannot change ") + name, errno);
        }
        return Status::ok();
    }

    int m_fd = -1;
    SerialConfig m_config;
};

std::string trimmed(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    return text.substr(start);
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
        return fail(Status::error(problems.front()));
    }
    // O_NONBLOCK also keeps open() from waiting for carrier detect on dial-in devices.
    const int fd = ::open(config.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        std::string message = "cannot open " + config.device + ": " + std::strerror(err);
#if defined(__linux__)
        if (err == EACCES) {
            message += " (is your user in the \"dialout\" group?)";
        }
#endif
        return fail(Status::error(message, err));
    }
    if (!::isatty(fd)) {
        ::close(fd);
        return fail(Status::error(config.device + " is not a serial device"));
    }
    // Refuse a port another program (minicom, screen, a second tab) is using.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        return fail(Status::error(config.device + " is in use by another program", err));
    }
#ifdef TIOCEXCL
    ::ioctl(fd, TIOCEXCL);
#endif
    if (auto applied = applyConfig(fd, config); !applied) {
        ::close(fd);
        return fail(std::move(applied));
    }
    ::tcflush(fd, TCIOFLUSH);
    return std::make_unique<PosixSerialPort>(fd, config);
}

std::vector<SerialPortInfo> availableSerialPorts() {
    namespace fs = std::filesystem;
    std::vector<SerialPortInfo> ports;
    std::error_code ec;
#if defined(__linux__)
    for (const auto& entry : fs::directory_iterator("/sys/class/tty", ec)) {
        const std::string name = entry.path().filename().string();
        const fs::path deviceLink = entry.path() / "device";
        if (!fs::exists(deviceLink, ec)) {
            continue; // virtual consoles and pseudo-terminals have no device
        }
        if (name.rfind("ttyS", 0) == 0) {
            // The 8250 driver registers ports without hardware; their UART type is 0 (unknown).
            const auto type = readFile((entry.path() / "type").string());
            if (!type || std::atoi(type->c_str()) == 0) {
                continue;
            }
        }
        const std::string device = "/dev/" + name;
        if (!fs::exists(device, ec)) {
            continue;
        }
        std::string description;
        fs::path node = fs::canonical(deviceLink, ec);
        for (int level = 0; level < 4 && !ec && !node.empty(); ++level, node = node.parent_path()) {
            if (const auto product = readFile((node / "product").string())) {
                const auto manufacturer = readFile((node / "manufacturer").string());
                description = trimmed((manufacturer ? trimmed(*manufacturer) + " " : std::string()) + *product);
                break;
            }
        }
        ports.push_back({device, description});
    }
#else
    for (const auto& entry : fs::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
#if defined(__APPLE__)
        // Call-out devices: opening them does not wait for carrier detect.
        if (name.rfind("cu.", 0) == 0) {
            ports.push_back({"/dev/" + name, name.substr(3)});
        }
#else
        for (const char* prefix : {"cuaU", "cuau", "ttyU", "ttyS", "ttyUSB", "ttyACM"}) {
            if (name.rfind(prefix, 0) == 0 && name.find('.') == std::string::npos) {
                ports.push_back({"/dev/" + name, {}});
                break;
            }
        }
#endif
    }
#endif
    std::sort(ports.begin(), ports.end(), [](const auto& a, const auto& b) { return a.device < b.device; });
    return ports;
}

} // namespace wt::platform
