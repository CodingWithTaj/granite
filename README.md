# Granite

[![CI](https://github.com/CodingWithTaj/granite/actions/workflows/ci.yml/badge.svg)](https://github.com/CodingWithTaj/granite/actions/workflows/ci.yml)

A SQL database written from scratch in C++20, with no dependencies: B+ tree storage, a buffer pool, a write-ahead log with crash recovery, and a SQL engine. It compiles to WebAssembly, so you can run it, and pull its plug, in your browser.

**[Try it: a database you can unplug](https://CodingWithTaj.github.io/granite/)**

```
granite> CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, city TEXT, age INTEGER);
granite> INSERT INTO users (name, city, age) VALUES ('Ada', 'Vancouver', 36), ('Linus', 'Abbotsford', 28);
granite> SELECT city, COUNT(*) AS people, AVG(age) FROM users GROUP BY city ORDER BY people DESC;
granite> EXPLAIN SELECT * FROM users WHERE id = 2;
+--------------------------------------------+
| plan                                       |
+--------------------------------------------+
| SEARCH users USING PRIMARY KEY (id = 2)    |
+--------------------------------------------+
```

## The promise, and how it's checked

A database's real job is keeping a promise: **once `COMMIT` returns, the data survives anything**, including the power dying mid-write, and an interrupted transaction is never left half-applied.

Granite is tested against that promise by crashing it on purpose. It runs on a simulated disk that behaves like a real one in a power cut: synced data survives, but each write made since the last sync may land, vanish, or be **torn** part-way through a sector. A torture harness runs random transactions, cuts the power at a random moment (often in the middle of a commit or a checkpoint), restarts, and checks that:

| Promise | Check |
|---|---|
| Durability | Every transaction whose `COMMIT` returned is fully present |
| Atomicity | A transaction interrupted mid-commit is either entirely present or entirely absent |
| Integrity | Every B+ tree is structurally sound: sorted, balanced, sibling links intact. Every page has exactly one owner (a tree or the free list), and none is leaked |
| Still works | The recovered database accepts new writes |

```
$ ./granite --torture 10000
Crashed the database 10000 times (seeds 1-10000).
  384635 transactions, 339620 committed; 6683 crashes landed mid-commit
  recovery replayed 148451 log frames and discarded 15614 unfinished or torn ones

Every committed transaction survived, none was half-applied, and every B+ tree and the free list passed their checks.
```

A checker that never fails could simply be broken. So five bugs can be switched on, four classic durability bugs and one free-list bug, and the harness catches every one:

| Injected bug | Caught at | What it broke |
|---|---|---|
| Report a commit as done before the log is synced | seed 1 | a transaction half-applied after a mid-commit crash |
| Empty the log during a checkpoint before the database file is synced | seed 23 | a page on the free list that is also in use |
| Trust log frames without verifying checksums | seed 222 | a torn page accepted as real data |
| Replay frames from transactions that never committed | seed 2 | a page on the free list that is also in use |
| Hand out a free page without taking it off the free list | seed 1 | the free list loops back on itself |

Every scenario comes from its seed, so a failure replays exactly. The native and WebAssembly builds catch each bug on the same seed.

## How it works

```text
SQL text → tokenizer → parser → planner/executor → B+ trees → pager + buffer pool → write-ahead log → disk
```

**Pages and the B+ tree** (`src/btree.cpp`). Everything is stored in 4 KB pages. Each table is a B+ tree keyed by its integer primary key: leaf pages hold rows in key order and link to the next leaf; internal pages hold separator keys that route a search. A lookup by key reads one page per level, typically 2 or 3 pages, instead of the whole table. When a page overflows it splits, and when the root splits, its contents move to a new page so the root's page number never changes.

**The buffer pool** (`src/storage.cpp`). Committed pages are cached in memory with least-recently-used eviction. Pages changed by an open transaction are kept privately in memory and never written to disk until `COMMIT` (a *no-steal* policy), which makes rollback trivial: forget them.

**The write-ahead log**, the same design SQLite uses in WAL mode. On `COMMIT`, every changed page is appended to the log as a frame, the last frame is marked as the commit, and the log is synced to disk. That sync is the instant the transaction becomes permanent. Each frame carries a running checksum chained from the frame before, so a torn or partly written frame can't pass as valid. On startup, recovery reads the log, keeps every frame up to the last complete commit, and discards the rest.

**Checkpoints** copy the latest version of each logged page into the database file, sync the file, and only then reset the log. Resetting the log first (one of the injectable bugs) loses data if the power dies in between.

**The free list** (`Pager::allocate` and `Pager::free`). `DROP TABLE` hands every page of the table's B+ tree back to a linked list of unused pages: page 0 holds the head of the list and a count, and each free page stores the number of the next one. Allocating a page pops the head, and only when the list is empty does the file grow. Because these are ordinary page writes, they travel through the same transaction and log as everything else, so a rollback or a crash before `COMMIT` simply un-frees them and no extra recovery code is needed. After every recovery, the integrity check confirms that each page is owned exactly once, by a tree or by the list. The effect, measured: five rounds of `CREATE TABLE`, insert 1,000 rows, `DROP TABLE` grew the database to 317 pages (1,298,432 bytes) before the free list existed; now it stays at 65 pages (266,240 bytes).

**The SQL engine** (`src/database.cpp`). A hand-written tokenizer and recursive-descent parser; `CREATE TABLE`, `DROP TABLE`, `INSERT`, `SELECT` with `WHERE`, inner `JOIN`, `GROUP BY`, `HAVING`, `ORDER BY` (by expression, alias or position), `LIMIT`, `COUNT/SUM/MIN/MAX/AVG`, `SELECT` without a table, `UPDATE`, `DELETE`, `BEGIN/COMMIT/ROLLBACK`, and `EXPLAIN`. The planner turns conditions on the primary key (`id = 5`, `id >= 10 AND id < 20`) into a B+ tree range seek, and joins on a primary key become index lookups instead of nested scans. Column names are checked before execution, so errors are reported even on empty tables.

## Checked against SQLite

The query engine is tested differentially: random tables and over 18,000 random queries, updates, deletes and `CREATE`/`DROP TABLE` cycles are run on both Granite and SQLite (the most heavily tested database there is), and every result must match. That covers `NULL` handling in comparisons and `AND`/`OR`/`NOT`, integer division and remainders (including by zero), grouping, `HAVING`, every aggregate, joins, and ordering with `LIMIT`. The table cycles make later tables reuse the pages of dropped ones, and the integrity check runs after every statement. Current result: zero mismatches. The engine has also been run under AddressSanitizer and UndefinedBehaviorSanitizer through the full test suite and thousands of fuzzed, malformed statements, with no memory errors or undefined behavior.

## What the tests caught during development

- **A dishonest statistic.** The "pages read" count, which the demo uses to show index lookups beating scans, ignored the leaf pages a cursor walked through, so a full scan looked almost as cheap as an index lookup. Leaf reads are now counted.
- **A missing error.** `SELECT x FROM t` returned nothing instead of "no such column" when `t` was empty, because names were only resolved while evaluating rows. Every column reference is now checked before any row is touched.
- **A wrong explanation.** Pulling the plug mid-commit usually destroys the transaction, but if every page of the commit happens to reach the disk before the power dies, recovery correctly keeps it. The demo announced "gone" either way. It now checks what recovery actually found, and a browser test verifies every claim against the data.
- **A name clash.** WebAssembly's C library defines a `PAGE_SIZE` macro, which silently collided with Granite's own constant.

## Limits

Granite is a teaching-sized engine, and honest about it: one writer at a time, integer primary keys only, rows up to 1000 bytes (no overflow pages), no secondary indexes, and only partial space reclamation (a dropped table's pages are reused through the free list, but a page emptied by `DELETE` is never freed and the database file never shrinks); each is a well-defined next step.

## Building

You need a C++20 compiler (g++ 10+ or clang 12+) and `make`.

```bash
make            # the granite shell
make test       # 22 tests, including 1,500 crashes and the five injected bugs
make differential   # compare thousands of random queries against SQLite
./granite mydata.db                      # a database file (its log is mydata.db-wal)
./granite --torture 2000                 # crash it 2,000 times
./granite --torture 5000 --bug skipWalChecksum
```

On Windows, [w64devkit](https://github.com/skeeto/w64devkit) gives you g++ and make in a single folder, no installer needed.

To build the website, install the [WASI SDK](https://github.com/WebAssembly/wasi-sdk) and run:

```bash
make wasm WASI_SDK=/path/to/wasi-sdk
python3 scripts/build_site.py      # site/index.html, and site/granite.html as one file
```

Pushing to `main` runs the tests, the SQLite comparison and a 5,000-crash torture run, builds the WebAssembly, and deploys the site.

## License

MIT
