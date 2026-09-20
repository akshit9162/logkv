#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "logkv/store.hpp"

namespace fs = std::filesystem;
using logkv::Store;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(cond)) {                                                             \
            ++g_failures;                                                          \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " << #cond \
                      << "\n";                                                     \
        }                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        ++g_checks;                                                                 \
        auto _a = (a);                                                              \
        auto _b = (b);                                                              \
        if (!(_a == _b)) {                                                          \
            ++g_failures;                                                           \
            std::cerr << "  FAIL " << __FILE__ << ":" << __LINE__ << "  " << #a     \
                      << " == " << #b << "  (" << _a << " vs " << _b << ")\n";      \
        }                                                                           \
    } while (0)

struct TempDir {
    fs::path dir;
    TempDir() {
        dir = fs::temp_directory_path() /
              ("logkv_test_" + std::to_string(::getpid()) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string log() const { return (dir / "store.log").string(); }
};

void run(const char* name, void (*fn)()) {
    std::cout << "- " << name << "\n";
    const int before = g_failures;
    fn();
    if (g_failures == before) std::cout << "  ok\n";
}

// ---------------------------------------------------------------------------

void test_roundtrip() {
    TempDir td;
    Store s(td.log(), {.sync = Store::Sync::None});
    s.put("alpha", "one");
    s.put("beta", "two");

    CHECK(s.get("alpha").value() == "one");
    CHECK(s.get("beta").value() == "two");
    CHECK(!s.get("missing").has_value());
    CHECK_EQ(s.size(), std::size_t{2});
}

void test_overwrite_wins() {
    TempDir td;
    Store s(td.log(), {.sync = Store::Sync::None});
    s.put("k", "v1");
    s.put("k", "v2");
    s.put("k", "v3");

    CHECK(s.get("k").value() == "v3");
    CHECK_EQ(s.size(), std::size_t{1});
}

void test_erase() {
    TempDir td;
    Store s(td.log(), {.sync = Store::Sync::None});
    s.put("k", "v");
    CHECK(s.erase("k"));
    CHECK(!s.get("k").has_value());
    CHECK(!s.erase("k"));  // already gone
}

void test_empty_value() {
    TempDir td;
    Store s(td.log(), {.sync = Store::Sync::None});
    s.put("k", "");
    CHECK(s.get("k").has_value());
    CHECK(s.get("k").value().empty());
}

void test_reopen_rebuilds_index() {
    TempDir td;
    {
        Store s(td.log(), {.sync = Store::Sync::Always});
        s.put("a", "1");
        s.put("b", "2");
        s.put("a", "3");   // overwrite
        s.erase("b");      // tombstone
        s.put("c", "4");
    }
    Store s2(td.log(), {.sync = Store::Sync::None});
    CHECK(s2.get("a").value() == "3");
    CHECK(!s2.get("b").has_value());  // tombstone survived the reopen
    CHECK(s2.get("c").value() == "4");
    CHECK_EQ(s2.size(), std::size_t{2});
    CHECK_EQ(s2.recovered_records(), std::uint64_t{5});
    CHECK_EQ(s2.truncated_bytes(), std::uint64_t{0});
}

void test_large_values() {
    TempDir td;
    const std::string big(1 << 16, 'x');
    {
        Store s(td.log(), {.sync = Store::Sync::None});
        for (int i = 0; i < 20; ++i) s.put("key" + std::to_string(i), big);
    }
    Store s2(td.log(), {.sync = Store::Sync::None});
    CHECK_EQ(s2.size(), std::size_t{20});
    CHECK(s2.get("key7").value() == big);
}

// Simulate a crash mid-append by chopping bytes off the end of the log, then
// reopening. Earlier records must survive; the torn tail must be discarded.
void test_torn_tail_is_discarded() {
    TempDir td;
    {
        Store s(td.log(), {.sync = Store::Sync::Always});
        s.put("keep1", "v1");
        s.put("keep2", "v2");
        s.put("torn", "this record will be cut in half");
    }
    const auto full = fs::file_size(td.log());

    // Cut 10 bytes off: the final record is now incomplete.
    int fd = ::open(td.log().c_str(), O_RDWR);
    CHECK(fd >= 0);
    CHECK_EQ(::ftruncate(fd, static_cast<off_t>(full - 10)), 0);
    ::close(fd);

    Store s2(td.log(), {.sync = Store::Sync::None});
    CHECK(s2.get("keep1").value() == "v1");
    CHECK(s2.get("keep2").value() == "v2");
    CHECK(!s2.get("torn").has_value());
    CHECK(s2.truncated_bytes() > 0);
    CHECK_EQ(s2.log_bytes(), fs::file_size(td.log()));  // file was truncated to last good offset
}

// A single flipped bit inside a committed record must be caught, not returned.
void test_bitflip_is_detected() {
    TempDir td;
    {
        Store s(td.log(), {.sync = Store::Sync::Always});
        s.put("k", "the quick brown fox");
    }

    int fd = ::open(td.log().c_str(), O_RDWR);
    CHECK(fd >= 0);
    unsigned char byte = 0;
    const off_t victim = static_cast<off_t>(Store::kHeaderSize + 1 + 3);  // inside the value
    CHECK_EQ(::pread(fd, &byte, 1, victim), ssize_t{1});
    byte ^= 0x20;
    CHECK_EQ(::pwrite(fd, &byte, 1, victim), ssize_t{1});
    ::close(fd);

    Store s2(td.log(), {.sync = Store::Sync::None});
    // Recovery rejects the record outright, so the key is simply absent.
    CHECK(!s2.get("k").has_value());
    CHECK(s2.truncated_bytes() > 0);
}

void test_compaction() {
    TempDir td;
    Store s(td.log(), {.sync = Store::Sync::None});
    for (int i = 0; i < 500; ++i) s.put("hot", "value-" + std::to_string(i));
    s.put("cold", "kept");
    s.put("dead", "to be removed");
    s.erase("dead");

    const auto before = s.log_bytes();
    s.compact();
    const auto after = s.log_bytes();

    CHECK(after < before);
    CHECK(s.get("hot").value() == "value-499");
    CHECK(s.get("cold").value() == "kept");
    CHECK(!s.get("dead").has_value());
    CHECK_EQ(s.size(), std::size_t{2});

    // And it must still be correct after a reopen.
    s.sync();
    Store s2(td.log(), {.sync = Store::Sync::None});
    CHECK(s2.get("hot").value() == "value-499");
    CHECK_EQ(s2.size(), std::size_t{2});
}

void test_move_semantics() {
    TempDir td;
    Store a(td.log(), {.sync = Store::Sync::None});
    a.put("k", "v");

    Store b = std::move(a);
    CHECK(b.get("k").value() == "v");

    TempDir td2;
    Store c(td2.log(), {.sync = Store::Sync::None});
    c = std::move(b);
    CHECK(c.get("k").value() == "v");
}

}  // namespace

int main() {
    std::cout << "logkv tests\n";
    run("roundtrip", test_roundtrip);
    run("overwrite wins", test_overwrite_wins);
    run("erase", test_erase);
    run("empty value", test_empty_value);
    run("reopen rebuilds index", test_reopen_rebuilds_index);
    run("large values", test_large_values);
    run("torn tail discarded", test_torn_tail_is_discarded);
    run("bitflip detected", test_bitflip_is_detected);
    run("compaction", test_compaction);
    run("move semantics", test_move_semantics);

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
