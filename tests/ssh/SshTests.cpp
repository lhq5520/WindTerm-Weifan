// Integration tests for wt::ssh against a real (unprivileged) OpenSSH server.

#include "SshdFixture.h"
#include "TestServers.h"
#include "WtTest.h"

#include "wt/platform/Files.h"
#include "wt/ssh/KnownHosts.h"
#include "wt/ssh/Sftp.h"
#include "wt/ssh/SshConnection.h"
#include "wt/ssh/SshShell.h"

#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>

using namespace wt;
using namespace std::chrono_literals;

namespace {

test::SshdFixture* server() {
    static test::SshdFixture fixture;
    static bool started = false;
    static bool tried = false;
    if (!tried) {
        tried = true;
        ::unsetenv("SSH_AUTH_SOCK"); // never use the developer's agent
        std::string reason;
        started = fixture.start(&reason);
        if (!started) {
            std::fprintf(stderr, "    SKIP: %s\n", reason.c_str());
        }
    }
    return started ? &fixture : nullptr;
}

struct ScriptedUi : ssh::SshInteraction {
    ssh::HostKeyDecision decision = ssh::HostKeyDecision::AcceptAndSave;
    int hostKeyPrompts = 0;
    ssh::HostKeyCheck lastCheck = ssh::HostKeyCheck::Failure;
    ssh::HostKey lastKey;
    std::vector<std::string> passwords;
    int passwordPrompts = 0;
    std::vector<std::string> passphrases;
    int passphrasePrompts = 0;
    std::vector<std::string> infos;

    ssh::HostKeyDecision confirmHostKey(const std::string&, std::uint16_t, const ssh::HostKey& key,
                                        ssh::HostKeyCheck check) override {
        ++hostKeyPrompts;
        lastCheck = check;
        lastKey = key;
        return decision;
    }
    std::optional<std::string> askPassword(const std::string&) override {
        ++passwordPrompts;
        if (passwords.empty()) {
            return std::nullopt;
        }
        std::string next = passwords.front();
        passwords.erase(passwords.begin());
        return next;
    }
    std::optional<std::string> askPassphrase(const std::string&) override {
        ++passphrasePrompts;
        if (passphrases.empty()) {
            return std::nullopt;
        }
        std::string next = passphrases.front();
        passphrases.erase(passphrases.begin());
        return next;
    }
    std::optional<std::vector<std::string>> keyboardInteractive(const std::string&, const std::string&,
                                                                const std::vector<ssh::KeyboardPrompt>&) override {
        return std::nullopt;
    }
    void info(const std::string& message) override { infos.push_back(message); }
};

ssh::ConnectOptions baseOptions(const test::SshdFixture& sshd) {
    ssh::ConnectOptions options;
    options.host = "127.0.0.1";
    options.port = sshd.port();
    options.username = test::SshdFixture::user();
    options.auth = ssh::AuthMethod::PublicKey;
    options.identityFile = sshd.userKey();
    options.knownHostsPath = sshd.knownHosts();
    options.timeout = 10s;
    options.tryDefaultKeys = false;
    return options;
}

// Collects shell output on the test thread.
struct ShellSink {
    std::mutex mutex;
    std::condition_variable changed;
    std::string output;
    bool closed = false;
    int exitStatus = -2;
    std::string error;

    ssh::SshShell::Callbacks callbacks() {
        return {[this](std::string_view data) {
                    std::lock_guard lock(mutex);
                    output.append(data);
                    changed.notify_all();
                },
                [this](int status, const std::string& message) {
                    std::lock_guard lock(mutex);
                    closed = true;
                    exitStatus = status;
                    error = message;
                    changed.notify_all();
                },
                {}};
    }
    bool waitFor(const std::string& text, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return output.find(text) != std::string::npos; });
    }
    bool waitClosed(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] { return closed; });
    }
};

