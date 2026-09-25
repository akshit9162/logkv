// Measures append throughput under each durability mode, plus point-read
// throughput. The whole point of the benchmark is the *ratio* between modes:
// it makes the cost of durability a number instead of an opinion.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "logkv/store.hpp"

namespace fs = std::filesystem;
using logkv::Store;
using clk = std::chrono::steady_clock;

namespace {

struct Result {
    double seconds;
    double ops_per_sec;
    double us_per_op;
};

Result time_it(std::size_t ops, const std::function<void()>& body) {
    const auto t0 = clk::now();
    body();
    const auto t1 = clk::now();
    const double s = std::chrono::duration<double>(t1 - t0).count();
    return {s, static_cast<double>(ops) / s, (s * 1e6) / static_cast<double>(ops)};
}

const char* mode_name(Store::Sync m) {
    switch (m) {
        case Store::Sync::None: return "none      ";
        case Store::Sync::Batched: return "batched   ";
        case Store::Sync::Always: return "always    ";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = (argc > 1) ? std::strtoul(argv[1], nullptr, 10) : 20000;
    const std::size_t vsize = (argc > 2) ? std::strtoul(argv[2], nullptr, 10) : 128;
    const std::size_t batch_n = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 128;

    const fs::path dir = fs::temp_directory_path() / "logkv_bench";
    fs::create_directories(dir);

    std::vector<std::string> keys(n);
    for (std::size_t i = 0; i < n; ++i) keys[i] = "key:" + std::to_string(i);
    const std::string value(vsize, 'v');

    std::printf("logkv benchmark\n");
    std::printf("  records      : %zu\n", n);
    std::printf("  value size   : %zu bytes\n", vsize);
    std::printf("  batch_n      : %zu (batched mode)\n\n", batch_n);
    std::printf("  %-10s %12s %12s %14s\n", "mode", "ops/sec", "us/op", "log bytes");
    std::printf("  %-10s %12s %12s %14s\n", "----------", "------------", "------------",
                "--------------");

    for (auto mode : {Store::Sync::None, Store::Sync::Batched, Store::Sync::Always}) {
        const std::string path = (dir / ("w_" + std::to_string(static_cast<int>(mode)) + ".log")).string();
        fs::remove(path);

        Store s(path, {.sync = mode, .batch_n = batch_n});
        const auto r = time_it(n, [&] {
            for (std::size_t i = 0; i < n; ++i) s.put(keys[i], value);
            s.sync();  // all modes end durable, so the comparison is honest
        });
        std::printf("  %-10s %12.0f %12.2f %14llu\n", mode_name(mode), r.ops_per_sec, r.us_per_op,
                    static_cast<unsigned long long>(s.log_bytes()));
    }

    // Reads: random point lookups against a warm index, cold page cache is not
    // controlled for — this is a relative number, not an absolute one.
    {
        const std::string path = (dir / "r.log").string();
        fs::remove(path);
        Store s(path, {.sync = Store::Sync::None});
        for (std::size_t i = 0; i < n; ++i) s.put(keys[i], value);
        s.sync();

        std::mt19937_64 rng(42);
        std::uniform_int_distribution<std::size_t> pick(0, n - 1);
        std::size_t found = 0;
        const auto r = time_it(n, [&] {
            for (std::size_t i = 0; i < n; ++i) {
                if (s.get(keys[pick(rng)])) ++found;
            }
        });
        std::printf("\n  %-10s %12.0f %12.2f   (%zu hits)\n", "get       ", r.ops_per_sec,
                    r.us_per_op, found);
    }

    // Recovery: how long to rebuild the index from a log of n records.
    {
        const std::string path = (dir / "rec.log").string();
        fs::remove(path);
        {
            Store s(path, {.sync = Store::Sync::None});
            for (std::size_t i = 0; i < n; ++i) s.put(keys[i], value);
            s.sync();
        }
        const auto t0 = clk::now();
        Store s2(path, {.sync = Store::Sync::None});
        const double secs = std::chrono::duration<double>(clk::now() - t0).count();
        std::printf("  %-10s %12.0f %12.2f   (%llu records replayed)\n", "recover   ",
                    static_cast<double>(n) / secs, (secs * 1e6) / static_cast<double>(n),
                    static_cast<unsigned long long>(s2.recovered_records()));
    }

    std::printf("\n  note: 'always' uses F_FULLFSYNC on macOS — a true media flush,\n");
    std::printf("        which is why the gap to 'none' is large.\n");
    return 0;
}
