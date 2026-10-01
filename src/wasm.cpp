// The interface the browser demo calls. Plain exported functions and a
// shared result buffer: JS copies a UTF-8 string in with g_alloc, calls a
// function, and reads JSON back via g_result_ptr / g_result_len.
#include <cstdlib>
#include <memory>
#include <string>

#include "database.h"
#include "torture.h"

using namespace granite;

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

namespace {
Power power;
MemFile dbFile(&power), walFile(&power);
std::unique_ptr<Database> db;
std::string result;
uint64_t plugs = 0;

std::string statsJson() {
    auto& s = db->pager().stats();
    return "{\"pages\":" + std::to_string(db->pager().pageCount()) + ",\"walFrames\":" + std::to_string(db->pager().walFrames()) +
           ",\"cached\":" + std::to_string(s.cached) + ",\"capacity\":" + std::to_string(s.capacity) +
           ",\"hits\":" + std::to_string(s.hits) + ",\"misses\":" + std::to_string(s.misses) +
           ",\"evictions\":" + std::to_string(s.evictions) + ",\"dirty\":" + std::to_string(db->pager().dirtyPages()) +
           ",\"inTransaction\":" + (db->inTransaction() ? "true" : "false") +
           ",\"dbBytes\":" + std::to_string(dbFile.size()) + ",\"walBytes\":" + std::to_string(walFile.size()) + "}";
}

std::string recoveryJson(const RecoveryReport& r) {
    return "{\"framesReplayed\":" + std::to_string(r.framesReplayed) + ",\"commitsRecovered\":" + std::to_string(r.commitsRecovered) +
           ",\"framesDiscarded\":" + std::to_string(r.framesDiscarded) + "}";
}

Bugs bugsFrom(int mask) {
    Bugs b;
    b.commitWithoutSync = mask & 1;
    b.checkpointResetsWalFirst = mask & 2;
    b.skipWalChecksum = mask & 4;
    b.replayUncommittedFrames = mask & 8;
    return b;
}
}  // namespace

EXPORT(g_alloc) char* g_alloc(int n) { return static_cast<char*>(std::malloc(size_t(n) + 1)); }
EXPORT(g_free) void g_free(char* p) { std::free(p); }
EXPORT(g_result_ptr) const char* g_result_ptr() { return result.data(); }
EXPORT(g_result_len) int g_result_len() { return int(result.size()); }

/// A fresh, empty in-memory database.
EXPORT(g_open) void g_open() {
    power = Power{};
    dbFile = MemFile(&power);
    walFile = MemFile(&power);
    db = std::make_unique<Database>(dbFile, walFile, Bugs{}, 32);
    db->open();
    result = statsJson();
}

EXPORT(g_exec) void g_exec(const char* sql, int len) {
    auto results = db->execute(std::string(sql, size_t(len)));
    result = "{\"results\":[";
    for (size_t i = 0; i < results.size(); i++) result += (i ? "," : "") + resultJson(results[i]);
    result += "],\"stats\":" + statsJson() + "}";
}

/// Tables, the B+ tree of one of them, and storage statistics.
EXPORT(g_state) void g_state(const char* table, int len) {
    std::string name(table, size_t(len));
    result = "{\"tables\":[";
    bool first = true;
    for (auto& t : db->tables()) {
        result += std::string(first ? "" : ",") + "{\"name\":" + jsonString(t.name) + ",\"root\":" + std::to_string(t.root) + ",\"columns\":[";
        for (size_t c = 0; c < t.cols.size(); c++)
            result += (c ? "," : "") + jsonString(t.cols[c].name + (t.cols[c].integer ? " INTEGER" : " TEXT") + (t.cols[c].primaryKey ? " PRIMARY KEY" : ""));
        result += "]}";
        first = false;
    }
    result += "],\"tree\":" + (name.empty() ? std::string("null") : db->treeJson(name)) + ",\"stats\":" + statsJson() + "}";
}

EXPORT(g_checkpoint) void g_checkpoint() {
    uint32_t frames = db->pager().walFrames();
    db->checkpoint();
    result = "{\"frames\":" + std::to_string(frames) + ",\"stats\":" + statsJson() + "}";
}

/// Cut the power. If `midCommit` and a transaction is open, the power fails
/// part-way through writing its commit to the log. Then the database is
/// reopened and recovers.
EXPORT(g_pull_plug) void g_pull_plug(int midCommit) {
    Rng rng(++plugs * 7919);
    bool wasInTxn = db->inTransaction();
    size_t dirty = db->pager().dirtyPages();
    bool cutDuringCommit = false;
    uint32_t framesBefore = db->pager().walFrames();
    if (midCommit && wasInTxn && dirty >= 1) {
        // let some (possibly all) of the commit's page writes through, then the
        // power dies before the log is synced
        power.budget = int64_t(rng.below(dirty + 1));
        db->execute("COMMIT");
        cutDuringCommit = power.failed;
    }
    int damaged = dbFile.crash(rng) + walFile.crash(rng);
    power = Power{};
    db = std::make_unique<Database>(dbFile, walFile, Bugs{}, 32);
    RecoveryReport rec = db->open();
    std::string integrity = db->check();
    // Whether the interrupted commit made it: if every frame happened to reach
    // the disk intact, recovery finds a complete commit and keeps it.
    bool survived = cutDuringCommit && db->pager().walFrames() > framesBefore;
    result = "{\"recovery\":" + recoveryJson(rec) + ",\"wasInTransaction\":" + (wasInTxn ? "true" : "false") +
             ",\"dirtyPagesLost\":" + std::to_string(dirty) + ",\"cutDuringCommit\":" + (cutDuringCommit ? "true" : "false") +
             ",\"transactionSurvived\":" + (survived ? "true" : "false") +
             ",\"writesDamaged\":" + std::to_string(damaged) + ",\"integrity\":" + jsonString(integrity) + ",\"stats\":" + statsJson() + "}";
}

/// One crash-torture scenario. `bugs` is a bitmask of injected bugs.
EXPORT(g_torture) void g_torture(int seed, int bugs) {
    TortureResult r = tortureRun(uint64_t(seed), bugsFrom(bugs));
    result = "{\"seed\":" + std::to_string(r.seed) + ",\"ok\":" + (r.ok ? "true" : "false") + ",\"failure\":" + jsonString(r.failure) +
             ",\"transactions\":" + std::to_string(r.transactions) + ",\"committed\":" + std::to_string(r.committed) +
             ",\"midCommit\":" + (r.crashedMidCommit ? "true" : "false") + ",\"midCheckpoint\":" + (r.crashedMidCheckpoint ? "true" : "false") +
             ",\"writesDamaged\":" + std::to_string(r.writesDamaged) + ",\"recovery\":" + recoveryJson(r.recovery) + "}";
}