// connect() that prints the reason when it fails, so test failures are self-explanatory.
std::shared_ptr<ssh::SshConnection> connectLogged(const ssh::ConnectOptions& options, ssh::SshInteraction& ui,
                                                  std::string* error, std::shared_ptr<ssh::SshConnection> via = {},
                                                  ssh::Aborter* aborter = nullptr) {
    auto connection = ssh::SshConnection::connect(options, ui, error, std::move(via), aborter);
    if (!connection) {
        std::fprintf(stderr, "    connect failed: %s\n", error->c_str());
    }
    return connection;
}

std::string randomBytes(std::size_t size) {
    std::mt19937 engine(42);
    std::string data(size, '\0');
    for (char& c : data) {
        c = static_cast<char>(engine() & 0xFF);
    }
    return data;
}

} // namespace

WT_TEST(ssh_private_key_encryption_detection) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    WT_CHECK(!ssh::isPrivateKeyEncrypted(*platform::readFile(sshd->userKey())));
    WT_CHECK(ssh::isPrivateKeyEncrypted(*platform::readFile(sshd->encryptedUserKey())));
    WT_CHECK(ssh::isPrivateKeyEncrypted("-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\n"));
    WT_CHECK(!ssh::isPrivateKeyEncrypted("garbage"));
    WT_CHECK_EQ(ssh::expandHome("~/x"), platform::homeDirectory() + "/x");
    WT_CHECK_EQ(ssh::remoteParent("/a/b/"), std::string("/a"));
    WT_CHECK_EQ(ssh::remoteParent("/a"), std::string("/"));
    WT_CHECK_EQ(ssh::joinRemotePath("/a/", "b"), std::string("/a/b"));
}

WT_TEST(ssh_unknown_host_key_is_confirmed_and_saved) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    std::filesystem::remove(sshd->knownHosts());
    ScriptedUi ui;
    ui.decision = ssh::HostKeyDecision::Reject;
    std::string error;
    auto connection = connectLogged(baseOptions(*sshd), ui, &error);
    WT_CHECK(!connection);
    WT_CHECK(error.find("not accepted") != std::string::npos);
    WT_CHECK_EQ(ui.hostKeyPrompts, 1);
    WT_CHECK(ui.lastCheck == ssh::HostKeyCheck::NotFound);
    WT_CHECK_EQ(ui.lastKey.type, std::string("ssh-ed25519"));
    WT_CHECK(ui.lastKey.fingerprint.rfind("SHA256:", 0) == 0);

    // The fingerprint matches what ssh-keygen reports.
    std::string expected;
    if (FILE* pipe = ::popen(("ssh-keygen -lf " + sshd->hostPublicKey()).c_str(), "r")) {
        char line[512];
        if (std::fgets(line, sizeof(line), pipe)) {
            expected = line;
        }
        ::pclose(pipe);
    }
    WT_CHECK(expected.find(ui.lastKey.fingerprint) != std::string::npos);

    ui.decision = ssh::HostKeyDecision::AcceptAndSave;
    connection = connectLogged(baseOptions(*sshd), ui, &error);
    WT_REQUIRE(connection != nullptr);
    WT_CHECK_EQ(connection->authMethodUsed(), std::string("publickey"));
    const auto saved = platform::readFile(sshd->knownHosts());
    WT_REQUIRE(saved.has_value());
    WT_CHECK(saved->find("[127.0.0.1]:" + std::to_string(sshd->port()) + " ssh-ed25519 ") == 0);

    // Known now: no prompt.
    connection.reset();
    ScriptedUi quiet;
    connection = connectLogged(baseOptions(*sshd), quiet, &error);
    WT_REQUIRE(connection != nullptr);
    WT_CHECK_EQ(quiet.hostKeyPrompts, 0);
    // OpenSSH agrees with the file we wrote.
    const std::string sshCommand = "ssh -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=" +
                                   sshd->knownHosts() + " -o IdentitiesOnly=yes -i " + sshd->userKey() + " -p " +
                                   std::to_string(sshd->port()) + " 127.0.0.1 true >/dev/null 2>&1";
    WT_CHECK_EQ(std::system(sshCommand.c_str()), 0);
}

