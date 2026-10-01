#include "storage.h"

#include <algorithm>
#include <cstring>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace granite {

// ------------------------------------------------------------------ helpers

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
static void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i)); }
static uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(p[i]) << (8 * i); return v; }
static uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= uint64_t(p[i]) << (8 * i); return v; }

/// A running 64-bit checksum (FNV-1a), chained from frame to frame.
static uint64_t mix(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

// ------------------------------------------------------------------ MemFile

void MemFile::apply(std::vector<uint8_t>& bytes, const Op& op, size_t limit) {
    if (op.truncate) { bytes.resize(op.offset); return; }
    size_t n = std::min(limit, op.data.size());
    if (bytes.size() < op.offset + n) bytes.resize(op.offset + n);
    std::memcpy(bytes.data() + op.offset, op.data.data(), n);
}

void MemFile::read(uint64_t offset, uint8_t* buf, size_t n) const {
    std::memset(buf, 0, n);
    if (offset >= current_.size()) return;
    std::memcpy(buf, current_.data() + offset, std::min<uint64_t>(n, current_.size() - offset));
}

void MemFile::write(uint64_t offset, const uint8_t* buf, size_t n) {
    if (power_ && !power_->tick()) return;  // the power is already off
    Op op{false, offset, std::vector<uint8_t>(buf, buf + n)};
    apply(current_, op, n);
    pending_.push_back(std::move(op));
}

void MemFile::truncate(uint64_t n) {
    if (power_ && !power_->tick()) return;
    Op op{true, n, {}};
    apply(current_, op, 0);
    pending_.push_back(std::move(op));
}

void MemFile::sync() {
    if (power_ && !power_->tick()) return;
    durable_ = current_;
    pending_.clear();
}

int MemFile::crash(Rng& rng, bool allowTorn) {
    // Disks and operating systems reorder and buffer writes, so after a power
    // cut, each write since the last sync independently may or may not have
    // landed, and a write in progress may be torn at a 512-byte sector boundary.
    std::vector<uint8_t> after = durable_;
    int damaged = 0;
    for (const Op& op : pending_) {
        uint64_t r = rng.below(10);
        if (r < 4) { damaged++; continue; }  // lost
        if (r < 5 && allowTorn && !op.truncate && op.data.size() > 512) {
            size_t sectors = op.data.size() / 512;
            apply(after, op, 512 * (1 + rng.below(sectors - 1)));  // torn
            damaged++;
            continue;
        }
        apply(after, op, op.data.size());
    }
    durable_ = current_ = after;
    pending_.clear();
    return damaged;
}

// ------------------------------------------------------------------ DiskFile

DiskFile::DiskFile(const std::string& path) : path_(path) {
    f_ = std::fopen(path.c_str(), "r+b");
    if (!f_) f_ = std::fopen(path.c_str(), "w+b");
}
DiskFile::~DiskFile() { if (f_) std::fclose(f_); }

uint64_t DiskFile::size() const {
    std::fseek(f_, 0, SEEK_END);
    return uint64_t(std::ftell(f_));
}
void DiskFile::read(uint64_t offset, uint8_t* buf, size_t n) const {
    std::memset(buf, 0, n);
    if (std::fseek(f_, long(offset), SEEK_SET) == 0) {
        size_t got = std::fread(buf, 1, n, f_);
        (void)got;
    }
}
void DiskFile::write(uint64_t offset, const uint8_t* buf, size_t n) {
    std::fseek(f_, long(offset), SEEK_SET);
    std::fwrite(buf, 1, n, f_);
}
void DiskFile::truncate(uint64_t n) {
    std::fflush(f_);
#ifdef _WIN32
    _chsize_s(_fileno(f_), (long long)n);
#else
    if (ftruncate(fileno(f_), off_t(n)) != 0) { /* best effort */ }
#endif
}
void DiskFile::sync() {
    std::fflush(f_);
#ifdef _WIN32
    _commit(_fileno(f_));
#else
    fsync(fileno(f_));
#endif
}

// ------------------------------------------------------------------ Wal
//
// File layout:
//   header (32 bytes): "GRWAL001", page size, salt, header checksum
//   frames: [page number | commit page count (0 = not a commit) | salt | pad | checksum] + page
//
// The salt changes every time the log is reset, so frames left over from
// before a reset can never be mistaken for new ones.

void Wal::writeHeader() {
    uint8_t h[HEADER] = {};
    std::memcpy(h, "GRWAL001", 8);
    put32(h + 8, PAGE_BYTES);
    put32(h + 12, salt_);
    put64(h + 16, mix(0xCBF29CE484222325ull, h, 16));
    f_.write(0, h, HEADER);
}

RecoveryReport Wal::recover() {
    RecoveryReport report;
    index_.clear();
    frames_ = 0;
    dbPages_ = 0;
    uint8_t h[HEADER];
    f_.read(0, h, HEADER);
    bool valid = std::memcmp(h, "GRWAL001", 8) == 0 && get32(h + 8) == PAGE_BYTES &&
                 get64(h + 16) == mix(0xCBF29CE484222325ull, h, 16);
    if (!valid) {
        // no usable log: start a fresh one
        salt_ = (f_.size() >= HEADER) ? get32(h + 12) + 1 : 1;
        checksum_ = mix(0xCBF29CE484222325ull, reinterpret_cast<uint8_t*>(&salt_), 4);
        writeHeader();
        f_.truncate(HEADER);
        f_.sync();
        return report;
    }
    salt_ = get32(h + 12);
    uint64_t cs = mix(0xCBF29CE484222325ull, reinterpret_cast<uint8_t*>(&salt_), 4);
    checksum_ = cs;

    std::vector<std::pair<uint32_t, uint32_t>> pending;  // frames since the last commit
    uint64_t size = f_.size();
    std::vector<uint8_t> buf(FRAME);
    uint32_t i = 0;
    for (;; i++) {
        uint64_t off = HEADER + uint64_t(i) * FRAME;
        if (off + FRAME > size) break;
        f_.read(off, buf.data(), FRAME);
        if (get32(buf.data() + 8) != salt_) break;  // left over from before a reset
        uint64_t next = mix(cs, buf.data(), 16);
        next = mix(next, buf.data() + FRAME_HEADER, PAGE_BYTES);
        if (next != get64(buf.data() + 16) && !bugs_.skipWalChecksum) break;  // torn or garbage
        cs = next;
        pending.push_back({get32(buf.data()), i});
        uint32_t commitPages = get32(buf.data() + 4);
        if (commitPages) {
            // a commit frame: everything up to here is a committed transaction
            for (auto& [pgno, frame] : pending) index_[pgno] = frame;
            report.framesReplayed += uint32_t(pending.size());
            report.commitsRecovered++;
            pending.clear();
            dbPages_ = commitPages;
            frames_ = i + 1;
            checksum_ = cs;
        }
    }
    if (bugs_.replayUncommittedFrames && !pending.empty()) {
        for (auto& [pgno, frame] : pending) index_[pgno] = frame;
        report.framesReplayed += uint32_t(pending.size());
        frames_ = pending.back().second + 1;
        checksum_ = cs;
        pending.clear();
    }
    uint64_t totalFrames = size > HEADER ? (size - HEADER) / FRAME : 0;
    report.framesDiscarded = uint32_t(totalFrames - frames_);
    report.walWasEmpty = totalFrames == 0;
    return report;
}

void Wal::appendCommit(const std::vector<std::pair<uint32_t, const Page*>>& pages, uint32_t dbPages) {
    std::vector<uint8_t> buf(FRAME);
    uint64_t cs = checksum_;
    for (size_t k = 0; k < pages.size(); k++) {
        bool last = k + 1 == pages.size();
        std::memset(buf.data(), 0, FRAME_HEADER);
        put32(buf.data(), pages[k].first);
        put32(buf.data() + 4, last ? dbPages : 0);
        put32(buf.data() + 8, salt_);
        std::memcpy(buf.data() + FRAME_HEADER, pages[k].second->data(), PAGE_BYTES);
        cs = mix(cs, buf.data(), 16);
        cs = mix(cs, buf.data() + FRAME_HEADER, PAGE_BYTES);
        put64(buf.data() + 16, cs);
        f_.write(HEADER + uint64_t(frames_ + k) * FRAME, buf.data(), FRAME);
    }
    // The durability point: once this sync returns, the transaction survives any crash.
    if (!bugs_.commitWithoutSync) f_.sync();
    for (size_t k = 0; k < pages.size(); k++) index_[pages[k].first] = frames_ + uint32_t(k);
    frames_ += uint32_t(pages.size());
    checksum_ = cs;
    dbPages_ = dbPages;
}

bool Wal::readPage(uint32_t pgno, Page& out) const {
    auto it = index_.find(pgno);
    if (it == index_.end()) return false;
    f_.read(HEADER + uint64_t(it->second) * FRAME + FRAME_HEADER, out.data(), PAGE_BYTES);
    return true;
}

void Wal::reset() {
    salt_++;
    checksum_ = mix(0xCBF29CE484222325ull, reinterpret_cast<uint8_t*>(&salt_), 4);
    writeHeader();
    f_.truncate(HEADER);
    f_.sync();
    index_.clear();
    frames_ = 0;
    dbPages_ = 0;
}

// ------------------------------------------------------------------ Pager

Pager::Pager(File& db, File& wal, const Bugs& bugs, size_t cachePages)
    : db_(db), wal_(wal, bugs), bugs_(bugs), capacity_(cachePages) {
    stats_.capacity = uint32_t(cachePages);
}

RecoveryReport Pager::open() {
    RecoveryReport r = wal_.recover();
    committedPages_ = wal_.dbPages() ? wal_.dbPages() : uint32_t(db_.size() / PAGE_BYTES);
    txnPages_ = committedPages_;
    lru_.clear();
    cache_.clear();
    dirty_.clear();
    inTxn_ = false;
    return r;
}

void Pager::cacheInsert(uint32_t pgno, const Page& page) {
    auto it = cache_.find(pgno);
    if (it != cache_.end()) {
        it->second->second = page;
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    if (lru_.size() >= capacity_) {
        cache_.erase(lru_.back().first);  // evict the least recently used page
        lru_.pop_back();
        stats_.evictions++;
    }
    lru_.emplace_front(pgno, page);
    cache_[pgno] = lru_.begin();
    stats_.cached = uint32_t(lru_.size());
}

bool Pager::loadCommitted(uint32_t pgno, Page& out) {
    auto it = cache_.find(pgno);
    if (it != cache_.end()) {
        stats_.hits++;
        lru_.splice(lru_.begin(), lru_, it->second);
        out = it->second->second;
        return true;
    }
    stats_.misses++;
    if (!wal_.readPage(pgno, out)) db_.read(uint64_t(pgno) * PAGE_BYTES, out.data(), PAGE_BYTES);
    cacheInsert(pgno, out);
    return true;
}

bool Pager::get(uint32_t pgno, Page& out) {
    if (pgno >= txnPages_) return false;
    auto it = dirty_.find(pgno);
    if (it != dirty_.end()) { out = it->second; return true; }
    return loadCommitted(pgno, out);
}

void Pager::put(uint32_t pgno, const Page& page) { dirty_[pgno] = page; }

uint32_t Pager::allocate() {
    Page zero{};
    uint32_t pgno = txnPages_++;
    dirty_[pgno] = zero;
    return pgno;
}

void Pager::begin() {
    inTxn_ = true;
    dirty_.clear();
    txnPages_ = committedPages_;
}

void Pager::commit() {
    if (!dirty_.empty()) {
        std::vector<std::pair<uint32_t, const Page*>> pages;
        for (auto& [pgno, page] : dirty_) pages.push_back({pgno, &page});
        wal_.appendCommit(pages, txnPages_);
        for (auto& [pgno, page] : dirty_) {
            if (cache_.count(pgno)) cacheInsert(pgno, page);
        }
    }
    committedPages_ = txnPages_;
    dirty_.clear();
    inTxn_ = false;
}

void Pager::rollback() {
    dirty_.clear();
    txnPages_ = committedPages_;
    inTxn_ = false;
}

void Pager::checkpoint() {
    if (inTxn_ || wal_.frames() == 0) return;
    Page page;
    std::vector<std::pair<uint32_t, uint32_t>> frames(wal_.index().begin(), wal_.index().end());
    std::sort(frames.begin(), frames.end());
    for (auto& [pgno, frame] : frames) {
        wal_.readPage(pgno, page);
        db_.write(uint64_t(pgno) * PAGE_BYTES, page.data(), PAGE_BYTES);
    }
    if (bugs_.checkpointResetsWalFirst) {
        wal_.reset();  // BUG: the log is gone before the database file is safe
        db_.sync();
    } else {
        db_.sync();    // first make the database file durable...
        wal_.reset();  // ...and only then throw the log away
    }
}

}  // namespace granite
