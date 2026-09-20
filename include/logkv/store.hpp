#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace logkv {

// A crash-safe, append-only key-value store.
//
// Every mutation is appended to a single log file as a self-describing,
// CRC-protected record. The in-memory index maps key -> location of the most
// recent record for that key, and is rebuilt by replaying the log at open().
//
// Durability is explicit rather than incidental: see Sync. A write is
// "acknowledged" only after put() returns, and the guarantee attached to that
// acknowledgement is exactly what the chosen Sync mode provides.
//
// On-disk record layout (little-endian):
//
//   offset  size   field
//   0       4      crc32   over bytes [4, end) of this record
//   4       1      type    0 = PUT, 1 = TOMBSTONE
//   5       4      klen
//   9       4      vlen    (0 for a tombstone)
//   13      klen   key
//   13+klen vlen   value
//
class Store {
public:
    enum class Sync {
        None,     // never fsync; the OS decides. Fastest, loses data on power loss.
        Batched,  // fsync every batch_n appends (group commit).
        Always,   // fsync before every put() returns. Slowest, strongest.
    };

    struct Options {
        Sync sync = Sync::Batched;
        std::size_t batch_n = 128;
        // On macOS, fsync() only pushes to the drive's cache; F_FULLFSYNC asks
        // the drive to flush that cache to stable media. The difference is
        // large enough to change benchmark numbers by an order of magnitude,
        // so it is a deliberate option rather than a hidden default.
        bool full_fsync = true;
    };

    // Two overloads rather than a defaulted `Options opts = {}`: Apple clang 14
    // rejects an in-class default argument that needs Options' own default
    // member initializers while Store is still incomplete.
    explicit Store(const std::string& path);
    Store(const std::string& path, Options opts);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&& other) noexcept;
    Store& operator=(Store&& other) noexcept;

    void put(std::string_view key, std::string_view value);
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;
    bool erase(std::string_view key);

    // Force everything written so far to stable storage.
    void sync();

    // Rewrite the log keeping only live records, then atomically swap it in.
    // Reclaims space held by overwritten keys and tombstones.
    void compact();

    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] std::uint64_t log_bytes() const noexcept { return tail_; }

    // Recovery statistics, populated at open(). Useful in tests and worth
    // surfacing: truncated_bytes() > 0 means a torn tail was discarded.
    [[nodiscard]] std::uint64_t recovered_records() const noexcept { return recovered_; }
    [[nodiscard]] std::uint64_t truncated_bytes() const noexcept { return truncated_; }

    static constexpr std::size_t kHeaderSize = 13;
    static constexpr std::uint8_t kPut = 0;
    static constexpr std::uint8_t kTombstone = 1;

private:
    struct Loc {
        std::uint64_t off;  // start of the record
        std::uint32_t len;  // total record length including header
    };

    void recover();
    void append(std::uint8_t type, std::string_view key, std::string_view value);
    void maybe_sync();
    void do_fsync();
    void close_fd() noexcept;

    std::string path_;
    Options opts_{};
    int fd_ = -1;
    std::uint64_t tail_ = 0;        // next append offset == current log size
    std::size_t since_sync_ = 0;
    std::uint64_t recovered_ = 0;
    std::uint64_t truncated_ = 0;
    std::unordered_map<std::string, Loc> index_;
};

}  // namespace logkv