WT_TEST(ssh_changed_host_key_is_reported_and_can_be_replaced) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    // A different key recorded for this host:port, plus an unrelated entry that must survive.
    const auto stranger = platform::readFile(sshd->strangerKey() + ".pub");
    WT_REQUIRE(stranger.has_value());
    const std::string strangerBlob = stranger->substr(12, stranger->find(' ', 12) - 12);
    const std::string unrelated = "other.example ssh-ed25519 " + strangerBlob;
    WT_REQUIRE(platform::writeFileAtomic(sshd->knownHosts(), "# comment\n[127.0.0.1]:" + std::to_string(sshd->port()) +
                                                                 " ssh-ed25519 " + strangerBlob + "\n" + unrelated + "\n")
                   .isOk());
    ScriptedUi ui;
    ui.decision = ssh::HostKeyDecision::Reject;
    std::string error;
    WT_CHECK(!connectLogged(baseOptions(*sshd), ui, &error));
    WT_CHECK(ui.lastCheck == ssh::HostKeyCheck::Mismatch);
    WT_CHECK(error.find("has changed") != std::string::npos);

    ui.decision = ssh::HostKeyDecision::AcceptAndSave;
    WT_REQUIRE(connectLogged(baseOptions(*sshd), ui, &error) != nullptr);
    const std::string file = *platform::readFile(sshd->knownHosts());
    WT_CHECK(file.find("# comment") != std::string::npos);
    WT_CHECK(file.find(unrelated) != std::string::npos);
    WT_CHECK(file.find("]:" + std::to_string(sshd->port()) + " ssh-ed25519 " + strangerBlob) == std::string::npos);
    ScriptedUi quiet;
    WT_CHECK(connectLogged(baseOptions(*sshd), quiet, &error) != nullptr);
    WT_CHECK_EQ(quiet.hostKeyPrompts, 0);
}

WT_TEST(ssh_encrypted_key_asks_for_the_passphrase) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    auto options = baseOptions(*sshd);
    options.identityFile = sshd->encryptedUserKey();
    options.knownHostsPath.clear(); // always ask; accept once
    ScriptedUi ui;
    ui.decision = ssh::HostKeyDecision::AcceptOnce;
    ui.passphrases = {"wrong", test::SshdFixture::kPassphrase};
    std::string error;
    auto connection = connectLogged(options, ui, &error);
    WT_REQUIRE(connection != nullptr);
    WT_CHECK_EQ(ui.passphrasePrompts, 2);
    WT_CHECK(connection->usedPassphrase() == std::optional<std::string>(test::SshdFixture::kPassphrase));

    // A stored passphrase is used without asking; cancelling the prompt aborts.
    options.passphrase = test::SshdFixture::kPassphrase;
    ScriptedUi stored;
    stored.decision = ssh::HostKeyDecision::AcceptOnce;
    WT_CHECK(connectLogged(options, stored, &error) != nullptr);
    WT_CHECK_EQ(stored.passphrasePrompts, 0);
    options.passphrase.reset();
    ScriptedUi cancel;
    cancel.decision = ssh::HostKeyDecision::AcceptOnce;
    WT_CHECK(!connectLogged(options, cancel, &error));
    WT_CHECK(error.find("cancelled") != std::string::npos);
}

WT_TEST(ssh_agent_and_password_authentication) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    auto options = baseOptions(*sshd);
    std::string error;
    if (sshd->startAgent()) {
        options.auth = ssh::AuthMethod::Agent;
        options.identityFile.clear();
        ScriptedUi ui;
        auto connection = connectLogged(options, ui, &error);
        WT_CHECK(connection != nullptr);
        if (connection) {
            WT_CHECK_EQ(connection->authMethodUsed(), std::string("agent"));
        }
        sshd->stopAgent();
    } else {
        std::fprintf(stderr, "    SKIP agent: ssh-agent unavailable\n");
    }

    // The unprivileged server cannot verify passwords: every attempt fails, then we give up.
    options.auth = ssh::AuthMethod::Password;
    ScriptedUi ui;
    ui.passwords = {"a", "b", "c", "d"};
    WT_CHECK(!connectLogged(options, ui, &error));
    WT_CHECK_EQ(ui.passwordPrompts, 3);
    WT_CHECK(error.find("Authentication failed") != std::string::npos);

    // A key the server does not know is rejected without prompting.
    options.auth = ssh::AuthMethod::PublicKey;
    options.identityFile = sshd->strangerKey();
    ScriptedUi stranger;
    WT_CHECK(!connectLogged(options, stranger, &error));
    WT_CHECK_EQ(stranger.passphrasePrompts, 0);
}

