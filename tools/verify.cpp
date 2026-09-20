// Verifies the durability contract after a SIGKILL:
//   every index the writer printed (i.e. every put() that returned) must be
//   present, with exactly the value it wrote.
//
//   usage: verify <log-path> <last-acked-index> [value-size]
//
// Exits 0 if the contract held, 1 if any acknowledged write is missing or
// wrong. Records *beyond* the last acknowledged index are permitted to exist
// (the process may have been killed after the write landed but before the
// acknowledgement was printed) — that is not a violation, only the reverse is.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "logkv/store.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: verify <log-path> <last-acked-index> [value-size]\n");
        return 2;
    }
    const std::string path = argv[1];
    const long long last = std::strtoll(argv[2], nullptr, 10);
    const std::size_t vsize = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 200;
    const std::string base(vsize, 'x');

    logkv::Store s(path, {.sync = logkv::Store::Sync::None});

    long long missing = 0, wrong = 0;
    for (long long i = 0; i <= last; ++i) {
        const auto v = s.get("k:" + std::to_string(i));
        if (!v) {
            ++missing;
            if (missing <= 5) std::fprintf(stderr, "  MISSING k:%lld\n", i);
        } else if (*v != base + ":" + std::to_string(i)) {
            ++wrong;
            if (wrong <= 5) std::fprintf(stderr, "  WRONG   k:%lld\n", i);
        }
    }

    std::printf("acked=%lld present=%lld missing=%lld wrong=%lld recovered=%llu truncated=%llu\n",
                last + 1, last + 1 - missing - wrong, missing, wrong,
                static_cast<unsigned long long>(s.recovered_records()),
                static_cast<unsigned long long>(s.truncated_bytes()));

    return (missing == 0 && wrong == 0) ? 0 : 1;
}
