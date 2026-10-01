"""Differential test: random tables and queries run on both Granite and SQLite;
every result must match.

    make differential          (or: python3 tests/differential.py FIRST_SEED LAST_SEED)
"""
import json, os, random, sqlite3, subprocess, sys
RUNNER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "diff-runner" + (".exe" if os.name == "nt" else ""))

def gen(seed):
    R = random.Random(seed)
    texts = ["'ant'", "'bee'", "'cat'", "'Dog'", "'eel'", "''", "'a b'"]
    def lit_int(): return str(R.choice([-5, -1, 0, 1, 2, 3, 5, 7, 10, 42, 100]))
    def val(col):
        if R.random() < 0.15: return "NULL"
        return R.choice(texts) if col in ("b", "s") else lit_int()
    setup = ["CREATE TABLE t1 (id INTEGER PRIMARY KEY, a INTEGER, b TEXT, c INTEGER)",
             "CREATE TABLE t2 (id INTEGER PRIMARY KEY, t1_id INTEGER, v INTEGER, s TEXT)"]
    n1, n2 = R.randint(0, 120), R.randint(0, 200)
    for i in range(n1):
        setup.append(f"INSERT INTO t1 (a, b, c) VALUES ({val('a')}, {val('b')}, {val('c')})")
    for i in range(n2):
        setup.append(f"INSERT INTO t2 (t1_id, v, s) VALUES ({R.randint(-1, n1 + 3)}, {val('v')}, {val('s')})")
    icols = {"t1": ["id", "a", "c"], "t2": ["id", "t1_id", "v"]}
    tcols = {"t1": ["b"], "t2": ["s"]}
    def pred(t, depth=0, q=""):
        r = R.random()
        if depth < 2 and r < 0.25:
            return f"({pred(t, depth+1, q)} {R.choice(['AND', 'OR'])} {pred(t, depth+1, q)})"
        if depth < 2 and r < 0.3:
            return f"NOT ({pred(t, depth+1, q)})"
        if r < 0.4:
            c = R.choice(icols[t] + tcols[t])
            return f"{q}{c} IS {R.choice(['', 'NOT '])}NULL"
        if r < 0.55:
            c = R.choice(tcols[t])
            return f"{q}{c} {R.choice(['=', '!=', '<', '>='])} {R.choice(texts)}"
        c = R.choice(icols[t])
        if r < 0.7:
            c2 = R.choice(icols[t])
            return f"{q}{c} {R.choice(['+', '-', '*', '%', '/'])} {q}{c2} {R.choice(['=', '<', '>', '!=', '<=', '>='])} {lit_int()}"
        return f"{q}{c} {R.choice(['=', '<', '>', '!=', '<=', '>='])} {lit_int()}"
    queries = []
    for _ in range(60):
        k = R.random()
        if k < 0.3:
            cols = R.choice(["*", "id, a", "b, c", "a + c, b", "id"])
            q = f"SELECT {cols} FROM t1"
            if R.random() < 0.8: q += " WHERE " + pred("t1")
            if R.random() < 0.5:
                q += f" ORDER BY {R.choice(['a', 'b', 'c', 'a DESC', 'b DESC'])}, id"
                if R.random() < 0.5: q += f" LIMIT {R.randint(0, 10)}"
            queries.append(("q", q))
        elif k < 0.5:
            agg = R.choice(["COUNT(*)", "COUNT(a)", "SUM(a)", "MIN(b)", "MAX(c)", "SUM(a + c)", "MIN(id)", "MAX(b)", "AVG(a)"])
            if R.random() < 0.6:
                g = R.choice(["b", "a", "c"])
                q = f"SELECT {g}, {agg} FROM t1"
                if R.random() < 0.5: q += " WHERE " + pred("t1")
                q += f" GROUP BY {g}"
            else:
                q = f"SELECT {agg} FROM t1"
                if R.random() < 0.7: q += " WHERE " + pred("t1")
            queries.append(("q", q))
        elif k < 0.58:
            g = R.choice(["b", "a"])
            agg = R.choice(["COUNT(*)", "SUM(c)", "MAX(a)"])
            q = f"SELECT {g}, {agg} AS x FROM t1 GROUP BY {g} HAVING {R.choice(['x', agg])} {R.choice(['>', '<', '>=', '='])} {lit_int()} ORDER BY 2, 1"
            queries.append(("q", q))
        elif k < 0.6:
            queries.append(("q", f"SELECT {lit_int()} + {lit_int()} * {lit_int()}, {lit_int()} / {lit_int()}, {lit_int()} % {lit_int()}"))
        elif k < 0.65:
            q = "SELECT t2.id, t1.b, t2.v FROM t2 JOIN t1 ON t1.id = t2.t1_id"
            if R.random() < 0.6: q += " WHERE " + pred("t2", q="t2.")
            queries.append(("q", q))
        elif k < 0.72:
            q = "SELECT t1.b, COUNT(*), SUM(t2.v) FROM t1 JOIN t2 ON t2.t1_id = t1.id GROUP BY t1.b"
            queries.append(("q", q))
        elif k < 0.86:
            c = R.choice(["a", "c"])
            q = f"UPDATE t1 SET {c} = {R.choice([lit_int(), 'a + 1', 'c * 2', 'NULL'])}, b = {R.choice(texts + ['NULL'])} WHERE " + pred("t1")
            queries.append(("w", q))
            queries.append(("q", "SELECT * FROM t1"))
        elif k < 0.94:
            queries.append(("w", "DELETE FROM t2 WHERE " + pred("t2")))
            queries.append(("q", "SELECT * FROM t2"))
        else:
            queries.append(("w", f"INSERT INTO t1 (a, b, c) VALUES ({val('a')}, {val('b')}, {val('c')})"))
            queries.append(("q", "SELECT * FROM t1 ORDER BY id"))
    return setup, queries

