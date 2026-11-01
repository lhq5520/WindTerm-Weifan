#include "wt/net/Telnet.h"

#include <algorithm>

namespace wt::net {
namespace {

using namespace telnet;

void appendCommand(std::string& out, std::uint8_t verb, std::uint8_t option) {
    out += static_cast<char>(kIac);
    out += static_cast<char>(verb);
    out += static_cast<char>(option);
}

// IAC SB <option> <payload with IAC doubled> IAC SE
std::string subnegotiation(std::uint8_t option, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 6);
    out += static_cast<char>(kIac);
    out += static_cast<char>(kSb);
    out += static_cast<char>(option);
    for (const char c : payload) {
        out += c;
        if (static_cast<std::uint8_t>(c) == kIac) {
            out += c;
        }
    }
    out += static_cast<char>(kIac);
    out += static_cast<char>(kSe);
    return out;
}

// NEW-ENVIRON escapes its own type bytes inside names and values.
void appendEnvText(std::string& out, std::string_view text) {
    for (const char c : text) {
        const auto byte = static_cast<std::uint8_t>(c);
        if (byte == kEnvVar || byte == kEnvValue || byte == kEnvEsc || byte == kEnvUserVar) {
            out += static_cast<char>(kEnvEsc);
        }
        out += c;
    }
}

bool isWellKnownVariable(std::string_view name) {
    for (const char* known : {"USER", "JOB", "ACCT", "PRINTER", "SYSTEMTYPE", "DISPLAY"}) {
        if (name == known) {
            return true;
        }
    }
    return false;
}

} // namespace

TelnetProtocol::TelnetProtocol(TelnetConfig config) : m_config(std::move(config)) {}

bool TelnetProtocol::localEnabled(std::uint8_t option) const {
    return m_options[option].us == Q::Yes;
}

bool TelnetProtocol::remoteEnabled(std::uint8_t option) const {
    return m_options[option].him == Q::Yes;
}

bool TelnetProtocol::acceptLocal(std::uint8_t option) const {
    switch (option) {
    case kOptBinary:
    case kOptSga:
    case kOptTerminalType:
    case kOptNaws:
    case kOptTerminalSpeed:
    case kOptNewEnviron: return true;
    default: return false;
    }
}

bool TelnetProtocol::acceptRemote(std::uint8_t option) const {
    return option == kOptBinary || option == kOptEcho || option == kOptSga;
}

std::string TelnetProtocol::initialNegotiation() {
    std::string out;
    requestLocal(kOptNaws, true, out);
    requestLocal(kOptTerminalType, true, out);
    requestLocal(kOptTerminalSpeed, true, out);
    if (!m_config.environment.empty()) {
        requestLocal(kOptNewEnviron, true, out);
    }
    requestLocal(kOptSga, true, out);
    requestRemote(kOptSga, true, out);
    requestRemote(kOptEcho, true, out);
    if (m_config.requestBinary) {
        requestLocal(kOptBinary, true, out);
        requestRemote(kOptBinary, true, out);
    }
    return out;
}

// RFC 1143 section 7: asking to enable or disable an option ourselves.
void TelnetProtocol::requestLocal(std::uint8_t option, bool enable, std::string& out) {
    Option& o = m_options[option];
    if (enable) {
        switch (o.us) {
        case Q::No:
            o.us = Q::WantYes;
            appendCommand(out, kWill, option);
            break;
        case Q::Yes: break;
        case Q::WantNo: o.usOpposite = true; break;
        case Q::WantYes: o.usOpposite = false; break;
        }
    } else {
        switch (o.us) {
        case Q::No: break;
        case Q::Yes:
            o.us = Q::WantNo;
            appendCommand(out, kWont, option);
            break;
        case Q::WantNo: o.usOpposite = false; break;
        case Q::WantYes: o.usOpposite = true; break;
        }
    }
}

