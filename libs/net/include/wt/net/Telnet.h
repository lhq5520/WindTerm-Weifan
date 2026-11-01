#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wt::net {

// Telnet commands and options (RFC 854/855 and the option RFCs). Names avoid the termios/stdio
// macros ECHO and EOF.
namespace telnet {
inline constexpr std::uint8_t kSe = 240;
inline constexpr std::uint8_t kNop = 241;
inline constexpr std::uint8_t kDataMark = 242;
inline constexpr std::uint8_t kBreak = 243;
inline constexpr std::uint8_t kInterruptProcess = 244;
inline constexpr std::uint8_t kAbortOutput = 245;
inline constexpr std::uint8_t kAreYouThere = 246;
inline constexpr std::uint8_t kEraseCharacter = 247;
inline constexpr std::uint8_t kEraseLine = 248;
inline constexpr std::uint8_t kGoAhead = 249;
inline constexpr std::uint8_t kSb = 250;
inline constexpr std::uint8_t kWill = 251;
inline constexpr std::uint8_t kWont = 252;
inline constexpr std::uint8_t kDo = 253;
inline constexpr std::uint8_t kDont = 254;
inline constexpr std::uint8_t kIac = 255;
inline constexpr std::uint8_t kEndOfFile = 236;
inline constexpr std::uint8_t kSuspend = 237;
inline constexpr std::uint8_t kAbort = 238;

inline constexpr std::uint8_t kOptBinary = 0;      // RFC 856
inline constexpr std::uint8_t kOptEcho = 1;        // RFC 857
inline constexpr std::uint8_t kOptSga = 3;         // RFC 858, suppress go-ahead
inline constexpr std::uint8_t kOptStatus = 5;      // RFC 859
inline constexpr std::uint8_t kOptTimingMark = 6;  // RFC 860
inline constexpr std::uint8_t kOptTerminalType = 24; // RFC 1091
inline constexpr std::uint8_t kOptNaws = 31;       // RFC 1073, window size
inline constexpr std::uint8_t kOptTerminalSpeed = 32; // RFC 1079
inline constexpr std::uint8_t kOptLineMode = 34;   // RFC 1184
inline constexpr std::uint8_t kOptNewEnviron = 39; // RFC 1572

// Subnegotiation verbs.
inline constexpr std::uint8_t kIs = 0;
inline constexpr std::uint8_t kSend = 1;
inline constexpr std::uint8_t kInfo = 2;
// NEW-ENVIRON item types.
inline constexpr std::uint8_t kEnvVar = 0;
inline constexpr std::uint8_t kEnvValue = 1;
inline constexpr std::uint8_t kEnvEsc = 2;
inline constexpr std::uint8_t kEnvUserVar = 3;
} // namespace telnet

struct TelnetConfig {
    // Answers to TERMINAL-TYPE requests, in order (RFC 1091 cycling; the last one repeats).
    std::vector<std::string> terminalTypes{"XTERM-256COLOR", "XTERM", "VT100"};
    int rows = 24;
    int columns = 80;
    // Sent through NEW-ENVIRON when the server asks (USER, DISPLAY, LANG, ...).
    std::vector<std::pair<std::string, std::string>> environment;
    std::string terminalSpeed = "38400,38400";
    // Ask for 8-bit transparent mode in both directions (servers that ask are always accepted).
    bool requestBinary = false;
    // Enter (CR) is sent as CR LF ("Telnet new line") when not in binary mode; otherwise CR NUL.
    bool crlf = true;
};

enum class TelnetCommand { Break, InterruptProcess, AbortOutput, AreYouThere, EraseCharacter, EraseLine, GoAhead, Nop,
                           EndOfFile, Suspend, Abort };

// Client side of the Telnet protocol as a pure state machine: no I/O, so the caller decides how
// bytes travel (sockets on an event loop, tests). Option negotiation follows the "Q method" of
// RFC 1143, which cannot loop. Supported: BINARY, ECHO and SGA from the server; BINARY, SGA,
// TERMINAL-TYPE, NAWS, TERMINAL-SPEED and NEW-ENVIRON on our side. Everything else is refused.
class TelnetProtocol {
public:
    explicit TelnetProtocol(TelnetConfig config = {});

    // Negotiation to send right after connecting (offers of the options above).
    std::string initialNegotiation();
    // Decodes bytes from the server. Returns the data for the terminal; answers to negotiation
    // and subnegotiation requests are appended to `reply` (send them to the server).
    std::string receive(std::string_view bytes, std::string& reply);
    // Encodes terminal input for the wire: doubles IAC and applies the NVT new-line rules.
    std::string encode(std::string_view input) const;
    // Records a new window size; returns the NAWS report to send (empty while NAWS is off).
    std::string resize(int rows, int columns);
    static std::string command(TelnetCommand command);

    // Option states: "local" options are performed by us (the server sent DO), "remote" ones by
    // the server (it sent WILL and we agreed).
    bool localEnabled(std::uint8_t option) const;
    bool remoteEnabled(std::uint8_t option) const;
    // The server does not echo: a client should echo typed characters itself (NVT default).
    bool wantsLocalEcho() const { return !remoteEnabled(telnet::kOptEcho); }
    // Number of negotiation commands seen (for diagnostics and tests).
    int negotiationCount() const { return m_negotiations; }

private:
    enum class Q : std::uint8_t { No, Yes, WantNo, WantYes };
    struct Option {
        Q us = Q::No;
        bool usOpposite = false;
        Q him = Q::No;
        bool himOpposite = false;
    };
    enum class State : std::uint8_t { Data, Cr, Iac, Will, Wont, Do, Dont, Sb, SbData, SbIac };

    bool acceptLocal(std::uint8_t option) const;
    bool acceptRemote(std::uint8_t option) const;
    void requestLocal(std::uint8_t option, bool enable, std::string& out);
    void requestRemote(std::uint8_t option, bool enable, std::string& out);
    void onWill(std::uint8_t option, std::string& reply);
    void onWont(std::uint8_t option, std::string& reply);
    void onDo(std::uint8_t option, std::string& reply);
    void onDont(std::uint8_t option, std::string& reply);
    void onLocalEnabled(std::uint8_t option, std::string& reply);
    void onSubnegotiation(std::string& reply);
    std::string nawsReport() const;
    std::string environmentReport(std::string_view request) const;

    TelnetConfig m_config;
    std::array<Option, 256> m_options{};
    State m_state = State::Data;
    std::uint8_t m_sbOption = 0;
    std::string m_sb;
    std::size_t m_terminalTypeIndex = 0;
    int m_negotiations = 0;
};

} // namespace wt::net
