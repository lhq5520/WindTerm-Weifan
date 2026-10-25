#include "WtTest.h"

#include "wt/platform/Files.h"
#include "wt/session/Backoff.h"
#include "wt/session/Keymap.h"
#include "wt/session/SessionProfile.h"
#include "wt/session/Settings.h"
#include "wt/session/Theme.h"

#include <filesystem>
#include <random>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace wt;
using namespace wt::session;

namespace {

std::string tempDir() {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("wt-session-test-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(dir);
    return dir.string();
}

SessionProfile sshProfile(std::string name, std::string host) {
    SessionProfile p;
    p.name = std::move(name);
    p.kind = SessionKind::Ssh;
    p.ssh.host = std::move(host);
    p.ssh.username = "alice";
    return p;
}

} // namespace

WT_TEST(themes_builtin_lookup_and_json_round_trip) {
    WT_CHECK(builtinThemes().size() >= 10u);
    WT_CHECK_EQ(builtinThemes().front().name, std::string(kDefaultThemeName));
    const auto dracula = findTheme("Dracula");
    WT_REQUIRE(dracula.has_value());
    WT_CHECK_EQ(dracula->palette.background, 0x282A36u);
    WT_CHECK_EQ(dracula->palette.indexed[1], 0xFF5555u);
    WT_CHECK_EQ(dracula->palette.indexed[200], terminal::paletteColor(200)); // extended colours unchanged
    WT_CHECK(!findTheme("No Such Theme"));

    auto parsed = themeFromJson(themeToJson(*dracula));
    WT_REQUIRE(parsed.has_value());
    WT_CHECK(*parsed == *dracula);

    Theme custom = *dracula;
    custom.name = "Dracula";
    custom.palette.background = 0x101010;
    WT_CHECK_EQ(findTheme("Dracula", {custom})->palette.background, 0x101010u); // custom shadows built-in

    std::string error;
    auto bad = core::Json::parse(R"({"name":"x","ansi":["#000000"]})");
    WT_CHECK(!themeFromJson(*bad, &error));
    WT_CHECK(!error.empty());
    WT_CHECK(!themeFromJson(*core::Json::parse(R"({"name":"x","background":"red"})")));
    WT_CHECK_EQ(formatHexColor(0x0A0B0C), std::string("#0a0b0c"));
    WT_CHECK(parseHexColor("0A0B0C") == std::optional<std::uint32_t>(0x0A0B0C));
    WT_CHECK(!parseHexColor("#12345"));
}

WT_TEST(keymap_defaults_have_no_conflicts) {
    for (auto flavor : {KeymapFlavor::Standard, KeymapFlavor::Mac}) {
        const Keymap map = Keymap::defaults(flavor);
        WT_CHECK(map.conflicts().empty());
        for (const ActionInfo& info : allActions()) {
            WT_CHECK(!map.sequences(info.action).empty());
            WT_CHECK(actionFromId(info.id) == info.action);
            WT_CHECK_EQ(actionId(info.action), info.id);
        }
    }
    WT_CHECK_EQ(allActions().size(), static_cast<std::size_t>(Action::Count_));
    WT_CHECK_EQ(Keymap::defaults(KeymapFlavor::Mac).sequences(Action::Copy).front(), std::string("Ctrl+C"));
    WT_CHECK_EQ(Keymap::defaults(KeymapFlavor::Standard).sequences(Action::Copy).front(),
                std::string("Ctrl+Shift+C"));
}

WT_TEST(keymap_json_stores_only_overrides) {
    const Keymap base = Keymap::defaults(KeymapFlavor::Standard);
    Keymap map = base;
    map.setSequences(Action::NewTab, {"Alt+T"});
    map.setSequences(Action::Reconnect, {});
    map.setSequences(Action::Find, {"Ctrl+Shift+T"}); // conflicts with nothing now
    const core::Json json = map.toJson(base);
    WT_CHECK_EQ(json.fields().size(), std::size_t(3));
    WT_CHECK_EQ(json["new-tab"].items().front().toString(), std::string("Alt+T"));
    WT_CHECK(json["reconnect"].items().empty());

    std::vector<std::string> warnings;
    auto input = core::Json::parse(R"({"new-tab":"Alt+T","bogus":"X","copy":["Ctrl+C","Ctrl+Insert"]})");
    const Keymap loaded = Keymap::fromJson(*input, base, &warnings);
    WT_CHECK_EQ(warnings.size(), std::size_t(1));
    WT_CHECK_EQ(loaded.sequences(Action::NewTab).front(), std::string("Alt+T"));
    WT_CHECK_EQ(loaded.sequences(Action::Copy).size(), std::size_t(2));
    WT_CHECK(loaded.sequences(Action::Paste) == base.sequences(Action::Paste));

    Keymap clash = base;
    clash.setSequences(Action::Copy, {"Ctrl+Shift+V"});
    const auto conflicts = clash.conflicts();
    WT_REQUIRE(conflicts.size() == 1u);
    WT_CHECK_EQ(conflicts[0].first, std::string("Ctrl+Shift+V"));
    WT_CHECK_EQ(conflicts[0].second.size(), std::size_t(2));
}

WT_TEST(settings_round_trip_and_validation) {
    Settings settings;
    settings.terminal.fontFamily = "JetBrains Mono";
    settings.terminal.fontSize = 13.5;
    settings.terminal.theme = "Nord";
    settings.terminal.scrollbackLines = 5000;
    settings.terminal.cursorStyle = CursorStyle::Bar;
    settings.terminal.cursorBlink = true;
    settings.terminal.optionAsMeta = true;
    settings.terminal.copyOnSelect = true;
    settings.terminal.allowClipboardWrite = true;
    settings.terminal.bell = BellStyle::None;
    settings.keymap.setSequences(Action::NewTab, {"Alt+N"});
    Theme custom = *findTheme("Nord");
    custom.name = "My Nord";
    settings.customThemes.push_back(custom);
    settings.confirmCloseRunning = false;
    settings.knownHostsFile = "/tmp/kh";

    std::vector<std::string> warnings;
    const Settings loaded = Settings::fromJson(*core::Json::parse(settings.toJson().dump(2)), &warnings);
    WT_CHECK(warnings.empty());
    WT_CHECK(loaded == settings);
    WT_CHECK_EQ(loaded.activeTheme().name, std::string("Nord"));

    auto messy = core::Json::parse(
        R"({"terminal":{"theme":"Gone","scrollbackLines":-5,"fontSize":1e9,"cursorStyle":"triangle","bell":3}})");
    const Settings fixed = Settings::fromJson(*messy, &warnings);
    WT_CHECK_EQ(fixed.terminal.theme, std::string(kDefaultThemeName));
    WT_CHECK_EQ(fixed.terminal.scrollbackLines, 0);
    WT_CHECK_EQ(fixed.terminal.fontSize, 0.0);
    WT_CHECK(fixed.terminal.cursorStyle == CursorStyle::Block);
    WT_CHECK(fixed.terminal.bell == BellStyle::Visual);
    WT_CHECK(warnings.size() >= 5u);
    WT_CHECK(Settings::fromJson(core::Json()) == Settings{}); // empty file -> defaults
}

WT_TEST(session_profiles_round_trip) {
    SessionStore store;
    SessionProfile local;
    local.name = "Build shell";
    local.group = "Dev/Local";
    local.local.program = "/bin/zsh";
    local.local.arguments = {"-l", "-i"};
    local.local.workingDirectory = "/tmp";
    local.local.environment = {{"FOO", "bar"}};
    const std::string localId = store.upsert(local);
    WT_CHECK(!localId.empty());

    SessionProfile ssh = sshProfile("prod", "example.com");
    ssh.group = "Servers";
    ssh.ssh.port = 2222;
    ssh.ssh.auth = AuthMethod::PublicKey;
    ssh.ssh.identityFile = "~/.ssh/id_ed25519";
    ssh.ssh.credentialId = "cred-1";
    ssh.ssh.keepAliveSeconds = 10;
    ssh.ssh.remoteCommand = "tmux attach";
    ssh.ssh.environment = {{"LC_ALL", "C.UTF-8"}};
    ssh.ssh.proxy.kind = ProxyKind::Socks5;
    ssh.ssh.proxy.host = "127.0.0.1";
    ssh.ssh.proxy.port = 1080;
    ssh.ssh.reconnect.maxAttempts = 3;
    ssh.theme = "Dracula";
    const std::string sshId = store.upsert(ssh);
    WT_CHECK(localId != sshId);
    WT_CHECK_EQ(store.find(sshId)->target(), std::string("alice@example.com:2222"));
    WT_CHECK(store.groups() == std::vector<std::string>({"Dev/Local", "Servers"}));

    std::vector<std::string> warnings;
    const SessionStore loaded = SessionStore::fromJson(*core::Json::parse(store.toJson().dump()), &warnings);
    WT_CHECK(warnings.empty());
    WT_REQUIRE(loaded.profiles().size() == 2u);
    WT_CHECK(loaded.profiles()[0] == store.profiles()[0]);
    WT_CHECK(loaded.profiles()[1] == store.profiles()[1]);

    // Update in place, duplicate, remove.
    SessionProfile renamed = *store.find(sshId);
    renamed.name = "production";
    WT_CHECK_EQ(store.upsert(renamed), sshId);
    WT_CHECK_EQ(store.find(sshId)->name, std::string("production"));
    const auto copyId = store.duplicate(sshId);
    WT_REQUIRE(copyId.has_value());
    WT_CHECK_EQ(store.find(*copyId)->name, std::string("production (copy)"));
    WT_CHECK(store.remove(*copyId));
    WT_CHECK(!store.remove(*copyId));
    WT_CHECK_EQ(store.profiles().size(), std::size_t(2));
}

WT_TEST(session_store_rejects_bad_entries) {
    std::vector<std::string> warnings;
    auto json = core::Json::parse(R"({"sessions":[{"name":"no id"},{"id":"a","kind":"telnet"},
        {"id":"b","name":"ok","kind":"ssh","ssh":{"host":"h","port":99999,"auth":"agent"}},
        {"id":"b","name":"dup"}]})");
    const SessionStore store = SessionStore::fromJson(*json, &warnings);
    WT_CHECK_EQ(store.profiles().size(), std::size_t(1));
    WT_CHECK_EQ(warnings.size(), std::size_t(3));
    WT_CHECK_EQ(store.profiles()[0].ssh.port, std::uint16_t(22)); // out of range -> default
    WT_CHECK(store.profiles()[0].ssh.auth == AuthMethod::Agent);
}