void TelnetProtocol::requestRemote(std::uint8_t option, bool enable, std::string& out) {
    Option& o = m_options[option];
    if (enable) {
        switch (o.him) {
        case Q::No:
            o.him = Q::WantYes;
            appendCommand(out, kDo, option);
            break;
        case Q::Yes: break;
        case Q::WantNo: o.himOpposite = true; break;
        case Q::WantYes: o.himOpposite = false; break;
        }
    } else {
        switch (o.him) {
        case Q::No: break;
        case Q::Yes:
            o.him = Q::WantNo;
            appendCommand(out, kDont, option);
            break;
        case Q::WantNo: o.himOpposite = false; break;
        case Q::WantYes: o.himOpposite = true; break;
        }
    }
}

void TelnetProtocol::onWill(std::uint8_t option, std::string& reply) {
    Option& o = m_options[option];
    switch (o.him) {
    case Q::No:
        if (acceptRemote(option)) {
            o.him = Q::Yes;
            appendCommand(reply, kDo, option);
        } else {
            appendCommand(reply, kDont, option);
        }
        break;
    case Q::Yes: break;
    case Q::WantNo:
        // DONT answered by WILL: a misbehaving peer; take what it says.
        if (o.himOpposite) {
            o.him = Q::Yes;
            o.himOpposite = false;
        } else {
            o.him = Q::No;
        }
        break;
    case Q::WantYes:
        if (o.himOpposite) {
            o.him = Q::WantNo;
            o.himOpposite = false;
            appendCommand(reply, kDont, option);
        } else {
            o.him = Q::Yes;
        }
        break;
    }
}

void TelnetProtocol::onWont(std::uint8_t option, std::string& reply) {
    Option& o = m_options[option];
    switch (o.him) {
    case Q::No: break;
    case Q::Yes:
        o.him = Q::No;
        appendCommand(reply, kDont, option);
        break;
    case Q::WantNo:
        if (o.himOpposite) {
            o.him = Q::WantYes;
            o.himOpposite = false;
            appendCommand(reply, kDo, option);
        } else {
            o.him = Q::No;
        }
        break;
    case Q::WantYes:
        o.him = Q::No;
        o.himOpposite = false;
        break;
    }
}

void TelnetProtocol::onDo(std::uint8_t option, std::string& reply) {
    Option& o = m_options[option];
    switch (o.us) {
    case Q::No:
        if (acceptLocal(option)) {
            o.us = Q::Yes;
            appendCommand(reply, kWill, option);
            onLocalEnabled(option, reply);
        } else {
            appendCommand(reply, kWont, option);
        }
        break;
    case Q::Yes: break;
    case Q::WantNo:
        if (o.usOpposite) {
            o.us = Q::Yes;
            o.usOpposite = false;
            onLocalEnabled(option, reply);
        } else {
            o.us = Q::No;
        }
        break;
    case Q::WantYes:
        if (o.usOpposite) {
            o.us = Q::WantNo;
            o.usOpposite = false;
            appendCommand(reply, kWont, option);
        } else {
            o.us = Q::Yes;
            onLocalEnabled(option, reply);
        }
        break;
    }
}

void TelnetProtocol::onDont(std::uint8_t option, std::string& reply) {
    Option& o = m_options[option];
    switch (o.us) {
    case Q::No: break;
    case Q::Yes:
        o.us = Q::No;
        appendCommand(reply, kWont, option);
        break;
    case Q::WantNo:
        if (o.usOpposite) {
            o.us = Q::WantYes;
            o.usOpposite = false;
            appendCommand(reply, kWill, option);
        } else {
            o.us = Q::No;
        }
        break;
    case Q::WantYes:
        o.us = Q::No;
        o.usOpposite = false;
        break;
    }
}

void TelnetProtocol::onLocalEnabled(std::uint8_t option, std::string& reply) {
    if (option == kOptNaws) {
        reply += nawsReport(); // RFC 1073: report the size as soon as the option is on
    }
}