WT_TEST(ssh_certificate_authentication) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    if (!sshd->certificatesAvailable()) {
        std::fprintf(stderr, "    SKIP: ssh-keygen cannot sign certificates\n");
        return;
    }
    auto options = baseOptions(*sshd);
    options.identityFile = sshd->certifiedKey(); // only its certificate is trusted
    std::string error;
    ScriptedUi ui;
    auto connection = connectLogged(options, ui, &error); // finds certified_key-cert.pub next to it
    WT_REQUIRE(connection != nullptr);
    WT_CHECK_EQ(connection->authMethodUsed(), std::string("publickey (certificate)"));
    connection.reset();

    // An explicit certificate path works as well...
    const std::string moved = sshd->dir() + "/elsewhere-cert.pub";
    std::filesystem::rename(sshd->certifiedKey() + "-cert.pub", moved);
    options.certificateFile = moved;
    ScriptedUi explicitUi;
    WT_CHECK(connectLogged(options, explicitUi, &error) != nullptr);
    // ... and without the certificate the bare key is refused.
    options.certificateFile.clear();
    ScriptedUi bareUi;
    WT_CHECK(!ssh::SshConnection::connect(options, bareUi, &error));
    WT_CHECK(error.find("Authentication failed") != std::string::npos);
    std::filesystem::rename(moved, sshd->certifiedKey() + "-cert.pub");
}

WT_TEST(ssh_exec_and_interactive_shell) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    ScriptedUi ui;
    std::string error;
    auto connection = connectLogged(baseOptions(*sshd), ui, &error);
    WT_REQUIRE(connection != nullptr);
    std::string output;
    int exitStatus = -1;
    WT_REQUIRE(connection->execute("echo exec-ok; exit 3", output, exitStatus).isOk());
    WT_CHECK_EQ(output, std::string("exec-ok\n"));
    WT_CHECK_EQ(exitStatus, 3);

    ssh::ShellOptions shellOptions;
    shellOptions.rows = 30;
    shellOptions.columns = 100;
    shellOptions.command = "/bin/sh";
    shellOptions.environment = {{"WT_GREETING", "hello-env"}};
    ShellSink sink;
    auto shell = ssh::SshShell::start(connection, shellOptions, sink.callbacks(), &error);
    WT_REQUIRE(shell != nullptr);
    shell->write("stty size; echo \"$TERM $WT_GREETING\"\n");
    WT_CHECK(sink.waitFor("30 100"));
    WT_CHECK(sink.waitFor("xterm-256color hello-env"));
    shell->resize(40, 120, 0, 0);
    shell->write("sleep 0.2; stty size\n");
    WT_CHECK(sink.waitFor("40 120"));
    shell->write("exit 7\n");
    WT_REQUIRE(sink.waitClosed());
    WT_CHECK_EQ(sink.exitStatus, 7);
    WT_CHECK(sink.error.empty());
}

