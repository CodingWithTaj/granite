// Storage: files, the write-ahead log, and the pager with its buffer pool.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace granite {

constexpr uint32_t PAGE_BYTES = 4096;
using Page = std::array<uint8_t, PAGE_BYTES>;

/// Deliberate bugs that can be switched on to prove the crash harness catches them.
struct Bugs {
    bool commitWithoutSync = false;         // report a commit as done before the log reaches the disk
    bool checkpointResetsWalFirst = false;  // empty the log before the database file is safely written
    bool skipWalChecksum = false;           // trust log frames without verifying their checksums
    bool replayUncommittedFrames = false;   // apply log frames that were never committed
};

/// Small, fast, seeded random numbers (xorshift64), so every crash scenario replays exactly.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull) { if (!s) s = 1; }
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
    bool chance(double p) { return double(next() >> 11) * (1.0 / 9007199254740992.0) < p; }
};

/// A byte-addressed file. Reads past the end return zeros.
class File {
public:
    virtual ~File() = default;
    virtual uint64_t size() const = 0;
    virtual void read(uint64_t offset, uint8_t* buf, size_t n) const = 0;
    virtual void write(uint64_t offset, const uint8_t* buf, size_t n) = 0;
    virtual void truncate(uint64_t n) = 0;
    /// Block until everything written so far is durable (fsync).
    virtual void sync() = 0;
};

/// Simulated power supply: after `budget` more writes/syncs, the power fails.
struct Power {
    int64_t budget = -1;  // -1 = never fail
    bool failed = false;
    bool tick() {
        if (failed) return false;
        if (budget == 0) { failed = true; return false; }
        if (budget > 0) budget--;
        return true;
    }
};

/// An in-memory file that models what a real disk does when power is lost:
/// synced data always survives, but each write made since the last sync may
/// have reached the disk, been lost, or been torn part-way through a sector.
class MemFile : public File {
public:
    explicit MemFile(Power* power = nullptr) : power_(power) {}
    uint64_t size() const override { return current_.size(); }
    void read(uint64_t offset, uint8_t* buf, size_t n) const override;
    void write(uint64_t offset, const uint8_t* buf, size_t n) override;
    void truncate(uint64_t n) override;
    void sync() override;
    /// Power loss. Returns how many unsynced writes were lost or torn.
    int crash(Rng& rng, bool allowTorn = true);
    size_t unsyncedWrites() const { return pending_.size(); }
private:
    struct Op { bool truncate; uint64_t offset; std::vector<uint8_t> data; };
    static void apply(std::vector<uint8_t>& bytes, const Op& op, size_t limit);
    std::vector<uint8_t> durable_, current_;
    std::vector<Op> pending_;
    Power* power_;
};

/// A real file on disk.
class DiskFile : public File {
public:
    explicit DiskFile(const std::string& path);
    ~DiskFile() override;
    bool ok() const { return f_ != nullptr; }
    uint64_t size() const override;
    void read(uint64_t offset, uint8_t* buf, size_t n) const override;
    void write(uint64_t offset, const uint8_t* buf, size_t n) override;
    void truncate(uint64_t n) override;
    void sync() override;
private:
    std::string path_;
    FILE* f_ = nullptr;
};

/// What recovery found when the database was opened.
struct RecoveryReport {
    uint32_t framesReplayed = 0;     // log frames from committed transactions
    uint32_t commitsRecovered = 0;   // committed transactions found in the log
    uint32_t framesDiscarded = 0;    // frames from a transaction that never committed, or torn
    bool walWasEmpty = true;
};

/// The write-ahead log. A transaction's changed pages are appended as frames,
/// the last one marked as the commit, and the log is synced: that sync is the
/// moment the transaction becomes permanent. Each frame carries a running
/// checksum, so a torn or partly written frame is detected on recovery.
class Wal {
public:
    Wal(File& file, const Bugs& bugs) : f_(file), bugs_(bugs) {}
    RecoveryReport recover();
    void appendCommit(const std::vector<std::pair<uint32_t, const Page*>>& pages, uint32_t dbPages);
    bool readPage(uint32_t pgno, Page& out) const;
    /// Start an empty log (after a checkpoint).
    void reset();
    const std::unordered_map<uint32_t, uint32_t>& index() const { return index_; }
    uint32_t frames() const { return frames_; }
    uint32_t dbPages() const { return dbPages_; }
private:
    static constexpr uint64_t HEADER = 32, FRAME_HEADER = 24, FRAME = FRAME_HEADER + PAGE_BYTES;
    void writeHeader();
    File& f_;
    const Bugs& bugs_;
    uint32_t salt_ = 1;
    uint64_t checksum_ = 0;
    uint32_t frames_ = 0;
    uint32_t dbPages_ = 0;
    std::unordered_map<uint32_t, uint32_t> index_;  // page number -> latest committed frame
};

struct PagerStats {
    uint64_t hits = 0, misses = 0, evictions = 0;
    uint32_t cached = 0, capacity = 0;
};

/// The pager hands out pages to the B+ tree. Committed pages come from a
/// buffer pool (an LRU cache) in front of the log and the database file.
/// Pages changed by the current transaction are kept privately in memory and
/// only reach disk on commit, so a rollback or a crash simply forgets them.
class Pager {
public:
    Pager(File& db, File& wal, const Bugs& bugs, size_t cachePages = 64);
    RecoveryReport open();
    bool get(uint32_t pgno, Page& out);
    void put(uint32_t pgno, const Page& page);
    uint32_t allocate();
    uint32_t pageCount() const { return txnPages_; }
    void begin();
    void commit();
    void rollback();
    bool inTransaction() const { return inTxn_; }
    /// Copy committed pages from the log into the database file, then empty the log.
    void checkpoint();
    uint32_t walFrames() const { return wal_.frames(); }
    size_t dirtyPages() const { return dirty_.size(); }
    const PagerStats& stats() const { return stats_; }
private:
    bool loadCommitted(uint32_t pgno, Page& out);
    void cacheInsert(uint32_t pgno, const Page& page);
    File& db_;
    Wal wal_;
    const Bugs& bugs_;
    size_t capacity_;
    std::list<std::pair<uint32_t, Page>> lru_;  // front = most recently used
    std::unordered_map<uint32_t, std::list<std::pair<uint32_t, Page>>::iterator> cache_;
    std::map<uint32_t, Page> dirty_;
    uint32_t committedPages_ = 0, txnPages_ = 0;
    bool inTxn_ = false;
    PagerStats stats_;
};

}  // namespace granite
