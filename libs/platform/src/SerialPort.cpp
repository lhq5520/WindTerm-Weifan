// Platform-independent parts of the serial port API.

#include "wt/platform/SerialPort.h"

namespace wt::platform {

std::string describeSerialConfig(const SerialConfig& config) {
    char parity = 'N';
    switch (config.parity) {
    case Parity::None: parity = 'N'; break;
    case Parity::Odd: parity = 'O'; break;
    case Parity::Even: parity = 'E'; break;
    case Parity::Mark: parity = 'M'; break;
    case Parity::Space: parity = 'S'; break;
    }
    const char* stop = config.stopBits == StopBits::One ? "1" : config.stopBits == StopBits::Two ? "2" : "1.5";
    std::string text = std::to_string(config.baudRate) + " " + std::to_string(config.dataBits) + parity + stop;
    switch (config.flowControl) {
    case FlowControl::None: break;
    case FlowControl::XonXoff: text += " XON/XOFF"; break;
    case FlowControl::RtsCts: text += " RTS/CTS"; break;
    case FlowControl::DsrDtr: text += " DSR/DTR"; break;
    }
    return text;
}

std::vector<std::string> validateSerialConfig(const SerialConfig& config) {
    std::vector<std::string> problems;
    if (config.device.empty()) {
        problems.emplace_back("no serial device given");
    }
    if (config.baudRate < 50 || config.baudRate > 16000000) {
        problems.emplace_back("baud rate must be between 50 and 16000000");
    }
    if (config.dataBits < 5 || config.dataBits > 8) {
        problems.emplace_back("data bits must be 5, 6, 7 or 8");
    }
    if (config.stopBits == StopBits::OneAndHalf && config.dataBits != 5) {
        problems.emplace_back("1.5 stop bits need 5 data bits");
    }
    return problems;
}

const std::vector<int>& standardBaudRates() {
    static const std::vector<int> rates = {300,    1200,   2400,   4800,   9600,    14400,   19200,   38400,
                                           57600,  115200, 230400, 460800, 500000,  921600,  1000000, 1500000,
                                           2000000, 3000000};
    return rates;
}

} // namespace wt::platform