std::string TelnetProtocol::nawsReport() const {
    const auto width = static_cast<std::uint16_t>(std::clamp(m_config.columns, 0, 0xFFFF));
    const auto height = static_cast<std::uint16_t>(std::clamp(m_config.rows, 0, 0xFFFF));
    const char payload[4] = {static_cast<char>(width >> 8), static_cast<char>(width & 0xFF),
                             static_cast<char>(height >> 8), static_cast<char>(height & 0xFF)};
    return subnegotiation(kOptNaws, std::string_view(payload, 4));
}

std::string TelnetProtocol::resize(int rows, int columns) {
    m_config.rows = rows;
    m_config.columns = columns;
    return localEnabled(kOptNaws) ? nawsReport() : std::string();
}

std::string TelnetProtocol::environmentReport(std::string_view request) const {
    // The request lists (type, name) pairs; an empty list asks for everything.
    struct Wanted {
        std::uint8_t type;
        std::string name;
    };
    std::vector<Wanted> wanted;
    for (std::size_t i = 0; i < request.size();) {
        const auto type = static_cast<std::uint8_t>(request[i++]);
        if (type != kEnvVar && type != kEnvUserVar) {
            continue;
        }
        std::string name;
        while (i < request.size()) {
            auto byte = static_cast<std::uint8_t>(request[i]);
            if (byte == kEnvVar || byte == kEnvUserVar) {
                break;
            }
            if (byte == kEnvEsc && i + 1 < request.size()) {
                byte = static_cast<std::uint8_t>(request[++i]);
            }
            name += static_cast<char>(byte);
            ++i;
        }
        wanted.push_back({type, std::move(name)});
    }
    std::string payload(1, static_cast<char>(kIs));
    auto append = [&payload](std::uint8_t type, const std::string& name, const std::string* value) {
        payload += static_cast<char>(type);
        appendEnvText(payload, name);
        if (value) {
            payload += static_cast<char>(kEnvValue);
            appendEnvText(payload, *value);
        }
    };
    auto find = [this](const std::string& name) -> const std::string* {
        for (const auto& [key, value] : m_config.environment) {
            if (key == name) {
                return &value;
            }
        }
        return nullptr;
    };
    if (wanted.empty()) {
        for (const auto& [name, value] : m_config.environment) {
            append(isWellKnownVariable(name) ? kEnvVar : kEnvUserVar, name, &value);
        }
    } else {
        for (const Wanted& item : wanted) {
            if (item.name.empty()) {
                // "VAR" alone asks for all well-known variables, "USERVAR" alone for the others.
                for (const auto& [name, value] : m_config.environment) {
                    if (isWellKnownVariable(name) == (item.type == kEnvVar)) {
                        append(item.type, name, &value);
                    }
                }
            } else {
                append(item.type, item.name, find(item.name)); // no VALUE: undefined
            }
        }
    }
    return subnegotiation(kOptNewEnviron, payload);
}

void TelnetProtocol::onSubnegotiation(std::string& reply) {
    if (m_sb.empty() || static_cast<std::uint8_t>(m_sb[0]) != kSend) {
        return;
    }
    switch (m_sbOption) {
    case kOptTerminalType:
        if (localEnabled(kOptTerminalType)) {
            const auto& types = m_config.terminalTypes;
            std::string name = "UNKNOWN";
            if (!types.empty()) {
                name = types[std::min(m_terminalTypeIndex, types.size() - 1)];
                // After the last type has been sent twice the list starts again.
                m_terminalTypeIndex = m_terminalTypeIndex >= types.size() ? 0 : m_terminalTypeIndex + 1;
            }
            reply += subnegotiation(kOptTerminalType, std::string(1, static_cast<char>(kIs)) + name);
        }
        break;
    case kOptTerminalSpeed:
        if (localEnabled(kOptTerminalSpeed)) {
            reply += subnegotiation(kOptTerminalSpeed, std::string(1, static_cast<char>(kIs)) + m_config.terminalSpeed);
        }
        break;
    case kOptNewEnviron:
        if (localEnabled(kOptNewEnviron)) {
            reply += environmentReport(std::string_view(m_sb).substr(1));
        }
        break;
    default: break;
    }
}