WT_TEST(ssh_sftp_directory_trees) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    namespace fs = std::filesystem;
    const fs::path local = fs::path(sshd->dir()) / "tree-local";
    fs::create_directories(local / "sub" / "deeper");
    fs::create_directories(local / "empty");
    WT_REQUIRE(platform::writeFileAtomic((local / "a.txt").string(), "alpha").isOk());
    WT_REQUIRE(platform::writeFileAtomic((local / "sub" / "b.bin").string(), randomBytes(300000)).isOk());
    WT_REQUIRE(platform::writeFileAtomic((local / "sub" / "deeper" / "c.txt").string(), "gamma").isOk());
    fs::create_directory_symlink(local, local / "sub" / "loop"); // must not recurse forever

    ScriptedUi ui;
    std::string error;
    auto connection = connectLogged(baseOptions(*sshd), ui, &error);
    WT_REQUIRE(connection != nullptr);
    auto sftp = connection->openSftp(&error);
    WT_REQUIRE(sftp != nullptr);
    const std::string remote = sshd->dir() + "/tree-remote"; // the server shares our file system
    std::uint64_t lastDone = 0;
    std::uint64_t lastTotal = 0;
    WT_REQUIRE(sftp->uploadTree(local.string(), remote, [&](std::uint64_t done, std::uint64_t total) {
                       lastDone = done;
                       lastTotal = total;
                       return true;
                   }).isOk());
    WT_CHECK_EQ(lastTotal, std::uint64_t(300010));
    WT_CHECK_EQ(lastDone, lastTotal);
    WT_CHECK(*platform::readFile(remote + "/sub/deeper/c.txt") == "gamma");
    WT_CHECK(platform::readFile(remote + "/sub/b.bin")->size() == 300000u);
    WT_CHECK(fs::is_directory(remote + "/empty"));
    WT_CHECK(!fs::exists(remote + "/sub/loop"));
    // Uploading again reuses the directories and overwrites the files.
    WT_REQUIRE(platform::writeFileAtomic((local / "a.txt").string(), "alpha-2").isOk());
    WT_REQUIRE(sftp->uploadTree(local.string(), remote).isOk());
    WT_CHECK(*platform::readFile(remote + "/a.txt") == "alpha-2");

    const fs::path back = fs::path(sshd->dir()) / "tree-back";
    WT_REQUIRE(sftp->downloadTree(remote, back.string()).isOk());
    WT_CHECK(*platform::readFile((back / "a.txt").string()) == "alpha-2");
    WT_CHECK(*platform::readFile((back / "sub" / "b.bin").string()) == *platform::readFile((local / "sub" / "b.bin").string()));
    WT_CHECK(fs::is_directory(back / "empty"));
    // Cancelling stops the transfer with an error.
    const auto cancelled = sftp->downloadTree(remote, (fs::path(sshd->dir()) / "tree-cancel").string(),
                                              [](std::uint64_t, std::uint64_t) { return false; });
    WT_CHECK(!cancelled.isOk());
    WT_CHECK(cancelled.message().find("cancelled") != std::string::npos);
    WT_CHECK(!sftp->uploadTree((local / "a.txt").string(), remote + "/x").isOk()); // not a directory
}