def norm(rows, ordered):
    out = []
    for r in rows:
        row = []
        for v in r:
            if isinstance(v, float): v = round(v, 2)
            if isinstance(v, float) and v == int(v): v = int(v)
            row.append(v)
        out.append(row)
    return out if ordered else sorted(out, key=lambda r: json.dumps(r))

total = mism = 0
for seed in range(int(sys.argv[1]), int(sys.argv[2])):
    setup, queries = gen(seed)
    lite = sqlite3.connect(":memory:")
    for s in setup: lite.execute(s)
    lines = setup + [q for _, q in queries]
    out = subprocess.run([RUNNER], input="\n".join(lines) + "\n", capture_output=True, text=True)
    if out.returncode != 0 or out.stderr:
        print("CRASH seed", seed, out.stderr[:2000]); sys.exit(1)
    res = [json.loads(l) for l in out.stdout.splitlines()]
    for i, s in enumerate(setup):
        if res[i].get("error") or "corrupt" in res[i]:
            print("SETUP FAIL", seed, s, res[i]); sys.exit(1)
    for j, (kind, q) in enumerate(queries):
        g = res[len(setup) + j]
        total += 1
        if "corrupt" in g: print("CORRUPT", seed, q, g); sys.exit(1)
        try:
            cur = lite.execute(q)
            want = cur.fetchall() if kind == "q" else None
        except Exception as e:
            print("sqlite rejected", q, e); continue
        if g.get("error"):
            mism += 1
            print("GRANITE ERROR seed", seed, "|", q, "|", g["error"]); continue
        if kind == "q":
            ordered = "ORDER BY" in q
            a, b = norm(g["rows"], ordered), norm([list(r) for r in want], ordered)
            if a != b:
                mism += 1
                if mism <= 8: print("MISMATCH seed", seed, "|", q, "\n  granite:", a[:6], "\n  sqlite: ", b[:6])
print(f"{total} statements compared with SQLite, {mism} mismatches")
sys.exit(1 if mism else 0)
