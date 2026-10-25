// Headless throughput benchmark for the VT parser + terminal model (no rendering).
//
//   wt_terminal_bench [megabytes]
//
// Feeds three workloads (plain ASCII lines, SGR-heavy coloured output, CJK + combining text)
// through wt::terminal::Terminal in 64 KiB chunks and prints MB/s for each.

#include "wt/terminal/Terminal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string makeWorkload(int kind, std::size_t bytes) {
    std::string out;
    out.reserve(bytes + 256);
    unsigned seed = 12345;
    auto next = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) & 0x7FFF;
    };
    while (out.size() < bytes) {
        switch (kind) {
        case 0:
            for (int i = 0; i < 100; ++i) {
                out.push_back(static_cast<char>('!' + next() % 90));
            }
            break;
        case 1:
            for (int i = 0; i < 20; ++i) {
                out += "\x1b[38;5;" + std::to_string(next() % 256) + ";48;2;" + std::to_string(next() % 256) + ";10;20m";
                out += "word";
            }
            out += "\x1b[0m";
            break;
        default:
            for (int i = 0; i < 20; ++i) {
                out += "中文字e\xCC\x81 ";
            }
            break;
        }
        out += "\r\n";
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t megabytes = argc > 1 ? static_cast<std::size_t>(std::atoi(argv[1])) : 64;
    const char* names[] = {"ascii", "sgr", "cjk+combining"};
    for (int kind = 0; kind < 3; ++kind) {
        const std::string data = makeWorkload(kind, megabytes * 1024 * 1024);
        wt::terminal::Terminal terminal(50, 200, 10000);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t offset = 0; offset < data.size(); offset += 64 * 1024) {
            terminal.feed(std::string_view(data).substr(offset, 64 * 1024));
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%-14s %8.1f MB/s  (%zu MB in %.2f s, %zu history lines)\n", names[kind],
                    static_cast<double>(data.size()) / (1024.0 * 1024.0) / seconds, megabytes, seconds,
                    terminal.scrollbackSize());
    }
    return 0;
}
