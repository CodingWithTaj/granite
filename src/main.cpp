// The `granite` command-line shell.
#include <cstdio>
#include <iostream>
#include <string>

#include "database.h"
#include "torture.h"

using namespace granite;

static const char* HELP = R"(Granite: a small SQL database with crash-safe storage.

USAGE:
    granite                 an in-memory database
    granite FILE.db         open or create a database file (its log is FILE.db-wal)
    granite --torture N     crash-test N scenarios
    granite --torture N --bug NAME
                            prove the crash test catches a deliberate bug:
                            commitWithoutSync, checkpointResetsWalFirst,
                            skipWalChecksum, replayUncommittedFrames

SHELL COMMANDS:
    .tables  .schema  .tree TABLE  .stats  .checkpoint  .check  .help  .quit
Statements end with ';'.
)";

static bool setBug(Bugs& b, const std::string& name) {
    if (name == "commitWithoutSync") b.commitWithoutSync = true;
    else if (name == "checkpointResetsWalFirst") b.checkpointResetsWalFirst = true;
    else if (name == "skipWalChecksum") b.skipWalChecksum = true;
    else if (name == "replayUncommittedFrames") b.replayUncommittedFrames = true;
    else return false;
    return true;
}

static int torture(int runs, const std::string& bug) {
    Bugs bugs;
    if (!bug.empty() && !setBug(bugs, bug)) { std::fprintf(stderr, "unknown bug %s\n", bug.c_str()); return 2; }
    if (!bug.empty()) std::printf("Injected bug: %s\n\n", bug.c_str());
    TortureSummary s = tortureMany(1, runs, bugs, !bug.empty());
    std::printf("Crashed the database %d times (seeds 1-%d).\n", s.runs, s.runs);
    std::printf("  %ld transactions, %ld committed; %ld crashes landed mid-commit\n", s.transactions, s.commits, s.midCommit);
    std::printf("  recovery replayed %ld log frames and discarded %ld unfinished or torn ones\n\n", s.framesReplayed, s.framesDiscarded);
    if (s.failures == 0) {
        std::printf("Every committed transaction survived, none was half-applied, and every B+ tree passed its checks.\n");
        return bug.empty() ? 0 : 1;
    }
    std::printf("FAILED on seed %llu: %s\n", (unsigned long long)s.firstFailure.seed, s.firstFailure.failure.c_str());
    return bug.empty() ? 1 : 0;
}

int main(int argc, char** argv) {
    std::string path, bug;
    int tortureRuns = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { std::printf("%s", HELP); return 0; }
        if (a == "--torture" && i + 1 < argc) tortureRuns = std::stoi(argv[++i]);
        else if (a == "--bug" && i + 1 < argc) bug = argv[++i];
        else path = a;
    }
    if (tortureRuns) return torture(tortureRuns, bug);

    MemFile memDb, memWal;
    std::unique_ptr<DiskFile> diskDb, diskWal;
    File *dbFile = &memDb, *walFile = &memWal;
    if (!path.empty()) {
        diskDb = std::make_unique<DiskFile>(path);
        diskWal = std::make_unique<DiskFile>(path + "-wal");
        if (!diskDb->ok() || !diskWal->ok()) { std::fprintf(stderr, "can't open %s\n", path.c_str()); return 1; }
        dbFile = diskDb.get();
        walFile = diskWal.get();
    }
    Database db(*dbFile, *walFile);
    RecoveryReport rec = db.open();
    std::printf("Granite. %s Type .help for commands.\n", path.empty() ? "In-memory database." : ("Opened " + path + ".").c_str());
    if (rec.commitsRecovered) std::printf("Recovered %u committed transactions from the log.\n", rec.commitsRecovered);
    if (rec.framesDiscarded) std::printf("Discarded %u log frames from an unfinished transaction.\n", rec.framesDiscarded);

    std::string buffer, line;
    for (;;) {
        std::printf(buffer.empty() ? (db.inTransaction() ? "granite*> " : "granite> ") : "    ...> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line)) { std::printf("\n"); break; }
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            std::string cmd = line.substr(0, line.find(' ')), arg = line.find(' ') == std::string::npos ? "" : line.substr(line.find(' ') + 1);
            if (cmd == ".quit" || cmd == ".exit") break;
            else if (cmd == ".help") std::printf("%s", HELP);
            else if (cmd == ".tables") for (auto& t : db.tables()) std::printf("%s\n", t.name.c_str());
            else if (cmd == ".schema") {
                for (auto& t : db.tables()) {
                    std::printf("CREATE TABLE %s (", t.name.c_str());
                    for (size_t c = 0; c < t.cols.size(); c++)
                        std::printf("%s%s %s%s", c ? ", " : "", t.cols[c].name.c_str(), t.cols[c].integer ? "INTEGER" : "TEXT", t.cols[c].primaryKey ? " PRIMARY KEY" : "");
                    std::printf(");  -- root page %u\n", t.root);
                }
            } else if (cmd == ".tree") std::printf("%s\n", db.treeJson(arg).c_str());
            else if (cmd == ".check") { std::string e = db.check(); std::printf("%s\n", e.empty() ? "ok: every B+ tree passed its checks" : e.c_str()); }
            else if (cmd == ".checkpoint") { db.checkpoint(); std::printf("Checkpointed: the log was copied into the database file and emptied.\n"); }
            else if (cmd == ".stats") {
                auto& s = db.pager().stats();
                std::printf("pages: %u  log frames: %u  buffer pool: %u/%u pages, %llu hits, %llu misses, %llu evictions\n",
                            db.pager().pageCount(), db.pager().walFrames(), s.cached, s.capacity,
                            (unsigned long long)s.hits, (unsigned long long)s.misses, (unsigned long long)s.evictions);
            } else std::printf("unknown command %s (try .help)\n", cmd.c_str());
            continue;
        }
        buffer += line + "\n";
        if (line.find(';') == std::string::npos) continue;
        for (auto& r : db.execute(buffer)) {
            std::printf("%s", formatResult(r).c_str());
            if (r.isQuery && !r.plan.empty() && r.columns != std::vector<std::string>{"plan"})
                std::printf("plan: %s (%d page%s read)\n", r.plan[0].c_str(), r.pagesRead, r.pagesRead == 1 ? "" : "s");
        }
        buffer.clear();
    }
    return 0;
}
