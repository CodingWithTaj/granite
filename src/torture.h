// Crash testing: run random transactions, cut the power at a random moment,
// recover, and check that every committed transaction survived intact.
#pragma once
#include <cstdint>
#include <string>

#include "storage.h"

namespace granite {

struct TortureResult {
    uint64_t seed = 0;
    bool ok = true;
    std::string failure;        // what went wrong, in plain words
    int transactions = 0;       // transactions attempted before the crash
    int committed = 0;          // ...of which reported as committed
    bool crashedMidCommit = false;
    bool crashedMidCheckpoint = false;
    int writesDamaged = 0;      // unsynced writes lost or torn by the crash
    RecoveryReport recovery;
};

TortureResult tortureRun(uint64_t seed, const Bugs& bugs = {});

struct TortureSummary {
    int runs = 0, failures = 0;
    long transactions = 0, commits = 0, framesReplayed = 0, framesDiscarded = 0, midCommit = 0;
    TortureResult firstFailure;
};

TortureSummary tortureMany(uint64_t firstSeed, int runs, const Bugs& bugs = {}, bool stopAtFirst = false);

}  // namespace granite
