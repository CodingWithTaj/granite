#include "torture.h"

#include <map>

#include "database.h"

namespace granite {

namespace {
using Model = std::map<int64_t, std::string>;

std::string randomText(Rng& rng) {
    static const char* words[] = {"granite", "basalt", "quartz", "marble", "slate", "shale", "flint", "obsidian"};
    size_t n = 1 + rng.below(rng.chance(0.2) ? 60 : 6);
    std::string s;
    for (size_t i = 0; i < n; i++) s += std::string(i ? " " : "") + words[rng.below(8)];
    return s;
}

bool readAll(Database& db, Model& out, std::string& err) {
    auto res = db.execute("SELECT id, val FROM kv");
    if (res.empty() || !res.back().error.empty()) {
        err = res.empty() ? "no result" : res.back().error;
        return false;
    }
    out.clear();
    for (auto& row : res.back().rows) out[row[0].i] = row[1].s;
    return true;
}

std::string diff(const Model& want, const Model& got) {
    for (auto& [k, v] : want) {
        auto it = got.find(k);
        if (it == got.end()) return "row " + std::to_string(k) + " is missing";
        if (it->second != v) return "row " + std::to_string(k) + " has the wrong value";
    }
    for (auto& [k, v] : got) if (!want.count(k)) return "row " + std::to_string(k) + " shouldn't exist";
    return "";
}
}  // namespace

TortureResult tortureRun(uint64_t seed, const Bugs& bugs) {
    TortureResult r;
    r.seed = seed;
    Rng rng(seed);
    Power power;
    MemFile dbFile(&power), walFile(&power);
    Model committed;           // what the database has promised to keep
    Model inFlight;            // a transaction whose commit was interrupted
    bool ambiguous = false;    // ...which may legitimately be all there, or not at all

    {
        // a tiny buffer pool forces evictions and re-reads from the log
        Database db(dbFile, walFile, bugs, 4 + rng.below(12));
        db.autoCheckpointFrames = 1u << 30;  // checkpoints happen explicitly below
        db.open();
        db.execute("CREATE TABLE kv (id INTEGER PRIMARY KEY, val TEXT)");
        power.budget = int64_t(3 + rng.below(rng.chance(0.5) ? 60 : 900));

        for (int txn = 0; txn < 120 && !power.failed; txn++) {
            Model pending = committed;
            db.execute("BEGIN");
            int ops = int(1 + rng.below(rng.chance(0.3) ? 40 : 6));
            for (int k = 0; k < ops; k++) {
                int64_t key = int64_t(1 + rng.below(rng.chance(0.5) ? 40 : 600));
                double x = double(rng.below(1000)) / 1000;
                std::string sql;
                if (x < 0.65) {
                    std::string val = randomText(rng);
                    sql = pending.count(key) ? "UPDATE kv SET val = '" + val + "' WHERE id = " + std::to_string(key)
                                             : "INSERT INTO kv VALUES (" + std::to_string(key) + ", '" + val + "')";
                    pending[key] = val;
                } else {
                    sql = "DELETE FROM kv WHERE id = " + std::to_string(key);
                    pending.erase(key);
                }
                db.execute(sql);
            }
            r.transactions++;
            if (rng.chance(0.1)) { db.execute("ROLLBACK"); continue; }
            db.execute("COMMIT");
            if (power.failed) {
                // the power died during the commit: all or nothing is acceptable
                r.crashedMidCommit = ambiguous = true;
                inFlight = pending;
                break;
            }
            committed = pending;
            r.committed++;
            if (rng.chance(0.2)) {
                db.checkpoint();
                if (power.failed) r.crashedMidCheckpoint = true;
            }
        }
    }

    // pull the plug: unsynced writes are lost, kept or torn at random
    r.writesDamaged = dbFile.crash(rng) + walFile.crash(rng);
    power.failed = false;
    power.budget = -1;

    Database db(dbFile, walFile, bugs, 16);
    r.recovery = db.open();
    auto fail = [&](const std::string& why) { r.ok = false; r.failure = why; return r; };
    std::string corrupt = db.check();
    if (!corrupt.empty()) return fail("the database is corrupt after recovery: " + corrupt);
    Model got;
    std::string err;
    if (!readAll(db, got, err)) return fail("can't read the table after recovery: " + err);
    std::string vsCommitted = diff(committed, got);
    if (!vsCommitted.empty()) {
        if (!ambiguous) return fail("a committed transaction was damaged: " + vsCommitted);
        std::string vsInFlight = diff(inFlight, got);
        if (!vsInFlight.empty())
            return fail("a transaction interrupted mid-commit was half-applied: " + vsCommitted + " compared with before it, and " + vsInFlight + " compared with after it");
    }
    // the recovered database must still work
    auto res = db.execute("INSERT INTO kv VALUES (100000, 'after recovery')");
    if (res.empty() || !res.back().error.empty()) return fail("can't write after recovery: " + (res.empty() ? "" : res.back().error));
    if (!readAll(db, got, err) || !got.count(100000)) return fail("a write after recovery didn't stick");
    return r;
}

TortureSummary tortureMany(uint64_t firstSeed, int runs, const Bugs& bugs, bool stopAtFirst) {
    TortureSummary s;
    for (int i = 0; i < runs; i++) {
        TortureResult r = tortureRun(firstSeed + uint64_t(i), bugs);
        s.runs++;
        s.transactions += r.transactions;
        s.commits += r.committed;
        s.framesReplayed += r.recovery.framesReplayed;
        s.framesDiscarded += r.recovery.framesDiscarded;
        s.midCommit += r.crashedMidCommit;
        if (!r.ok) {
            if (s.failures++ == 0) s.firstFailure = r;
            if (stopAtFirst) break;
        }
    }
    return s;
}

}  // namespace granite
