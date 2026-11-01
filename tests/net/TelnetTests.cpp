// Unit tests for the Telnet protocol state machine (no network involved).

#include "WtTest.h"

#include "wt/net/Telnet.h"

using namespace wt::net;
using namespace wt::net::telnet;

namespace {

std::string cmd(std::uint8_t verb, std::uint8_t option) {
    return std::string{static_cast<char>(kIac), static_cast<char>(verb), static_cast<char>(option)};
}

std::string sb(std::uint8_t option, const std::string& payload) {
    std::string out{static_cast<char>(kIac), static_cast<char>(kSb), static_cast<char>(option)};
    out += payload;
    out += static_cast<char>(kIac);
    out += static_cast<char>(kSe);
    return out;
}

std::string bytes(std::initializer_list<int> values) {
    std::string out;
    for (const int v : values) {
        out += static_cast<char>(v);
    }
    return out;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

WT_TEST(telnet_initial_negotiation_offers_supported_options) {
    TelnetProtocol protocol;
    const std::string offer = protocol.initialNegotiation();
    WT_CHECK(contains(offer, cmd(kWill, kOptNaws)));
    WT_CHECK(contains(offer, cmd(kWill, kOptTerminalType)));
    WT_CHECK(contains(offer, cmd(kWill, kOptSga)));
    WT_CHECK(contains(offer, cmd(kDo, kOptSga)));
    WT_CHECK(contains(offer, cmd(kDo, kOptEcho)));
    WT_CHECK(!contains(offer, cmd(kWill, kOptNewEnviron))); // nothing to send
    WT_CHECK(!contains(offer, cmd(kDo, kOptBinary)));       // only on request
    // Nothing is enabled until the server agrees.
    WT_CHECK(!protocol.localEnabled(kOptNaws));
    WT_CHECK(protocol.wantsLocalEcho());
}

WT_TEST(telnet_agreement_to_our_offer_needs_no_reply) {
    TelnetProtocol protocol;
    protocol.initialNegotiation();
    std::string reply;
    // DO NAWS answers our WILL NAWS: no WILL again, but the size report follows (RFC 1073).
    protocol.receive(cmd(kDo, kOptNaws) + cmd(kWill, kOptEcho) + cmd(kWill, kOptSga) + cmd(kDo, kOptSga), reply);
    WT_CHECK(protocol.localEnabled(kOptNaws));
    WT_CHECK(protocol.remoteEnabled(kOptEcho));
    WT_CHECK(protocol.remoteEnabled(kOptSga));
    WT_CHECK(!protocol.wantsLocalEcho());
    WT_CHECK_EQ(reply, sb(kOptNaws, bytes({0, 80, 0, 24})));
    // Repeating an agreed option must not start a negotiation loop.
    reply.clear();
    protocol.receive(cmd(kDo, kOptNaws) + cmd(kWill, kOptEcho), reply);
    WT_CHECK_EQ(reply, std::string());
}

WT_TEST(telnet_server_initiated_options) {
    TelnetProtocol protocol; // no initial negotiation: the server starts
    std::string reply;
    const std::string data = protocol.receive(cmd(kDo, kOptTerminalType) + cmd(kWill, kOptEcho) +
                                                  cmd(kDo, 99) + cmd(kWill, 98) + cmd(kDo, kOptLineMode) + "login: ",
                                              reply);
    WT_CHECK_EQ(data, std::string("login: "));
    WT_CHECK_EQ(reply, cmd(kWill, kOptTerminalType) + cmd(kDo, kOptEcho) + cmd(kWont, 99) + cmd(kDont, 98) +
                           cmd(kWont, kOptLineMode));
    WT_CHECK(protocol.localEnabled(kOptTerminalType));
    WT_CHECK(!protocol.localEnabled(99));
    // A refused option stays refused without loops.
    reply.clear();
    protocol.receive(cmd(kWont, 98), reply);
    WT_CHECK_EQ(reply, std::string());
    // Turning ECHO off is acknowledged and switches to local echo.
    protocol.receive(cmd(kWont, kOptEcho), reply);
    WT_CHECK_EQ(reply, cmd(kDont, kOptEcho));
    WT_CHECK(protocol.wantsLocalEcho());
    WT_CHECK(protocol.negotiationCount() == 7);
}

WT_TEST(telnet_terminal_type_cycles_through_the_list) {
    TelnetConfig config;
    config.terminalTypes = {"XTERM-256COLOR", "VT100"};
    TelnetProtocol protocol(config);
    std::string reply;
    protocol.receive(cmd(kDo, kOptTerminalType), reply);
    std::vector<std::string> answers;
    for (int i = 0; i < 4; ++i) {
        reply.clear();
        protocol.receive(sb(kOptTerminalType, bytes({kSend})), reply);
        answers.push_back(reply);
    }
    const std::string is(1, static_cast<char>(kIs));
    WT_CHECK_EQ(answers[0], sb(kOptTerminalType, is + "XTERM-256COLOR"));
    WT_CHECK_EQ(answers[1], sb(kOptTerminalType, is + "VT100"));
    WT_CHECK_EQ(answers[2], sb(kOptTerminalType, is + "VT100")); // repeated: end of list
    WT_CHECK_EQ(answers[3], sb(kOptTerminalType, is + "XTERM-256COLOR"));
}

WT_TEST(telnet_subnegotiation_requires_the_option) {
    TelnetProtocol protocol;
    std::string reply;
    protocol.receive(sb(kOptTerminalType, bytes({kSend})) + sb(kOptNewEnviron, bytes({kSend})), reply);
    WT_CHECK_EQ(reply, std::string()); // never agreed: do not leak anything
}

WT_TEST(telnet_window_size_reports) {
    TelnetProtocol protocol;
    std::string reply;
    WT_CHECK_EQ(protocol.resize(40, 120), std::string()); // NAWS not enabled yet
    protocol.receive(cmd(kDo, kOptNaws), reply);
    WT_CHECK_EQ(reply, cmd(kWill, kOptNaws) + sb(kOptNaws, bytes({0, 120, 0, 40})));
    // 255 inside the payload is doubled.
    WT_CHECK_EQ(protocol.resize(255, 511), sb(kOptNaws, bytes({1, 255, 255, 0, 255, 255})));
    // DONT NAWS stops the reports.
    reply.clear();
    protocol.receive(cmd(kDont, kOptNaws), reply);
    WT_CHECK_EQ(reply, cmd(kWont, kOptNaws));
    WT_CHECK_EQ(protocol.resize(30, 100), std::string());
}

WT_TEST(telnet_data_escaping_and_split_input) {
    TelnetProtocol protocol;
    std::string reply;
    std::string data = protocol.receive(bytes({'a', kIac, kIac, 'b'}), reply);
    WT_CHECK_EQ(data, bytes({'a', 0xFF, 'b'}));
    // A command split across reads.
    data = protocol.receive(bytes({'x', kIac}), reply);
    WT_CHECK_EQ(data, std::string("x"));
    data = protocol.receive(bytes({kWill}), reply);
    WT_CHECK_EQ(data, std::string());
    data = protocol.receive(bytes({kOptEcho, 'y'}), reply);
    WT_CHECK_EQ(data, std::string("y"));
    WT_CHECK(protocol.remoteEnabled(kOptEcho));
    // A subnegotiation split across reads, with a doubled IAC inside.
    reply.clear();
    protocol.receive(cmd(kDo, kOptTerminalType), reply);
    reply.clear();
    protocol.receive(bytes({kIac, kSb, kOptTerminalType}), reply);
    protocol.receive(bytes({kSend, kIac}), reply);
    data = protocol.receive(bytes({kSe, 'z'}), reply);
    WT_CHECK_EQ(data, std::string("z"));
    WT_CHECK(contains(reply, "XTERM-256COLOR"));
    // Other commands (NOP, GA, DM) are swallowed.
    data = protocol.receive(bytes({'1', kIac, kNop, '2', kIac, kGoAhead, '3', kIac, kDataMark}), reply);
    WT_CHECK_EQ(data, std::string("123"));
}

WT_TEST(telnet_newline_rules) {
    TelnetProtocol protocol;
    std::string reply;
    // In NVT mode "CR NUL" is a bare carriage return; "CR LF" is kept.
    WT_CHECK_EQ(protocol.receive(bytes({'a', '\r', 0, 'b', '\r', '\n'}), reply), std::string("a\rb\r\n"));
    // CR at the end of one read, NUL at the start of the next.
    WT_CHECK_EQ(protocol.receive(std::string("c\r"), reply), std::string("c\r"));
    WT_CHECK_EQ(protocol.receive(bytes({0, 'd'}), reply), std::string("d"));
    // Enter goes out as CR LF; pasted CR LF is not doubled; IAC is escaped.
    WT_CHECK_EQ(protocol.encode("ls\r"), std::string("ls\r\n"));
    WT_CHECK_EQ(protocol.encode("a\r\nb"), std::string("a\r\nb"));
    WT_CHECK_EQ(protocol.encode(bytes({0xFF, 'q'})), bytes({0xFF, 0xFF, 'q'}));
    TelnetConfig config;
    config.crlf = false;
    TelnetProtocol nul(config);
    WT_CHECK_EQ(nul.encode("x\r"), bytes({'x', '\r', 0}));
}

WT_TEST(telnet_binary_mode_is_transparent) {
    TelnetConfig config;
    config.requestBinary = true;
    TelnetProtocol protocol(config);
    const std::string offer = protocol.initialNegotiation();
    WT_CHECK(contains(offer, cmd(kWill, kOptBinary)));
    WT_CHECK(contains(offer, cmd(kDo, kOptBinary)));
    std::string reply;
    protocol.receive(cmd(kDo, kOptBinary) + cmd(kWill, kOptBinary), reply);
    WT_CHECK_EQ(reply, std::string());
    WT_CHECK(protocol.localEnabled(kOptBinary));
    WT_CHECK(protocol.remoteEnabled(kOptBinary));
    WT_CHECK_EQ(protocol.receive(bytes({'\r', 0, 'x'}), reply), bytes({'\r', 0, 'x'}));
    WT_CHECK_EQ(protocol.encode("a\r"), std::string("a\r"));
    WT_CHECK_EQ(protocol.encode(bytes({0xFF})), bytes({0xFF, 0xFF})); // IAC is always escaped
}

WT_TEST(telnet_new_environ) {
    TelnetConfig config;
    config.environment = {{"USER", "alice"}, {"LANG", "en_US.UTF-8"}, {"ODD", std::string("a\x01" "b")}};
    TelnetProtocol protocol(config);
    WT_CHECK(contains(protocol.initialNegotiation(), cmd(kWill, kOptNewEnviron)));
    std::string reply;
    protocol.receive(cmd(kDo, kOptNewEnviron), reply);
    WT_CHECK(protocol.localEnabled(kOptNewEnviron));
    // Specific variables: known ones get values, unknown ones are reported as undefined.
    reply.clear();
    protocol.receive(sb(kOptNewEnviron, bytes({kSend, kEnvVar, 'U', 'S', 'E', 'R', kEnvUserVar, 'N', 'O', 'P', 'E'})),
                     reply);
    WT_CHECK_EQ(reply, sb(kOptNewEnviron, bytes({kIs, kEnvVar, 'U', 'S', 'E', 'R', kEnvValue, 'a', 'l', 'i', 'c', 'e',
                                                 kEnvUserVar, 'N', 'O', 'P', 'E'})));
    // Everything; well-known names use VAR, others USERVAR; type bytes inside values are escaped.
    reply.clear();
    protocol.receive(sb(kOptNewEnviron, bytes({kSend})), reply);
    WT_CHECK(contains(reply, bytes({kEnvVar, 'U', 'S', 'E', 'R', kEnvValue})));
    WT_CHECK(contains(reply, bytes({kEnvUserVar, 'L', 'A', 'N', 'G', kEnvValue})));
    WT_CHECK(contains(reply, bytes({kEnvValue, 'a', kEnvEsc, 1, 'b'})));
}

WT_TEST(telnet_terminal_speed_and_commands) {
    TelnetProtocol protocol;
    std::string reply;
    protocol.receive(cmd(kDo, kOptTerminalSpeed), reply);
    reply.clear();
    protocol.receive(sb(kOptTerminalSpeed, bytes({kSend})), reply);
    WT_CHECK_EQ(reply, sb(kOptTerminalSpeed, std::string(1, static_cast<char>(kIs)) + "38400,38400"));
    WT_CHECK_EQ(TelnetProtocol::command(TelnetCommand::Break), bytes({kIac, kBreak}));
    WT_CHECK_EQ(TelnetProtocol::command(TelnetCommand::InterruptProcess), bytes({kIac, kInterruptProcess}));
    WT_CHECK_EQ(TelnetProtocol::command(TelnetCommand::AreYouThere), bytes({kIac, kAreYouThere}));
}

WT_TEST(telnet_q_method_handles_crossed_requests) {
    TelnetProtocol protocol;
    protocol.initialNegotiation(); // WILL NAWS sent (WANTYES)
    std::string reply;
    // The server refuses: WANTYES -> NO without a reply.
    protocol.receive(cmd(kDont, kOptNaws), reply);
    WT_CHECK_EQ(reply, std::string());
    WT_CHECK(!protocol.localEnabled(kOptNaws));
    // It changes its mind later: accepted normally.
    protocol.receive(cmd(kDo, kOptNaws), reply);
    WT_CHECK(protocol.localEnabled(kOptNaws));
    WT_CHECK(contains(reply, cmd(kWill, kOptNaws)));
    // A garbage stream never produces unbounded replies.
    std::string noise;
    for (int i = 0; i < 1000; ++i) {
        noise += cmd(static_cast<std::uint8_t>(kWill + (i % 4)), static_cast<std::uint8_t>(i % 7));
    }
    reply.clear();
    protocol.receive(noise, reply);
    WT_CHECK(reply.size() <= noise.size());
}
