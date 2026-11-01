// Unit tests for session logs, command history, input tracking and the new settings sections.

#include "WtTest.h"

#include "wt/platform/Files.h"
#include "wt/session/CommandHistory.h"
#include "wt/session/SessionLog.h"
#include "wt/session/Settings.h"

#include <filesystem>
#include <random>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace wt;
using namespace wt::session;

namespace {

std::string tempPath(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / ("wt-log-test-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

std::vector<std::string> assemble(PlainTextAssembler& text, std::string_view bytes) {
    std::vector<std::string> lines;
    text.feed(bytes, lines);
    return lines;
}

} // namespace

WT_TEST(plain_text_strips_sequences_and_applies_line_edits) {
    PlainTextAssembler text;
    // Colours, OSC titles (BEL and ST terminated), DCS strings and charset switches vanish.
    auto lines = assemble(text, "\x1b[1;31mred\x1b[0m \x1b]0;title\x07ok\x1b]2;t\x1b\\!\x1bP1$r0m\x1b\\\x1b(B\r\n");
    WT_REQUIRE(lines.size() == 1u);
    WT_CHECK_EQ(lines[0], std::string("red ok!"));
    // Progress bars rewrite the line with CR; shells redraw with CR + text + erase-in-line.
    lines = assemble(text, "10%\r50%\r100%\n");
    WT_CHECK(lines == std::vector<std::string>({"100%"}));
    lines = assemble(text, "$ lss\b \b -l\n"); // a typo erased the way shells echo it
    WT_CHECK(lines == std::vector<std::string>({"$ ls -l"}));
    lines = assemble(text, "$ long command\r$ short\x1b[K\n");
    WT_CHECK(lines == std::vector<std::string>({"$ short"}));
    // Cursor left/right, delete and insert characters (readline editing in the middle).
    lines = assemble(text, "abcdef\x1b[3D\x1b[PX\x1b[2@\x1b[C\x1b[CY\n");
    WT_CHECK(lines == std::vector<std::string>({"abcX  Y"}));
    // Tabs, wide characters and combining marks.
    lines = assemble(text, "a\tb\n\xe4\xb8\xad\xe6\x96\x87x\n" "e\xcc\x81\n");
    WT_CHECK(lines == std::vector<std::string>({"a       b", "\xe4\xb8\xad\xe6\x96\x87x", "e\xcc\x81"}));
    lines = assemble(text, "\xe4\xb8\xad\b\bA\n"); // backspace over a wide character
    WT_CHECK(lines == std::vector<std::string>({"A"}));
    // Split sequences and UTF-8 across feeds; the prompt stays the current line.
    WT_CHECK(assemble(text, "\x1b[3").empty());
    WT_CHECK(assemble(text, "2mgreen\xe2").empty());
    WT_CHECK(assemble(text, "\x82\xac$ ").empty());
    WT_CHECK_EQ(text.currentLine(), std::string("green\xe2\x82\xac$"));
    const auto before = text.lineNumber();
    WT_CHECK_EQ(text.takeCurrentLine(), std::string("green\xe2\x82\xac$"));
    WT_CHECK_EQ(text.lineNumber(), before + 1);
    WT_CHECK(!text.hasCurrentLine());
}

WT_TEST(session_logger_text_raw_and_timestamps) {
    const std::string path = tempPath("session.log");
    SessionLogger log;
    WT_REQUIRE(log.open(path, LogFormat::Text, false).isOk());
    WT_CHECK(log.isOpen());
    log.write("\x1b[32mhello\x1b[0m\r\nprompt$ ");
    log.note("[Connection closed]");
    log.write("after\n");
    log.close();
    WT_CHECK_EQ(*platform::readFile(path), std::string("hello\nprompt$\n[Connection closed]\nafter\n"));
#ifndef _WIN32
    struct stat info {};
    WT_REQUIRE(::stat(path.c_str(), &info) == 0);
    WT_CHECK_EQ(info.st_mode & 0777, 0600u); // logs are private
#endif

    // Appends; timestamps come from the clock.
    SessionLogger stamped;
    stamped.setClock([] { return std::chrono::system_clock::from_time_t(86400 * 365) + std::chrono::milliseconds(42); });
    WT_REQUIRE(stamped.open(path, LogFormat::Text, true).isOk());
    stamped.write("x\n");
    stamped.close();
    const std::string contents = *platform::readFile(path);
    const auto last = contents.rfind('[');
    WT_CHECK(contents.find("after\n[") != std::string::npos);
    WT_CHECK_EQ(contents.substr(contents.size() - 2), std::string("x\n"));
    WT_CHECK_EQ(contents.substr(last + 20, 4), std::string(".042")); // [YYYY-MM-DD HH:MM:SS.mmm]

    const std::string raw = tempPath("raw.log");
    SessionLogger rawLog;
    WT_REQUIRE(rawLog.open(raw, LogFormat::Raw, true).isOk());
    rawLog.write("\x1b[1mbold\x1b[0m\r\n");
    WT_CHECK_EQ(rawLog.bytesWritten(), std::uint64_t(14));
    rawLog.close();
    WT_CHECK_EQ(*platform::readFile(raw), std::string("\x1b[1mbold\x1b[0m\r\n"));

    SessionLogger bad;
    WT_CHECK(!bad.open(raw + "/not-a-directory/here.log", LogFormat::Text, false).isOk());
    bad.write("ignored"); // writing to a closed logger is harmless
}

WT_TEST(log_file_name_patterns) {
    std::tm time{};
    time.tm_year = 2026 - 1900;
    time.tm_mon = 10;
    time.tm_mday = 1;
    time.tm_hour = 9;
    time.tm_min = 5;
    time.tm_sec = 7;
    WT_CHECK_EQ(expandLogFileName("%Y-%m-%d_%H-%M-%S_%N.log", "prod/db", "10.0.0.1", time),
                std::string("2026-11-01_09-05-07_prod_db.log"));
    WT_CHECK_EQ(expandLogFileName("%h-%%-%x", "", "a:b", time), std::string("a_b-%-%x"));
    WT_CHECK_EQ(expandLogFileName("%N", "", "", time), std::string("session"));
}

WT_TEST(command_history_completion_and_persistence) {
    CommandHistory history;
    WT_CHECK(history.add("host-a", "git status", 100));
    WT_CHECK(history.add("host-a", "git commit -m fix", 101));
    WT_CHECK(history.add("host-b", "git log --oneline", 102));
    WT_CHECK(history.add("host-a", "ls -la", 103));
    WT_CHECK(!history.add("host-a", " secret --token=x")); // leading space: not remembered
    WT_CHECK(!history.add("host-a", ""));
    WT_CHECK(!history.add("host-a", "multi\nline"));
    WT_CHECK(history.add("host-a", "git status   ", 104)); // moves to the front, trailing blanks dropped
    WT_CHECK_EQ(history.size(), std::size_t(4));
    // Same context first (newest first), then the others; exact matches are not suggested.
    WT_CHECK(history.complete("host-a", "git") ==
             std::vector<std::string>({"git status", "git commit -m fix", "git log --oneline"}));
    WT_CHECK(history.complete("host-b", "git") ==
             std::vector<std::string>({"git log --oneline", "git status", "git commit -m fix"}));
    WT_CHECK(history.complete("host-a", "git status").empty());
    WT_CHECK(history.complete("host-a", "g", 1) == std::vector<std::string>({"git status"}));

    const std::string path = tempPath("history.json");
    WT_REQUIRE(history.save(path).isOk());
    CommandHistory loaded;
    WT_REQUIRE(loaded.load(path).isOk());
    WT_CHECK(loaded.complete("host-a", "git") == history.complete("host-a", "git"));
    CommandHistory missing;
    WT_CHECK(missing.load(path + ".none").isOk());
    WT_CHECK_EQ(missing.size(), std::size_t(0));

    loaded.setLimit(2);
    WT_CHECK_EQ(loaded.size(), std::size_t(2));
    WT_CHECK(loaded.remove("git status"));
    WT_CHECK(!loaded.remove("git status"));
}

WT_TEST(input_line_tracker_follows_typing) {
    InputLineTracker tracker;
    auto submitted = tracker.feed("ls -lx\x7f" "a");
    WT_CHECK(submitted.empty());
    WT_CHECK_EQ(tracker.line(), std::string("ls -la"));
    WT_CHECK(tracker.reliable());
    submitted = tracker.feed("\r");
    WT_REQUIRE(submitted.size() == 1u);
    WT_CHECK_EQ(submitted[0].text, std::string("ls -la"));
    WT_CHECK(submitted[0].reliable);
    WT_CHECK(tracker.empty());

    // UTF-8 backspace, Ctrl+W, Ctrl+U.
    tracker.feed("echo \xe4\xb8\xad\xe6\x96\x87\x7f");
    WT_CHECK_EQ(tracker.line(), std::string("echo \xe4\xb8\xad"));
    tracker.feed("\x17");
    WT_CHECK_EQ(tracker.line(), std::string("echo "));
    tracker.feed("\x15");
    WT_CHECK(tracker.empty());

    // Arrow keys / history recall / tab completion make the line unknowable.
    tracker.feed("git st\x1b[A");
    WT_CHECK(!tracker.reliable());
    submitted = tracker.feed("\r");
    WT_REQUIRE(submitted.size() == 1u);
    WT_CHECK(!submitted[0].reliable);
    WT_CHECK(tracker.reliable()); // a fresh line
    tracker.feed("cd /us\t");
    WT_CHECK(!tracker.reliable());
    tracker.feed("\x03"); // Ctrl+C abandons it
    WT_CHECK(tracker.reliable());
    WT_CHECK(tracker.empty());

    // A lone Escape key press does not swallow the Enter that follows it.
    tracker.feed("abc");
    tracker.feed("\x1b");
    submitted = tracker.feed("\r");
    WT_REQUIRE(submitted.size() == 1u);
    WT_CHECK(!submitted[0].reliable);

    // A bracketed paste with several lines submits each of them.
    submitted = tracker.feed("\x1b[200~one\rtwo\x1b[201~\r");
    WT_REQUIRE(submitted.size() == 2u);
    WT_CHECK_EQ(submitted[0].text, std::string("one"));
    WT_CHECK_EQ(submitted[1].text, std::string("two"));
    WT_CHECK(submitted[1].reliable);
}

WT_TEST(settings_logging_triggers_and_completion) {
    Settings settings;
    settings.logging.autoStart = true;
    settings.logging.directory = "/tmp/logs";
    settings.logging.format = LogFormat::Raw;
    settings.logging.timestamps = false;
    settings.triggers = {{"errors", "error|fail(ed)?", false, true, TriggerAction::Highlight, "#ffffff", "#aa0000", ""},
                         {"sudo", "\\[sudo\\] password", true, false, TriggerAction::Send, "", "", "secret\\r"}};
    settings.completion.enabled = false;
    settings.completion.minimumPrefix = 3;
    std::vector<std::string> warnings;
    const Settings loaded = Settings::fromJson(*core::Json::parse(settings.toJson().dump()), &warnings);
    WT_CHECK(warnings.empty());
    WT_CHECK(loaded == settings);

    auto json = core::Json::parse(R"({"logging":{"format":"xml","fileName":""},
        "triggers":[{"name":"empty"},{"name":"bad colour","pattern":"x","background":"red"}],
        "completion":{"minimumPrefix":99}})");
    warnings.clear();
    const Settings fixed = Settings::fromJson(*json, &warnings);
    WT_CHECK_EQ(warnings.size(), std::size_t(3)); // format, empty pattern, colour
    WT_CHECK(fixed.logging.format == LogFormat::Text);
    WT_CHECK_EQ(fixed.logging.fileName, LoggingSettings{}.fileName);
    WT_REQUIRE(fixed.triggers.size() == 1u);
    WT_CHECK(fixed.triggers[0].background.empty());
    WT_CHECK_EQ(fixed.completion.minimumPrefix, 20);

    WT_CHECK_EQ(unescapeTriggerText("yes\\r\\n\\t\\e\\\\\\q"), std::string("yes\r\n\t\x1b\\\\q"));
}
