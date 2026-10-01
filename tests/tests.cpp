// Tests: SQL, the B+ tree, transactions, recovery, and the crash harness.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "../src/database.h"
#include "../src/torture.h"

using namespace granite;

static int failures = 0, passed = 0;
static std::vector<std::pair<std::string, std::function<void()>>> tests;
#define TEST(name) static void name(); static bool reg_##name = (tests.push_back({#name, name}), true); static void name()
#define CHECK(cond) do { if (!(cond)) { std::printf("    FAILED: %s (line %d)\n", #cond, __LINE__); failures++; return; } } while (0)

struct Fixture {
    MemFile dbFile, walFile;
    Database db{dbFile, walFile};
    Fixture() { db.open(); }
    Result run(const std::string& sql) { auto r = db.execute(sql); return r.empty() ? Result{} : r.back(); }
};

static std::string cell(const Result& r, size_t row, size_t col) { return r.rows.at(row).at(col).show(); }

TEST(creates_inserts_and_selects) {
    Fixture f;
    CHECK(f.run("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)").error.empty());
    CHECK(f.run("INSERT INTO users (name, age) VALUES ('Ada', 36), ('Linus', 28), ('Grace', 45)").message == "Inserted 3 rows.");
    auto r = f.run("SELECT name FROM users WHERE age > 30 ORDER BY age DESC");
    CHECK(r.error.empty() && r.rows.size() == 2);
    CHECK(cell(r, 0, 0) == "Grace" && cell(r, 1, 0) == "Ada");
    CHECK(cell(f.run("SELECT id FROM users WHERE name = 'Linus'"), 0, 0) == "2");
}

TEST(aggregates_and_group_by) {
    Fixture f;
    f.run("CREATE TABLE t (id INTEGER PRIMARY KEY, city TEXT, n INTEGER)");
    f.run("INSERT INTO t (city, n) VALUES ('Abbotsford', 1), ('Vancouver', 5), ('Abbotsford', 3), ('Surrey', NULL)");
    auto r = f.run("SELECT COUNT(*), COUNT(n), SUM(n), MIN(n), MAX(n), AVG(n) FROM t");
    CHECK(cell(r, 0, 0) == "4" && cell(r, 0, 1) == "3" && cell(r, 0, 2) == "9" && cell(r, 0, 3) == "1" && cell(r, 0, 4) == "5" && cell(r, 0, 5) == "3.00");
    r = f.run("SELECT city, COUNT(*) AS c FROM t GROUP BY city ORDER BY c DESC, city");
    CHECK(r.rows.size() == 3 && cell(r, 0, 0) == "Abbotsford" && cell(r, 0, 1) == "2");
    CHECK(cell(f.run("SELECT COUNT(*) FROM t WHERE n > 100"), 0, 0) == "0");
}

TEST(select_without_a_table_having_and_order_by_position) {
    Fixture f;
    CHECK(cell(f.run("SELECT 1 + 2 * 3 AS n"), 0, 0) == "7");
    CHECK(f.run("SELECT 7 / 0").rows[0][0].isNull());
    CHECK(f.run("SELECT * ").error.find("FROM") != std::string::npos);
    f.run("CREATE TABLE t (id INTEGER PRIMARY KEY, city TEXT, n INTEGER)");
    f.run("INSERT INTO t (city, n) VALUES ('A', 1), ('B', 5), ('A', 3), ('C', 2), ('B', 1), ('A', 9)");
    auto r = f.run("SELECT city, COUNT(*) AS c FROM t GROUP BY city HAVING c >= 2 ORDER BY 2 DESC");
    CHECK(r.error.empty() && r.rows.size() == 2 && cell(r, 0, 0) == "A" && cell(r, 1, 0) == "B");
    r = f.run("SELECT city FROM t GROUP BY city HAVING SUM(n) > 5 ORDER BY 1");
    CHECK(r.rows.size() == 2 && cell(r, 0, 0) == "A" && cell(r, 1, 0) == "B");
    CHECK(f.run("SELECT city FROM t ORDER BY 3").error.find("out of range") != std::string::npos);
}