WT_TEST(ssh_sftp_file_operations) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    ScriptedUi ui;
    std::string error;
    auto connection = connectLogged(baseOptions(*sshd), ui, &error);
    WT_REQUIRE(connection != nullptr);
    auto sftp = connection->openSftp(&error);
    WT_REQUIRE(sftp != nullptr);

    const std::string remoteDir = sshd->dir() + "/remote";
    WT_REQUIRE(sftp->makeDirectory(remoteDir).isOk());
    WT_CHECK(!sftp->makeDirectory(remoteDir).isOk()); // exists

    const std::string payload = randomBytes(3 * 1024 * 1024 + 17);
    const std::string localFile = sshd->dir() + "/local.bin";
    WT_REQUIRE(platform::writeFileAtomic(localFile, payload).isOk());
    int progressCalls = 0;
    std::uint64_t lastDone = 0;
    WT_REQUIRE(sftp->upload(localFile, remoteDir + "/data.bin",
                            [&](std::uint64_t done, std::uint64_t total) {
                                ++progressCalls;
                                lastDone = done;
                                return total == payload.size();
                            })
                   .isOk());
    WT_CHECK(progressCalls >= 2);
    WT_CHECK_EQ(lastDone, static_cast<std::uint64_t>(payload.size()));

    std::vector<ssh::SftpEntry> entries;
    WT_REQUIRE(sftp->makeDirectory(remoteDir + "/sub").isOk());
    WT_REQUIRE(sftp->list(remoteDir, entries).isOk());
    WT_REQUIRE(entries.size() == 2u);
    WT_CHECK(entries[0].isDirectory && entries[0].name == "sub"); // directories first
    WT_CHECK_EQ(entries[1].name, std::string("data.bin"));
    WT_CHECK_EQ(entries[1].size, static_cast<std::uint64_t>(payload.size()));

    const std::string downloaded = sshd->dir() + "/downloaded.bin";
    WT_REQUIRE(sftp->download(remoteDir + "/data.bin", downloaded).isOk());
    WT_CHECK(platform::readFile(downloaded) == std::optional<std::string>(payload));

    // Cancelling a download leaves no partial file behind.
    const std::string cancelled = sshd->dir() + "/cancelled.bin";
    const auto status = sftp->download(remoteDir + "/data.bin", cancelled, [](std::uint64_t, std::uint64_t) { return false; });
    WT_CHECK(!status.isOk());
    WT_CHECK(!std::filesystem::exists(cancelled));

    WT_CHECK(sftp->rename(remoteDir + "/data.bin", remoteDir + "/renamed.bin").isOk());
    ssh::SftpEntry info;
    WT_CHECK(sftp->stat(remoteDir + "/renamed.bin", info).isOk());
    WT_CHECK(!sftp->stat(remoteDir + "/data.bin", info).isOk());
    std::string resolved;
    WT_CHECK(sftp->realPath(remoteDir + "/sub/..", resolved).isOk());
    WT_CHECK_EQ(resolved, std::filesystem::canonical(remoteDir).string());
    WT_CHECK(!sftp->removeDirectory(remoteDir).isOk()); // not empty
    WT_CHECK(sftp->removeFile(remoteDir + "/renamed.bin").isOk());
    WT_CHECK(sftp->removeDirectory(remoteDir + "/sub").isOk());
    WT_CHECK(sftp->removeDirectory(remoteDir).isOk());
    const auto missing = sftp->list(remoteDir, entries);
    WT_CHECK(!missing.isOk());
    WT_CHECK(missing.message().find("no such file") != std::string::npos);
}

WT_TEST(ssh_jump_host_and_proxies) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    std::string error;
    ScriptedUi ui;
    // Jump: reach the same server through itself.
    auto jump = connectLogged(baseOptions(*sshd), ui, &error);
    WT_REQUIRE(jump != nullptr);
    auto options = baseOptions(*sshd);
    options.host = "localhost"; // a different name than the jump connection used
    ScriptedUi innerUi;
    innerUi.decision = ssh::HostKeyDecision::AcceptOnce;
    auto inner = connectLogged(options, innerUi, &error, jump);
    WT_REQUIRE(inner != nullptr);
    jump.reset(); // the inner connection keeps the jump alive
    std::string output;
    int exitStatus = -1;
    WT_REQUIRE(inner->execute("echo via-jump", output, exitStatus).isOk());
    WT_CHECK_EQ(output, std::string("via-jump\n"));
    inner.reset();

    // SOCKS5 and HTTP CONNECT proxies.
    for (auto kind : {test::ProxyServer::Kind::Socks5, test::ProxyServer::Kind::Http}) {
        test::ProxyServer proxy(kind, "u", "p");
        WT_REQUIRE(proxy.start());
        options = baseOptions(*sshd);
        options.proxy = {kind == test::ProxyServer::Kind::Socks5 ? net::ProxyType::Socks5 : net::ProxyType::HttpConnect,
                         "127.0.0.1", proxy.port(), "u", "p"};
        auto viaProxy = connectLogged(options, ui, &error);
        WT_REQUIRE(viaProxy != nullptr);
        output.clear();
        WT_REQUIRE(viaProxy->execute("echo via-proxy", output, exitStatus).isOk());
        WT_CHECK_EQ(output, std::string("via-proxy\n"));
        WT_CHECK_EQ(proxy.lastTarget(), "127.0.0.1:" + std::to_string(sshd->port()));
    }
}

