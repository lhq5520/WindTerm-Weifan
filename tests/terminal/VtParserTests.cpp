#include "WtTest.h"

#include "wt/terminal/VtParser.h"

#include <string>
#include <vector>

using namespace wt::terminal;

#define ESC "\x1b"

namespace {

// Records parser callbacks as compact strings, e.g. "csi(?|25|h)".
class Recorder : public VtHandler {
public:
    std::vector<std::string> events;
    std::u32string printed;

    void print(char32_t cp) override { printed.push_back(cp); }
    void execute(std::uint8_t c) override { events.push_back("exec(" + std::to_string(c) + ")"); }
    void csiDispatch(const CsiParams& params, char prefix, std::string_view intermediates, char final) override {
        std::string s = "csi(";
        if (prefix) {
            s += prefix;
        }
        s += "|";
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i > 0) {
                s += params.isSubParam(i) ? ":" : ";";
            }
            s += std::to_string(params.raw(i));
        }
        s += "|" + std::string(intermediates) + final + ")";
        events.push_back(s);
    }
    void escDispatch(std::string_view intermediates, char final) override {
        events.push_back("esc(" + std::string(intermediates) + final + ")");
    }
    void oscDispatch(std::string_view payload, bool bell) override {
        events.push_back(std::string(bell ? "osc-bel(" : "osc(") + std::string(payload) + ")");
    }
};

std::vector<std::string> parse(std::string_view input, std::u32string* printed = nullptr) {
    Recorder recorder;
    VtParser parser(recorder);
    parser.feed(input);
    if (printed) {
        *printed = recorder.printed;
    }
    return recorder.events;
}

using Events = std::vector<std::string>;

} // namespace

WT_TEST(parser_prints_text_and_utf8) {
    std::u32string printed;
    WT_CHECK(parse("hi 中文\xF0\x9F\x98\x80", &printed).empty());
    WT_CHECK(printed == U"hi 中文\U0001F600");
}

WT_TEST(parser_executes_c0_controls) {
    WT_CHECK(parse("\r\n\b\t\a") == (Events{"exec(13)", "exec(10)", "exec(8)", "exec(9)", "exec(7)"}));
}

WT_TEST(parser_csi_params_prefix_and_intermediates) {
    WT_CHECK(parse(ESC "[1;31m") == Events{"csi(|1;31|m)"});
    WT_CHECK(parse(ESC "[m") == Events{"csi(||m)"});
    WT_CHECK(parse(ESC "[;5H") == Events{"csi(|0;5|H)"});
    WT_CHECK(parse(ESC "[?1049h") == Events{"csi(?|1049|h)"});
    WT_CHECK(parse(ESC "[>c") == Events{"csi(>||c)"});
    WT_CHECK(parse(ESC "[2 q") == Events{"csi(|2| q)"});
    WT_CHECK(parse(ESC "[!p") == Events{"csi(||!p)"});
}

WT_TEST(parser_csi_subparameters) {
    WT_CHECK(parse(ESC "[38:2::10:20:30m") == Events{"csi(|38:2:0:10:20:30|m)"});
    WT_CHECK(parse(ESC "[4:3;1m") == Events{"csi(|4:3;1|m)"});
}

WT_TEST(parser_csi_param_overflow_is_clamped) {
    WT_CHECK(parse(ESC "[99999999A") == Events{"csi(|65535|A)"});
    std::string many = ESC "[";
    for (int i = 0; i < 40; ++i) {
        many += "1;";
    }
    many += "m";
    const auto events = parse(many);
    WT_REQUIRE(events.size() == 1);
    WT_CHECK(events[0].size() < 100); // capped at 32 params
}

WT_TEST(parser_sequences_split_across_feeds) {
    Recorder recorder;
    VtParser parser(recorder);
    for (char c : std::string(ESC "[12;3" "4H" ESC "]0;ti" "tle\a\xE4\xB8\xAD")) {
        parser.feed(std::string_view(&c, 1));
    }
    WT_CHECK(recorder.events == (Events{"csi(|12;34|H)", "osc-bel(0;title)"}));
    WT_CHECK(recorder.printed == U"中");
}

WT_TEST(parser_c0_inside_csi_is_executed) {
    WT_CHECK(parse(ESC "[1\r2H") == (Events{"exec(13)", "csi(|12|H)"}));
}

WT_TEST(parser_escape_sequences) {
    WT_CHECK(parse(ESC "7" ESC "8" ESC "M") == (Events{"esc(7)", "esc(8)", "esc(M)"}));
    WT_CHECK(parse(ESC "(0" ESC "(B" ESC "#8") == (Events{"esc((0)", "esc((B)", "esc(#8)"}));
}

WT_TEST(parser_osc_terminators) {
    WT_CHECK(parse(ESC "]2;bel\a") == Events{"osc-bel(2;bel)"});
    WT_CHECK(parse(ESC "]2;st" ESC "\\") == (Events{"osc(2;st)", "esc(\\)"}));
    WT_CHECK(parse(ESC "]0;\xE6\xA0\x87\xE9\xA2\x98\a") == Events{"osc-bel(0;标题)"});
}

WT_TEST(parser_can_aborts_sequence) {
    std::u32string printed;
    WT_CHECK(parse(ESC "[12\x18" "A", &printed) == Events{"exec(24)"});
    WT_CHECK(printed == U"A");
}

WT_TEST(parser_ignores_dcs_and_apc_strings) {
    std::u32string printed;
    const auto events = parse(ESC "P1$r0m" ESC "\\" ESC "_apc payload" ESC "\\ok", &printed);
    WT_CHECK(events == (Events{"esc(\\)", "esc(\\)"}));
    WT_CHECK(printed == U"ok");
}

WT_TEST(parser_private_marker_after_params_is_ignored) {
    std::u32string printed;
    WT_CHECK(parse(ESC "[1?hX", &printed).empty());
    WT_CHECK(printed == U"X");
}