WT_TEST(session_validation_and_jump_chains) {
    SessionStore store;
    SessionProfile bastion = sshProfile("bastion", "bastion.example.com");
    bastion.id = "bastion";
    SessionProfile inner = sshProfile("inner", "10.0.0.5");
    inner.id = "inner";
    inner.ssh.proxy.kind = ProxyKind::JumpHost;
    inner.ssh.proxy.jumpProfileId = "bastion";
    SessionProfile deep = sshProfile("deep", "10.0.1.9");
    deep.id = "deep";
    deep.ssh.proxy.kind = ProxyKind::JumpHost;
    deep.ssh.proxy.jumpProfileId = "inner";
    store.upsert(bastion);
    store.upsert(inner);
    store.upsert(deep);

    std::vector<const SessionProfile*> chain;
    WT_CHECK(store.jumpChain(deep, chain).isOk());
    WT_REQUIRE(chain.size() == 2u);
    WT_CHECK_EQ(chain[0]->id, std::string("inner"));
    WT_CHECK_EQ(chain[1]->id, std::string("bastion"));
    WT_CHECK(store.validate(deep).empty());

    // Cycle: bastion -> deep -> inner -> bastion.
    bastion.ssh.proxy.kind = ProxyKind::JumpHost;
    bastion.ssh.proxy.jumpProfileId = "deep";
    store.upsert(bastion);
    WT_CHECK(!store.jumpChain(deep, chain).isOk());
    WT_CHECK(!store.validate(deep).empty());

    SessionProfile broken = sshProfile("", "");
    broken.ssh.port = 0;
    broken.ssh.auth = AuthMethod::PublicKey;
    broken.ssh.proxy.kind = ProxyKind::HttpConnect;
    WT_CHECK_EQ(store.validate(broken).size(), std::size_t(5));
    broken.ssh.proxy.kind = ProxyKind::JumpHost;
    broken.ssh.proxy.jumpProfileId = "missing";
    WT_CHECK(!store.jumpChain(broken, chain).isOk());
}