WT_TEST(ssh_connection_loss_is_reported) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    test::ProxyServer proxy(test::ProxyServer::Kind::Socks5);
    WT_REQUIRE(proxy.start());
    auto options = baseOptions(*sshd);
    options.proxy = {net::ProxyType::Socks5, "127.0.0.1", proxy.port(), {}, {}};
    options.keepAliveSeconds = 1;
    ScriptedUi ui;
    std::string error;
    auto connection = connectLogged(options, ui, &error);
    WT_REQUIRE(connection != nullptr);
    ShellSink sink;
    ssh::ShellOptions shellOptions;
    shellOptions.command = "/bin/sh";
    auto shell = ssh::SshShell::start(connection, shellOptions, sink.callbacks(), &error);
    WT_REQUIRE(shell != nullptr);
    shell->write("echo alive\n");
    WT_CHECK(sink.waitFor("alive"));
    proxy.dropAll(); // the network goes away
    WT_REQUIRE(sink.waitClosed(10s));
    WT_CHECK_EQ(sink.exitStatus, -1);
    WT_CHECK(sink.error.find("Connection lost") != std::string::npos);

    // Aborting a connection attempt from another thread returns promptly.
    ssh::Aborter aborter;
    aborter.abort();
    ScriptedUi again;
    WT_CHECK(!connectLogged(baseOptions(*sshd), again, &error, nullptr, &aborter));
    WT_CHECK(error.find("cancelled") != std::string::npos);
}

WT_TEST(ssh_aborter_never_touches_reused_sockets) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    ssh::Aborter aborter;
    ScriptedUi ui;
    std::string error;
    auto connection = connectLogged(baseOptions(*sshd), ui, &error, nullptr, &aborter);
    WT_REQUIRE(connection != nullptr);
    connection.reset(); // closes the socket; the next one will likely get the same number
    net::Socket a;
    net::Socket b;
    WT_REQUIRE(net::socketPair(a, b).isOk());
    aborter.abort();
    WT_CHECK(a.sendAll("still-open").isOk());
    char buffer[10];
    WT_CHECK(b.receiveExact(buffer, sizeof(buffer), 1000ms).isOk());
}

WT_TEST(ssh_known_hosts_updates_are_safe) {
    auto* sshd = server();
    if (!sshd) {
        return;
    }
    // A symlinked known_hosts stays a symlink and keeps its other lines.
    const std::string real = sshd->dir() + "/real_known_hosts";
    const std::string link = sshd->dir() + "/linked_known_hosts";
    WT_REQUIRE(platform::writeFileAtomic(real, "# keep me\nother.example ssh-ed25519 AAAA").isOk()); // no final newline
    std::filesystem::remove(link);
    std::filesystem::create_symlink(real, link);
    auto options = baseOptions(*sshd);
    options.knownHostsPath = link;
    ScriptedUi ui;
    std::string error;
    WT_REQUIRE(connectLogged(options, ui, &error) != nullptr);
    WT_CHECK(std::filesystem::is_symlink(link));
    const std::string contents = *platform::readFile(real);
    WT_CHECK(contents.rfind("# keep me\nother.example ssh-ed25519 AAAA\n[127.0.0.1]:", 0) == 0);

    // An unreadable file is never replaced.
    const std::string locked = sshd->dir() + "/locked_known_hosts";
    WT_REQUIRE(platform::writeFileAtomic(locked, "precious\n").isOk());
    std::filesystem::permissions(locked, std::filesystem::perms::none);
    if (!platform::readFile(locked)) { // (root can read anything; skip then)
        options.knownHostsPath = locked;
        ScriptedUi again;
        auto connection = connectLogged(options, again, &error); // accepted once, saving fails
        WT_CHECK(connection != nullptr);
        bool warned = false;
        for (const auto& info : again.infos) {
            warned = warned || info.find("could not save") != std::string::npos;
        }
        WT_CHECK(warned);
        std::filesystem::permissions(locked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        WT_CHECK(platform::readFile(locked) == std::optional<std::string>("precious\n"));
    }
}