std::string TelnetProtocol::receive(std::string_view bytes, std::string& reply) {
    constexpr std::size_t kMaxSubnegotiation = 8192;
    std::string data;
    data.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto byte = static_cast<std::uint8_t>(bytes[i]);
        switch (m_state) {
        case State::Cr:
            m_state = State::Data;
            if (byte == 0) {
                break; // CR NUL is a bare carriage return
            }
            [[fallthrough]];
        case State::Data:
            if (byte == kIac) {
                m_state = State::Iac;
            } else {
                data += static_cast<char>(byte);
                if (byte == '\r' && !remoteEnabled(kOptBinary)) {
                    m_state = State::Cr;
                }
            }
            break;
        case State::Iac:
            switch (byte) {
            case kIac:
                data += static_cast<char>(kIac);
                m_state = State::Data;
                break;
            case kWill: m_state = State::Will; break;
            case kWont: m_state = State::Wont; break;
            case kDo: m_state = State::Do; break;
            case kDont: m_state = State::Dont; break;
            case kSb: m_state = State::Sb; break;
            default: m_state = State::Data; break; // NOP, GA, DM, ...: nothing to do for a client
            }
            break;
        case State::Will:
        case State::Wont:
        case State::Do:
        case State::Dont: {
            ++m_negotiations;
            const State verb = m_state;
            m_state = State::Data;
            if (verb == State::Will) {
                onWill(byte, reply);
            } else if (verb == State::Wont) {
                onWont(byte, reply);
            } else if (verb == State::Do) {
                onDo(byte, reply);
            } else {
                onDont(byte, reply);
            }
            break;
        }
        case State::Sb:
            m_sbOption = byte;
            m_sb.clear();
            m_state = State::SbData;
            break;
        case State::SbData:
            if (byte == kIac) {
                m_state = State::SbIac;
            } else if (m_sb.size() < kMaxSubnegotiation) {
                m_sb += static_cast<char>(byte);
            }
            break;
        case State::SbIac:
            if (byte == kIac) {
                if (m_sb.size() < kMaxSubnegotiation) {
                    m_sb += static_cast<char>(kIac);
                }
                m_state = State::SbData;
            } else {
                ++m_negotiations;
                onSubnegotiation(reply);
                m_sb.clear();
                m_state = State::Data;
                if (byte != kSe) {
                    // Unterminated subnegotiation: treat the byte as the command after IAC.
                    m_state = State::Iac;
                    --i;
                }
            }
            break;
        }
    }
    return data;
}

std::string TelnetProtocol::encode(std::string_view input) const {
    const bool binary = localEnabled(kOptBinary);
    std::string out;
    out.reserve(input.size() + 4);
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (static_cast<std::uint8_t>(c) == kIac) {
            out += c;
            out += c;
        } else if (c == '\r' && !binary) {
            out += '\r';
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                out += '\n';
                ++i;
            } else {
                out += m_config.crlf ? '\n' : '\0';
            }
        } else {
            out += c;
        }
    }
    return out;
}

std::string TelnetProtocol::command(TelnetCommand command) {
    std::uint8_t code = kNop;
    switch (command) {
    case TelnetCommand::Break: code = kBreak; break;
    case TelnetCommand::InterruptProcess: code = kInterruptProcess; break;
    case TelnetCommand::AbortOutput: code = kAbortOutput; break;
    case TelnetCommand::AreYouThere: code = kAreYouThere; break;
    case TelnetCommand::EraseCharacter: code = kEraseCharacter; break;
    case TelnetCommand::EraseLine: code = kEraseLine; break;
    case TelnetCommand::GoAhead: code = kGoAhead; break;
    case TelnetCommand::Nop: code = kNop; break;
    case TelnetCommand::EndOfFile: code = kEndOfFile; break;
    case TelnetCommand::Suspend: code = kSuspend; break;
    case TelnetCommand::Abort: code = kAbort; break;
    }
    return std::string{static_cast<char>(kIac), static_cast<char>(code)};
}

} // namespace wt::net
