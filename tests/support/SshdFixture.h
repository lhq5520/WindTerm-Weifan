#pragma once

// Runs an unprivileged OpenSSH server on 127.0.0.1 for integration tests (POSIX only). It accepts
// the generated user keys for the current user; password logins cannot succeed without root.

#include "wt/net/Socket.h"
#include "wt/platform/Files.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>

#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace wt::test {

class SshdFixture {
public:
    static constexpr const char* kPassphrase = "correct horse";

    ~SshdFixture() { stop(); }

    // Starts the server; false (with a reason) when sshd or ssh-keygen are unavailable.
    bool start(std::string* reason) {
        const char* candidates[] = {"/usr/sbin/sshd", "/usr/local/sbin/sshd", "/opt/homebrew/sbin/sshd",
                                    "/home/linuxbrew/.linuxbrew/sbin/sshd"};
        for (const char* path : candidates) {
            if (::access(path, X_OK) == 0) {
                m_sshd = path;
                break;
            }
        }
        if (m_sshd.empty()) {
            *reason = "sshd not found";
            return false;
        }
        m_dir = (std::filesystem::temp_directory_path() / ("wt-sshd-" + std::to_string(std::random_device{}()))).string();
        std::filesystem::create_directories(m_dir);
        ::chmod(m_dir.c_str(), 0700);
        if (!run("ssh-keygen -q -t ed25519 -N '' -f " + m_dir + "/host_key") ||
            !run("ssh-keygen -q -t ed25519 -N '' -C plain -f " + m_dir + "/user_key") ||
            !run("ssh-keygen -q -t ed25519 -N '" + std::string(kPassphrase) + "' -C encrypted -f " + m_dir +
                 "/user_key_encrypted") ||
            !run("ssh-keygen -q -t ed25519 -N '' -C stranger -f " + m_dir + "/stranger_key") ||
            !run("cat " + m_dir + "/user_key.pub " + m_dir + "/user_key_encrypted.pub > " + m_dir +
                 "/authorized_keys")) {
            *reason = "ssh-keygen failed";
            return false;
        }
        net::Socket probe;
        std::uint16_t port = 0;
        if (!net::listenLoopback(0, probe, port)) {
            *reason = "no free port";
            return false;
        }
        probe.close();
        m_port = port;
        const std::string config = "Port " + std::to_string(m_port) +
                                   "\nListenAddress 127.0.0.1\nHostKey " + m_dir + "/host_key\nPidFile " + m_dir +
                                   "/sshd.pid\nAuthorizedKeysFile " + m_dir +
                                   "/authorized_keys\nStrictModes no\nUsePAM no\nPasswordAuthentication yes\n"
                                   "KbdInteractiveAuthentication no\nAllowTcpForwarding yes\nAcceptEnv WT_*\n"
                                   "Subsystem sftp internal-sftp\nLogLevel ERROR\n";
        if (!platform::writeFileAtomic(m_dir + "/sshd_config", config)) {
            *reason = "cannot write sshd_config";
            return false;
        }
        m_pid = ::fork();
        if (m_pid == 0) {
            const std::string log = m_dir + "/sshd.log";
            if (!std::freopen(log.c_str(), "w", stderr)) {
                ::_exit(126);
            }
            const std::string configPath = m_dir + "/sshd_config";
            ::execl(m_sshd.c_str(), m_sshd.c_str(), "-D", "-e", "-f", configPath.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        for (int i = 0; i < 100; ++i) {
            net::Socket socket;
            if (net::connectTcp("127.0.0.1", m_port, std::chrono::milliseconds(200), socket)) {
                return true;
            }
            int status = 0;
            if (::waitpid(m_pid, &status, WNOHANG) == m_pid) {
                m_pid = -1;
                *reason = "sshd exited: " + platform::readFile(m_dir + "/sshd.log").value_or("");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        *reason = "sshd did not start listening";
        return false;
    }

    void stop() {
        stopAgent();
        if (m_pid > 0) {
            ::kill(m_pid, SIGTERM);
            ::waitpid(m_pid, nullptr, 0);
            m_pid = -1;
        }
        if (!m_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(m_dir, ec);
            m_dir.clear();
        }
    }

    // Starts ssh-agent holding the plain user key and points SSH_AUTH_SOCK at it.
    bool startAgent() {
        const std::string socket = m_dir + "/agent.sock";
        const std::string pidFile = m_dir + "/agent.pid";
        if (!run("ssh-agent -a " + socket + " | sed -n 's/^SSH_AGENT_PID=\\([0-9]*\\).*/\\1/p' > " + pidFile) ||
            !run("SSH_AUTH_SOCK=" + socket + " ssh-add -q " + m_dir + "/user_key")) {
            return false;
        }
        m_agentPid = std::atoi(platform::readFile(pidFile).value_or("0").c_str());
        ::setenv("SSH_AUTH_SOCK", socket.c_str(), 1);
        return m_agentPid > 0;
    }

    void stopAgent() {
        if (m_agentPid > 0) {
            ::kill(m_agentPid, SIGTERM);
            m_agentPid = 0;
        }
        ::unsetenv("SSH_AUTH_SOCK");
    }

    std::uint16_t port() const { return m_port; }
    const std::string& dir() const { return m_dir; }
    std::string userKey() const { return m_dir + "/user_key"; }
    std::string encryptedUserKey() const { return m_dir + "/user_key_encrypted"; }
    std::string strangerKey() const { return m_dir + "/stranger_key"; }
    std::string hostPublicKey() const { return m_dir + "/host_key.pub"; }
    std::string knownHosts() const { return m_dir + "/known_hosts"; }
    static std::string user() {
        const passwd* pw = ::getpwuid(::getuid());
        return pw ? pw->pw_name : "";
    }

private:
    static bool run(const std::string& command) { return std::system(("{ " + command + "; } >/dev/null 2>&1").c_str()) == 0; }

    std::string m_sshd;
    std::string m_dir;
    std::uint16_t m_port = 0;
    pid_t m_pid = -1;
    int m_agentPid = 0;
};

} // namespace wt::test