WT_TEST(session_store_file_persistence) {
    const std::string dir = tempDir();
    const std::string path = dir + "/nested/sessions.json";
    SessionStore store;
    WT_CHECK(store.load(path).isOk()); // missing file: empty store
    WT_CHECK(store.profiles().empty());
    store.upsert(sshProfile("one", "host1"));
    WT_REQUIRE(store.save(path).isOk());
#ifndef _WIN32
    struct stat info {};
    WT_REQUIRE(::stat(path.c_str(), &info) == 0);
    WT_CHECK_EQ(info.st_mode & 0777, 0600u);
#endif
    SessionStore reloaded;
    WT_REQUIRE(reloaded.load(path).isOk());
    WT_CHECK_EQ(reloaded.profiles().size(), std::size_t(1));
    WT_CHECK(platform::writeFileAtomic(path, "{broken").isOk());
    WT_CHECK(!reloaded.load(path).isOk());
    std::filesystem::remove_all(dir);
}

WT_TEST(files_atomic_write_and_read) {
    const std::string dir = tempDir();
    const std::string path = dir + "/a/b/file.txt";
    WT_REQUIRE(platform::writeFileAtomic(path, std::string("one\0two", 7)).isOk());
    WT_CHECK(platform::readFile(path) == std::optional<std::string>(std::string("one\0two", 7)));
    WT_REQUIRE(platform::writeFileAtomic(path, "replaced").isOk());
    WT_CHECK(platform::readFile(path) == std::optional<std::string>("replaced"));
    std::string error;
    WT_CHECK(!platform::readFile(dir + "/missing", &error));
    WT_CHECK(!error.empty());
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir + "/a/b")) {
        ++entries; // no temporary files left behind
    }
    WT_CHECK_EQ(entries, std::size_t(1));
    WT_CHECK(!platform::homeDirectory().empty());
    std::filesystem::remove_all(dir);
}

WT_TEST(backoff_grows_caps_and_stops) {
    ReconnectSettings settings;
    settings.maxAttempts = 6;
    settings.initialDelayMs = 1000;
    settings.maxDelayMs = 5000;
    Backoff backoff(settings, [] { return 0.5; }); // no jitter at 0.5
    std::vector<long long> delays;
    while (auto delay = backoff.next()) {
        delays.push_back(delay->count());
    }
    WT_CHECK(delays == std::vector<long long>({1000, 2000, 4000, 5000, 5000, 5000}));
    WT_CHECK_EQ(backoff.attempts(), 6);
    backoff.reset();
    WT_CHECK_EQ(backoff.next()->count(), 1000);

    Backoff jittered(settings, [] { return 0.0; });
    WT_CHECK_EQ(jittered.next()->count(), 800);
    settings.enabled = false;
    WT_CHECK(!Backoff(settings).next());
}
