// Writes acknowledged records forever and reports each acknowledgement on
// stdout. A harness kills this process with SIGKILL mid-run; whatever was
// printed before the kill is what the store promised, and recovery must
// deliver exactly that.
//
//   usage: crashwriter <log-path> [value-size]
//
// stdout is line-buffered and flushed after every put, so the last printed
// index is the last write the store acknowledged to the caller.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "logkv/store.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: crashwriter <log-path> [value-size]\n");
        return 2;
    }
    const std::string path = argv[1];
    const std::size_t vsize = (argc > 2) ? std::strtoul(argv[2], nullptr, 10) : 200;

    // Always-sync: every put() that returns is a durable, acknowledged write.
    // That is the contract the crash test verifies.
    logkv::Store s(path, {.sync = logkv::Store::Sync::Always});

    const std::string value(vsize, 'x');
    for (std::uint64_t i = 0;; ++i) {
        s.put("k:" + std::to_string(i), value + ":" + std::to_string(i));
        std::printf("%llu\n", static_cast<unsigned long long>(i));
        std::fflush(stdout);
    }
}
