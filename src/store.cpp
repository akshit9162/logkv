#include "logkv/store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "logkv/crc32.hpp"

namespace logkv {
namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

void put_u32(unsigned char* p, std::uint32_t v) noexcept {
    p[0] = static_cast<unsigned char>(v);
    p[1] = static_cast<unsigned char>(v >> 8);
    p[2] = static_cast<unsigned char>(v >> 16);
    p[3] = static_cast<unsigned char>(v >> 24);
}

std::uint32_t get_u32(const unsigned char* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// write()/pwrite() may legally write fewer bytes than asked. Looping is not
// optional; a short write that is silently ignored is a corrupt log.
void pwrite_full(int fd, const void* buf, std::size_t n, std::uint64_t off) {
    const auto* p = static_cast<const unsigned char*>(buf);
    while (n > 0) {
        ssize_t w = ::pwrite(fd, p, n, static_cast<off_t>(off));
        if (w < 0) {
            if (errno == EINTR) continue;
            throw_errno("pwrite");
        }
        p += w;
        off += static_cast<std::uint64_t>(w);
        n -= static_cast<std::size_t>(w);
    }
}

// Returns false on clean EOF (zero bytes available), true on a full read.
// A partial read is reported as false too: that is a torn tail, not an error.
bool pread_full(int fd, void* buf, std::size_t n, std::uint64_t off) {
    auto* p = static_cast<unsigned char*>(buf);
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::pread(fd, p + got, n - got, static_cast<off_t>(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw_errno("pread");
        }
        if (r == 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

}  // namespace

Store::Store(const std::string& path, Options opts) : path_(path), opts_(opts) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) throw_errno("open log");
    recover();
}

Store::Store(const std::string& path) : Store(path, Options{}) {}

Store::~Store() {
    if (fd_ >= 0) {
        try {
            do_fsync();
        } catch (...) {
            // Destructors do not throw. A failure here is already unrecoverable.
        }
        close_fd();
    }
}

Store::Store(Store&& o) noexcept
    : path_(std::move(o.path_)),
      opts_(o.opts_),
      fd_(std::exchange(o.fd_, -1)),
      tail_(std::exchange(o.tail_, 0)),
      since_sync_(std::exchange(o.since_sync_, 0)),
      recovered_(std::exchange(o.recovered_, 0)),
      truncated_(std::exchange(o.truncated_, 0)),
      index_(std::move(o.index_)) {}

Store& Store::operator=(Store&& o) noexcept {
    if (this != &o) {
        close_fd();
        path_ = std::move(o.path_);
        opts_ = o.opts_;
        fd_ = std::exchange(o.fd_, -1);
        tail_ = std::exchange(o.tail_, 0);
        since_sync_ = std::exchange(o.since_sync_, 0);
        recovered_ = std::exchange(o.recovered_, 0);
        truncated_ = std::exchange(o.truncated_, 0);
        index_ = std::move(o.index_);
    }
    return *this;
}

void Store::close_fd() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// Replay the log from the start, rebuilding the index. Stop at the first
// record that fails any structural or checksum check — everything from there
// on is a torn tail from a crash mid-append, and is discarded by truncating
// the file back to the last known-good offset. This is what makes a partial
// write invisible to the caller rather than silently corrupting a read.
void Store::recover() {
    struct stat st {};
    if (::fstat(fd_, &st) < 0) throw_errno("fstat");
    const auto file_size = static_cast<std::uint64_t>(st.st_size);

    std::uint64_t off = 0;
    unsigned char hdr[kHeaderSize];
    std::vector<unsigned char> body;

    while (off + kHeaderSize <= file_size) {
        if (!pread_full(fd_, hdr, kHeaderSize, off)) break;

        const std::uint32_t stored_crc = get_u32(hdr);
        const std::uint8_t type = hdr[4];
        const std::uint32_t klen = get_u32(hdr + 5);
        const std::uint32_t vlen = get_u32(hdr + 9);

        if (type != kPut && type != kTombstone) break;
        if (klen == 0) break;

        const std::uint64_t rec_len =
            static_cast<std::uint64_t>(kHeaderSize) + klen + vlen;
        if (off + rec_len > file_size) break;  // truncated tail

        body.resize(static_cast<std::size_t>(klen) + vlen);
        if (!body.empty() && !pread_full(fd_, body.data(), body.size(), off + kHeaderSize)) break;

        // CRC covers the header fields after the checksum, plus key and value.
        std::uint32_t c = crc32(hdr + 4, kHeaderSize - 4);
        c = crc32(body.data(), body.size(), c);
        if (c != stored_crc) break;

        std::string key(reinterpret_cast<const char*>(body.data()), klen);
        if (type == kPut) {
            index_[std::move(key)] = Loc{off, static_cast<std::uint32_t>(rec_len)};
        } else {
            index_.erase(key);
        }

        off += rec_len;
        ++recovered_;
    }

    if (off < file_size) {
        truncated_ = file_size - off;
        if (::ftruncate(fd_, static_cast<off_t>(off)) < 0) throw_errno("ftruncate");
    }
    tail_ = off;
}

void Store::append(std::uint8_t type, std::string_view key, std::string_view value) {
    if (key.empty()) throw std::invalid_argument("logkv: empty key");
    if (key.size() > 0xFFFFFFFFull || value.size() > 0xFFFFFFFFull) {
        throw std::length_error("logkv: key or value too large");
    }

    const std::uint32_t klen = static_cast<std::uint32_t>(key.size());
    const std::uint32_t vlen = static_cast<std::uint32_t>(value.size());
    const std::size_t rec_len = kHeaderSize + klen + vlen;

    // One buffer, one pwrite. This does not make the append atomic — nothing
    // at this layer can — but it keeps the torn window as small as the kernel
    // and device allow, and recovery handles the rest.
    std::vector<unsigned char> rec(rec_len);
    rec[4] = type;
    put_u32(rec.data() + 5, klen);
    put_u32(rec.data() + 9, vlen);
    std::memcpy(rec.data() + kHeaderSize, key.data(), klen);
    if (vlen) std::memcpy(rec.data() + kHeaderSize + klen, value.data(), vlen);
    put_u32(rec.data(), crc32(rec.data() + 4, rec_len - 4));

    pwrite_full(fd_, rec.data(), rec_len, tail_);

    const std::uint64_t rec_off = tail_;
    tail_ += rec_len;

    if (type == kPut) {
        index_[std::string(key)] = Loc{rec_off, static_cast<std::uint32_t>(rec_len)};
    } else {
        index_.erase(std::string(key));
    }

    maybe_sync();
}

void Store::put(std::string_view key, std::string_view value) {
    append(kPut, key, value);
}

bool Store::erase(std::string_view key) {
    const std::string k(key);
    if (index_.find(k) == index_.end()) return false;
    append(kTombstone, key, {});
    return true;
}

std::optional<std::string> Store::get(std::string_view key) const {
    const auto it = index_.find(std::string(key));
    if (it == index_.end()) return std::nullopt;

    const Loc loc = it->second;
    std::vector<unsigned char> rec(loc.len);
    if (!pread_full(fd_, rec.data(), rec.size(), loc.off)) {
        throw std::runtime_error("logkv: short read for indexed record");
    }

    // Verify on every read. The index says where the record is; it does not
    // say the bytes are still good.
    const std::uint32_t stored_crc = get_u32(rec.data());
    if (crc32(rec.data() + 4, rec.size() - 4) != stored_crc) {
        throw std::runtime_error("logkv: checksum mismatch on read");
    }

    const std::uint32_t klen = get_u32(rec.data() + 5);
    const std::uint32_t vlen = get_u32(rec.data() + 9);
    return std::string(reinterpret_cast<const char*>(rec.data()) + kHeaderSize + klen, vlen);
}

void Store::do_fsync() {
    if (fd_ < 0) return;
#ifdef __APPLE__
    if (opts_.full_fsync) {
        // fsync() on macOS returns once the data reaches the drive's write
        // cache, not stable media. F_FULLFSYNC asks the drive to flush.
        if (::fcntl(fd_, F_FULLFSYNC, 0) == 0) {
            since_sync_ = 0;
            return;
        }
        // Some filesystems do not support it; fall through to fsync().
    }
#endif
    if (::fsync(fd_) < 0) throw_errno("fsync");
    since_sync_ = 0;
}

void Store::maybe_sync() {
    switch (opts_.sync) {
        case Sync::None:
            break;
        case Sync::Always:
            do_fsync();
            break;
        case Sync::Batched:
            if (++since_sync_ >= opts_.batch_n) do_fsync();
            break;
    }
}

void Store::sync() { do_fsync(); }

// Rewrite the log with only the live version of each key, then swap it in with
// rename(2), which is atomic within a filesystem. The directory fsync after
// the rename is the part people forget: without it, the rename itself can be
// lost on power failure even though the new file's contents were durable.
void Store::compact() {
    const std::string tmp = path_ + ".compact";

    int nfd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (nfd < 0) throw_errno("open compaction target");

    std::unordered_map<std::string, Loc> new_index;
    new_index.reserve(index_.size());
    std::uint64_t noff = 0;

    try {
        for (const auto& [key, loc] : index_) {
            std::vector<unsigned char> rec(loc.len);
            if (!pread_full(fd_, rec.data(), rec.size(), loc.off)) {
                throw std::runtime_error("logkv: short read during compaction");
            }
            if (crc32(rec.data() + 4, rec.size() - 4) != get_u32(rec.data())) {
                throw std::runtime_error("logkv: checksum mismatch during compaction");
            }
            pwrite_full(nfd, rec.data(), rec.size(), noff);
            new_index[key] = Loc{noff, loc.len};
            noff += rec.size();
        }

        if (::fsync(nfd) < 0) throw_errno("fsync compaction target");
        if (::rename(tmp.c_str(), path_.c_str()) < 0) throw_errno("rename");
    } catch (...) {
        ::close(nfd);
        ::unlink(tmp.c_str());
        throw;
    }

    close_fd();
    fd_ = nfd;
    index_ = std::move(new_index);
    tail_ = noff;
    since_sync_ = 0;

    // Make the rename itself durable.
    const auto slash = path_.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".") : path_.substr(0, slash);
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

}  // namespace logkv