TEST(joins_use_the_primary_key) {
    Fixture f;
    f.run("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");
    f.run("CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, total INTEGER)");
    f.run("INSERT INTO users (name) VALUES ('Ada'), ('Linus')");
    f.run("INSERT INTO orders (user_id, total) VALUES (1, 30), (2, 10), (1, 25)");
    auto r = f.run("SELECT u.name, SUM(o.total) FROM orders o JOIN users u ON u.id = o.user_id GROUP BY u.name ORDER BY u.name");
    CHECK(r.error.empty() && r.rows.size() == 2 && cell(r, 0, 1) == "55" && cell(r, 1, 1) == "10");
    CHECK(r.plan.size() >= 2 && r.plan[1].find("USING PRIMARY KEY") != std::string::npos);
}

TEST(primary_key_lookups_read_few_pages) {
    Fixture f;
    f.run("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    std::string sql = "INSERT INTO t (v) VALUES ";
    for (int i = 0; i < 3000; i++) sql += std::string(i ? "," : "") + "('row " + std::to_string(i) + " with some padding text')";
    CHECK(f.run(sql).error.empty());
    auto point = f.run("SELECT v FROM t WHERE id = 1234");
    CHECK(cell(point, 0, 0) == "row 1233 with some padding text");
    CHECK(point.plan[0].find("SEARCH") != std::string::npos);
    auto scan = f.run("SELECT v FROM t WHERE v = 'row 1233 with some padding text'");
    CHECK(scan.plan[0].find("SCAN") != std::string::npos);
    CHECK(point.pagesRead * 10 < scan.pagesRead);
    auto range = f.run("SELECT COUNT(*) FROM t WHERE id >= 100 AND id < 200");
    CHECK(cell(range, 0, 0) == "100");
    CHECK(f.db.check().empty());
}

TEST(btree_stays_sound_through_random_inserts_and_deletes) {
    Fixture f;
    f.run("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    Rng rng(7);
    std::map<int64_t, bool> live;
    for (int i = 0; i < 4000; i++) {
        int64_t k = int64_t(rng.below(5000));
        if (rng.chance(0.7)) {
            std::string v(1 + rng.below(200), 'x');
            if (live.count(k)) f.run("UPDATE t SET v = '" + v + "' WHERE id = " + std::to_string(k));
            else f.run("INSERT INTO t VALUES (" + std::to_string(k) + ", '" + v + "')");
            live[k] = true;
        } else {
            f.run("DELETE FROM t WHERE id = " + std::to_string(k));
            live.erase(k);
        }
    }
    CHECK(f.db.check().empty());
    auto r = f.run("SELECT id FROM t");
    CHECK(r.rows.size() == live.size());
    size_t i = 0;
    for (auto& [k, _] : live) { CHECK(r.rows[i++][0].i == k); }
    CHECK(BTree(f.db.pager(), f.db.table("t")->root).depth() >= 2);
}

TEST(rollback_discards_changes) {
    Fixture f;
    f.run("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)");
    f.run("INSERT INTO t (v) VALUES ('keep')");
    f.run("BEGIN; INSERT INTO t (v) VALUES ('discard'); UPDATE t SET v = 'changed' WHERE id = 1");
    CHECK(f.run("SELECT COUNT(*) FROM t").rows[0][0].i == 2);
    f.run("ROLLBACK");
    auto r = f.run("SELECT v FROM t");
    CHECK(r.rows.size() == 1 && cell(r, 0, 0) == "keep");
}

TEST(errors_are_reported_clearly) {
    Fixture f;
    f.run("CREATE TABLE t (id INTEGER PRIMARY KEY, n INTEGER)");
    CHECK(f.run("SELEC * FROM t").error.find("expected a statement") != std::string::npos);
    CHECK(f.run("SELECT * FROM nope").error == "no such table: nope");
    CHECK(f.run("SELECT x FROM t").error == "no such column: x");
    CHECK(f.run("INSERT INTO t VALUES (1, 'abc')").error.find("needs an INTEGER") != std::string::npos);
    f.run("INSERT INTO t VALUES (1, 5)");
    CHECK(f.run("INSERT INTO t VALUES (1, 6)").error.find("duplicate primary key") != std::string::npos);
    CHECK(f.run("SELECT * FROM t WHERE n = 'oops").error.find("unterminated") != std::string::npos);
}

TEST(committed_data_survives_reopening) {
    MemFile dbFile, walFile;
    {
        Database db(dbFile, walFile);
        db.open();
        db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t (v) VALUES ('a'), ('b')");
    }
    Database db(dbFile, walFile);
    RecoveryReport rec = db.open();
    CHECK(rec.commitsRecovered >= 2);
    CHECK(db.execute("SELECT COUNT(*) FROM t").back().rows[0][0].i == 2);
}

TEST(uncommitted_data_vanishes_after_a_crash) {
    Power power;
    MemFile dbFile(&power), walFile(&power);
    {
        Database db(dbFile, walFile);
        db.open();
        db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t (v) VALUES ('committed')");
        db.execute("BEGIN; INSERT INTO t (v) VALUES ('never committed')");
    }
    Rng rng(1);
    dbFile.crash(rng);
    walFile.crash(rng);
    Database db(dbFile, walFile);
    db.open();
    auto r = db.execute("SELECT v FROM t").back();
    CHECK(r.rows.size() == 1 && cell(r, 0, 0) == "committed");
}

TEST(a_torn_log_frame_is_detected_and_discarded) {
    MemFile dbFile, walFile;
    {
        Database db(dbFile, walFile);
        db.open();
        db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t (v) VALUES ('safe')");
        db.execute("INSERT INTO t (v) VALUES ('torn')");
    }
    // corrupt a byte inside the last frame, as a torn write would
    uint8_t b = 0;
    walFile.read(walFile.size() - 100, &b, 1);
    b ^= 0xFF;
    walFile.write(walFile.size() - 100, &b, 1);
    Database db(dbFile, walFile);
    RecoveryReport rec = db.open();
    CHECK(rec.framesDiscarded >= 1);
    auto r = db.execute("SELECT v FROM t").back();
    CHECK(r.rows.size() == 1 && cell(r, 0, 0) == "safe");
}

TEST(checkpoint_moves_the_log_into_the_database_file) {
    MemFile dbFile, walFile;
    {
        Database db(dbFile, walFile);
        db.open();
        db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t (v) VALUES ('x'), ('y')");
        CHECK(db.pager().walFrames() > 0);
        db.checkpoint();
        CHECK(db.pager().walFrames() == 0);
    }
    Database db(dbFile, walFile);
    db.open();
    CHECK(db.execute("SELECT COUNT(*) FROM t").back().rows[0][0].i == 2);
}

TEST(crash_torture_finds_no_problems_in_the_real_engine) {
    TortureSummary s = tortureMany(1, 1500);
    if (s.failures) std::printf("    seed %llu: %s\n", (unsigned long long)s.firstFailure.seed, s.firstFailure.failure.c_str());
    CHECK(s.failures == 0);
    CHECK(s.midCommit > 50);           // plenty of crashes really did land mid-commit
    CHECK(s.framesDiscarded > 100);    // and recovery really did throw away unfinished work
}

TEST(crash_torture_is_reproducible) {
    auto a = tortureRun(42), b = tortureRun(42);
    CHECK(a.transactions == b.transactions && a.committed == b.committed && a.writesDamaged == b.writesDamaged);
}

static void expectCaught(Bugs bugs, const char* name) {
    TortureSummary s = tortureMany(1, 3000, bugs, true);
    if (!s.failures) std::printf("    %s survived 3000 crashes\n", name);
    else std::printf("    %s caught on seed %llu: %s\n", name, (unsigned long long)s.firstFailure.seed, s.firstFailure.failure.c_str());
    if (!s.failures) failures++;
}

TEST(crash_torture_catches_every_injected_bug) {
    Bugs a; a.commitWithoutSync = true; expectCaught(a, "commitWithoutSync");
    Bugs b; b.checkpointResetsWalFirst = true; expectCaught(b, "checkpointResetsWalFirst");
    Bugs c; c.skipWalChecksum = true; expectCaught(c, "skipWalChecksum");
    Bugs d; d.replayUncommittedFrames = true; expectCaught(d, "replayUncommittedFrames");
}

int main() {
    for (auto& [name, fn] : tests) {
        int before = failures;
        fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name.c_str());
        passed += failures == before;
    }
    std::printf("\n%d passed, %d failed\n", passed, int(tests.size()) - passed);
    return failures ? 1 : 0;
}
