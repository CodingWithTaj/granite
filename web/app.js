/* Granite in the browser: the C++ database compiled to WebAssembly. */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const esc = (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  const kb = (n) => (n < 1024 ? `${n} B` : `${(n / 1024).toFixed(n < 10240 ? 1 : 0)} KB`);
  let x, mem;

  // ---------------------------------------------------------------- WebAssembly
  async function load() {
    let bytes;
    if (window.GRANITE_WASM_B64) {
      const bin = atob(window.GRANITE_WASM_B64);
      bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    } else {
      bytes = new Uint8Array(await (await fetch("granite.wasm")).arrayBuffer());
    }
    // The few system calls the C++ runtime links against. The database
    // itself never touches real files here: its "disk" is simulated in memory.
    const wasi = {
      fd_write: (fd, iovs, n, written) => { const v = new DataView(mem.buffer); let t = 0; for (let i = 0; i < n; i++) t += v.getUint32(iovs + 8 * i + 4, true); v.setUint32(written, t, true); return 0; },
      fd_close: () => 0, fd_seek: () => 0, fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8,
      proc_exit: (code) => { throw new Error("exit " + code); },
    };
    const { instance } = await WebAssembly.instantiate(bytes, { wasi_snapshot_preview1: wasi });
    x = instance.exports;
    mem = x.memory;
    x._initialize();
  }
  const out = () => JSON.parse(new TextDecoder().decode(new Uint8Array(mem.buffer, x.g_result_ptr(), x.g_result_len())));
  function call(fn, text) {
    const b = new TextEncoder().encode(text);
    const p = x.g_alloc(b.length);
    new Uint8Array(mem.buffer, p, b.length).set(b);
    fn(p, b.length);
    x.g_free(p);
    return out();
  }
  const exec = (sql) => call(x.g_exec, sql);

  // ---------------------------------------------------------------- sample data
  const FIRST = ["Ada", "Grace", "Linus", "Ken", "Barbara", "Edsger", "Margaret", "Donald", "Frances", "Alan", "Radia", "Tim", "Hedy", "Dennis", "Katherine", "John", "Shafi", "Leslie", "Anita", "Niklaus"];
  const LAST = ["Moore", "Okafor", "Singh", "Tanaka", "Novak", "Silva", "Kowalski", "Haddad", "Chen", "Larsen", "Dubois", "Rossi", "Murphy", "Kim", "Patel"];
  const CITIES = ["Vancouver", "Abbotsford", "Surrey", "Burnaby", "Richmond", "Chilliwack", "Langley", "Victoria"];
  const ITEMS = ["keyboard", "monitor", "laptop stand", "headphones", "desk lamp", "webcam", "mouse", "USB hub", "chair", "notebook"];
  let rngState = 12345;
  const rnd = (n) => { rngState = (rngState * 1103515245 + 12345) % 2147483648; return rngState % n; };
  const person = () => [`${FIRST[rnd(FIRST.length)]} ${LAST[rnd(LAST.length)]}`, CITIES[rnd(CITIES.length)], 18 + rnd(60)];
  function insertUsers(n) {
    const rows = [];
    for (let i = 0; i < n; i++) { const [name, city, age] = person(); rows.push(`('${name}', '${city}', ${age})`); }
    return `INSERT INTO users (name, city, age) VALUES ${rows.join(", ")};`;
  }
  function seed() {
    x.g_open();
    exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, city TEXT, age INTEGER); CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER, item TEXT, total INTEGER);");
    for (let b = 0; b < 3; b++) exec(insertUsers(200));
    for (let b = 0; b < 3; b++) {
      const rows = [];
      for (let i = 0; i < 500; i++) rows.push(`(${1 + rnd(600)}, '${ITEMS[rnd(ITEMS.length)]}', ${5 + rnd(95)})`);
      exec(`INSERT INTO orders (user_id, item, total) VALUES ${rows.join(", ")};`);
    }
    x.g_checkpoint();
  }

  // ---------------------------------------------------------------- examples
  let user42 = "";
  const examples = () => [
    ["Find user #42", "SELECT * FROM users WHERE id = 42;"],
    ["Find by name (no index)", `SELECT * FROM users WHERE name = '${user42}';`],
    ["People per city", "SELECT city, COUNT(*) AS people, AVG(age) AS avg_age\nFROM users\nGROUP BY city\nORDER BY people DESC;"],
    ["Top spenders (a join)", "SELECT u.name, COUNT(*) AS orders, SUM(o.total) AS spent\nFROM orders o JOIN users u ON u.id = o.user_id\nGROUP BY u.name\nORDER BY spent DESC\nLIMIT 5;"],
    ["Add 300 users", "__insert__"],
    ["Start a transaction", "BEGIN;\nDELETE FROM orders WHERE total > 40;\nSELECT COUNT(*) AS orders_left FROM orders;"],
    ["Commit it", "COMMIT;"],
  ];

  // ---------------------------------------------------------------- state and drawing
  let state = null, table = "users", lastKey = null, plugged = null;

  function refresh() {
    state = call(x.g_state, table);
    const sel = $("tableSel");
    const names = state.tables.map((t) => t.name);
    if (!names.includes(table) && names.length) { table = names[0]; state = call(x.g_state, table); }
    sel.innerHTML = names.map((n) => `<option${n === table ? " selected" : ""}>${esc(n)}</option>`).join("");
    drawStatus();
    drawStorage();
    drawTree();
    const canCut = state.stats.inTransaction && state.stats.dirty >= 1;
    $("plugCommit").disabled = !canCut;
    $("plugCommit").title = canCut ? "Run COMMIT, and cut the power while it's being written"
      : "Start a transaction (BEGIN) and change some data first";
  }

  function drawStatus() {
    const s = state.stats, box = $("status");
    let cls = "", h, p;
    if (plugged) {
      cls = "out";
      h = plugged.cutDuringCommit && plugged.transactionSurvived ? "The power died mid-commit, but every page of the commit had reached the disk: the whole transaction survived."
        : plugged.cutDuringCommit ? "The power died mid-commit. The transaction is gone entirely: all or nothing."
        : plugged.wasInTransaction ? "The power died with a transaction open. Its changes are gone; everything committed is intact."
        : "The power died, and every committed transaction came back.";
      p = (plugged.recovery.commitsRecovered
            ? `Recovery replayed ${plugged.recovery.framesReplayed} log frames from ${plugged.recovery.commitsRecovered} committed transaction${plugged.recovery.commitsRecovered === 1 ? "" : "s"}`
            : "Every committed change was already in the database file") +
          `.${plugged.recovery.framesDiscarded ? ` Recovery threw away ${plugged.recovery.framesDiscarded} log frames that never formed a complete commit.` : ""}` +
          ` ${plugged.integrity ? "Integrity check FAILED: " + esc(plugged.integrity) : "Every B+ tree passed its integrity check."}`;
    } else if (s.inTransaction) {
      cls = "warn";
      h = `A transaction is open: ${s.dirty} changed page${s.dirty === 1 ? " exists" : "s exist"} only in memory.`;
      p = "COMMIT appends them to the write-ahead log and syncs it to disk, which makes them permanent. Pull the plug now and they vanish, as if the transaction never started.";
    } else {
      h = "Everything committed is safe on disk.";
      p = s.walFrames
        ? `The write-ahead log holds ${s.walFrames} page${s.walFrames === 1 ? "" : "s"} of recent changes that the database file hasn't absorbed yet. If the power died now, recovery would replay them.`
        : "The log is empty: every committed change has been copied into the database file. Try a query, or pull the plug.";
    }
    box.className = `status ${cls}`;
    box.innerHTML = `<h1>${h}</h1><p>${p}</p>`;
  }

  function drawStorage() {
    const s = state.stats;
    const lookups = s.hits + s.misses;
    $("storage").innerHTML = `
      <div class="meter"><b>${s.walFrames}</b><span>pages in the write-ahead log (${kb(s.walBytes)})</span></div>
      <div class="meter"><b>${kb(s.dbBytes)}</b><span>database file, ${s.pages} pages of 4 KB</span></div>
      <div class="meter"><b>${lookups ? Math.round((100 * s.hits) / lookups) : 0}%</b><span>buffer pool hits (${s.cached}/${s.capacity} pages cached)</span></div>`;
  }

  function drawTree() {
    const svg = $("tree"), root = state.tree;
    if (!root) { svg.innerHTML = ""; $("treeHint").textContent = ""; return; }
    // lay out leaves left to right; parents sit over their children
    const levels = [], leaves = [];
    (function walk(n, d) {
      if (!n) return;
      n.depth = d;
      (levels[d] = levels[d] || []).push(n);
      if (n.leaf) leaves.push(n); else n.kids.forEach((k) => walk(k, d + 1));
    })(root, 0);
    // fit the tree to the panel: leaves shrink (and drop their labels) as the table grows
    const avail = Math.max(280, $("treeWrap").clientWidth - 4);
    const slot = Math.min(66, (avail - 24) / leaves.length);
    const GAP = slot >= 24 ? 8 : slot >= 10 ? 3 : 1, LH = 84;
    const LW = Math.max(2, slot - GAP);
    leaves.forEach((l, i) => (l.x = 12 + i * (LW + GAP) + LW / 2));
    const label = (n) => (n.keys.length > 4 ? `${n.keys.slice(0, 3).join(" · ")} … ${n.keys.length} keys` : n.keys.join(" · "));
    (function place(n) {
      if (!n || n.leaf) return;
      n.kids.forEach(place);
      const xs = n.kids.filter(Boolean).map((k) => k.x);
      n.x = (Math.min(...xs) + Math.max(...xs)) / 2;
    })(root);
    const width = Math.max(leaves.length * (LW + GAP) + 24, avail);
    const iw = (n) => Math.max(96, label(n).length * 6.4 + 20);
    for (const lv of levels) for (const n of lv) if (!n.leaf) n.x = Math.min(Math.max(n.x, iw(n) / 2 + 4), width - iw(n) / 2 - 4);
    const depth = levels.length;
    const height = depth * LH + 10;
    // the path a lookup of `lastKey` follows
    const onPath = new Set();
    if (lastKey !== null) {
      let n = root;
      while (n) {
        onPath.add(n);
        if (n.leaf) break;
        let i = 0;
        while (i < n.keys.length && lastKey >= n.keys[i]) i++;
        n = n.kids[i];
      }
    }
    let g = "";
    for (const lv of levels) for (const n of lv) {
      if (n.leaf) continue;
      n.kids.forEach((k) => {
        if (!k) return;
        const hot = onPath.has(n) && onPath.has(k);
        g += `<line x1="${n.x}" y1="${n.depth * LH + 40}" x2="${k.x}" y2="${k.depth * LH + 8}" stroke="${hot ? "#F25C05" : "#C3C8D0"}" stroke-width="${hot ? 2.5 : 1}"/>`;
      });
    }
    for (let i = 0; GAP >= 3 && i + 1 < leaves.length; i++) {
      const a = leaves[i], b = leaves[i + 1], y = a.depth * LH + 46;
      g += `<line x1="${a.x + LW / 2 - 2}" y1="${y}" x2="${b.x - LW / 2 + 2}" y2="${y}" stroke="#9AA2AD" stroke-width="1" marker-end="url(#arr)"/>`;
    }
    for (const lv of levels) for (const n of lv) {
      const hot = onPath.has(n);
      const y = n.depth * LH + 8;
      if (n.leaf) {
        const fill = Math.min(100, n.fill);
        g += `<g><title>Leaf page ${n.page}: ${n.count} rows${n.count ? `, keys ${n.first} to ${n.last}` : ""}, ${fill}% full</title>
          <rect x="${n.x - LW / 2}" y="${y}" width="${LW}" height="32" rx="4" fill="#fff" stroke="${hot ? "#F25C05" : "#9AA2AD"}" stroke-width="${hot ? 2.5 : 1}"/>
          <rect x="${n.x - LW / 2 + 1}" y="${y + 33 - 32 * fill / 100}" width="${LW - 2}" height="${31 * fill / 100}" rx="3" fill="#C9D9CF" opacity="0.9"/>
          ${LW >= 44 ? `<text x="${n.x}" y="${y + 14}" text-anchor="middle" font-size="10" font-weight="700" fill="#1C1F23">${n.count ? n.first : "–"}</text>
          <text x="${n.x}" y="${y + 26}" text-anchor="middle" font-size="9" fill="#3E444D">${n.count ? "…" + n.last : "empty"}</text>` : ""}</g>`;
      } else {
        const shown = label(n), IW = iw(n);
        g += `<g><title>Internal page ${n.page}: ${n.keys.length} separator keys route each search to one of ${n.kids.length} children</title>
          <rect x="${n.x - IW / 2}" y="${y}" width="${IW}" height="32" rx="5" fill="#1C1F23" stroke="${hot ? "#F25C05" : "#1C1F23"}" stroke-width="${hot ? 3 : 1}"/>
          <text x="${n.x}" y="${y + 13}" text-anchor="middle" font-size="9" fill="#A7AFBA">page ${n.page}</text>
          <text x="${n.x}" y="${y + 25}" text-anchor="middle" font-size="10" font-weight="700" fill="#fff">${esc(shown)}</text></g>`;
      }
    }
    svg.setAttribute("width", width);
    svg.setAttribute("height", height);
    svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
    svg.innerHTML = `<defs><marker id="arr" viewBox="0 0 6 6" refX="5" refY="3" markerWidth="6" markerHeight="6" orient="auto"><path d="M0 0L6 3L0 6z" fill="#9AA2AD"/></marker></defs>${g}`;
    $("treeHint").innerHTML = `<b>${esc(table)}</b>: ${depth} level${depth === 1 ? "" : "s"}, ${leaves.length} leaf page${leaves.length === 1 ? "" : "s"}. ` +
      (lastKey !== null ? `The orange path is how the lookup of key ${lastKey} found its row: ${depth} page reads instead of ${leaves.length}.` : "Look up a row by id to see the path a search takes.");
  }

  function renderResults(results, sql) {
    let h = "";
    for (const r of results) {
      h += `<div class="res">`;
      if (r.error) {
        let where = "";
        if (r.errorPos >= 0) {
          const before = sql.slice(0, r.errorPos);
          const line = before.split("\n").length, col = r.errorPos - before.lastIndexOf("\n");
          where = ` (line ${line}, column ${col})`;
        }
        h += `<p class="err">Error${where}: ${esc(r.error)}</p>`;
      } else if (r.isQuery) {
        const shown = r.rows.slice(0, 100);
        h += `<table><thead><tr>${r.columns.map((c) => `<th>${esc(c)}</th>`).join("")}</tr></thead><tbody>` +
          shown.map((row) => `<tr>${row.map((v) => v === null ? `<td class="null">NULL</td>` : `<td${typeof v === "number" ? ' class="n"' : ""}>${esc(typeof v === "number" && !Number.isInteger(v) ? v.toFixed(2) : v)}</td>`).join("")}</tr>`).join("") +
          `</tbody></table>`;
        if (r.rows.length > shown.length) h += `<p class="more">…and ${r.rows.length - shown.length} more rows</p>`;
        if (r.plan.length && r.columns[0] !== "plan") {
          h += `<div class="plan">Plan: ${r.plan.map((p) => `<code>${esc(p)}</code>`).join(" ")} <span>· <b>${r.pagesRead}</b> page${r.pagesRead === 1 ? "" : "s"} read · ${r.rows.length} row${r.rows.length === 1 ? "" : "s"}</span></div>`;
        }
      } else {
        h += `<p class="msg">${esc(r.message)}</p>`;
      }
      h += `</div>`;
    }
    $("results").innerHTML = h || `<p class="hint">Nothing to run.</p>`;
  }

  function run(sql) {
    if (sql === "__insert__") sql = insertUsers(300);
    // a transaction is already open: run the rest without starting another
    if (state && state.stats.inTransaction) sql = sql.replace(/^\s*BEGIN\s*;\s*/i, "");
    plugged = null;
    $("report").innerHTML = "";
    const res = exec(sql);
    renderResults(res.results, sql);
    const m = /SEARCH (\w+) USING PRIMARY KEY \(\w+ = (-?\d+)\)/.exec(res.results.map((r) => r.plan.join(" ")).join(" "));
    lastKey = m ? Number(m[2]) : null;
    if (m) table = m[1];
    refresh();
  }

  // ---------------------------------------------------------------- the plug
  function pullPlug(midCommit) {
    x.g_pull_plug(midCommit ? 1 : 0);
    plugged = out();
    lastKey = null;
    const r = plugged, rec = r.recovery;
    const steps = [];
    if (r.cutDuringCommit && r.transactionSurvived) steps.push(`The power failed while the commit was being written, before the log was synced. By luck, all ${r.dirtyPagesLost} of its pages had already reached the disk intact, so recovery found a complete, valid commit and kept it. All or nothing: this time, all.`);
    else if (r.cutDuringCommit) steps.push(`The power failed part-way through writing a commit: of its ${r.dirtyPagesLost} changed page${r.dirtyPagesLost === 1 ? "" : "s"}, not all reached the disk intact, so the commit is incomplete.`);
    else if (r.wasInTransaction) steps.push(`An open transaction with ${r.dirtyPagesLost} changed page${r.dirtyPagesLost === 1 ? "" : "s"} was lost. Those pages only ever existed in memory.`);
    else steps.push("No transaction was open, so nothing was in flight.");
    if (r.writesDamaged) steps.push(`${r.writesDamaged} disk write${r.writesDamaged === 1 ? " that hadn't been synced was" : "s that hadn't been synced were"} lost or torn, as happens in a real power cut.`);
    steps.push(rec.commitsRecovered
      ? `On restart, recovery read the write-ahead log and replayed <b>${rec.framesReplayed}</b> frames from <b>${rec.commitsRecovered}</b> committed transaction${rec.commitsRecovered === 1 ? "" : "s"} that the database file hadn't absorbed yet.`
      : "On restart, recovery found no committed changes waiting in the log: everything committed was already in the database file.");
    if (rec.framesDiscarded) steps.push(`It discarded <b>${rec.framesDiscarded}</b> frames that weren't part of a complete commit; checksums reject torn ones.`);
    steps.push(r.integrity ? `Integrity check failed: ${esc(r.integrity)}` : `<span class="ok">Every B+ tree passed its integrity check.</span>`);
    $("report").innerHTML = `<div class="report"><h3>Power cut, restart, recovery</h3><ol>${steps.map((s) => `<li>${s}</li>`).join("")}</ol></div>`;
    renderResults([], "");
    $("results").innerHTML = `<p class="hint">The database restarted. Run a query to see what survived.</p>`;
    refresh();
  }

  // ---------------------------------------------------------------- torture test
  $("runTorture").addEventListener("click", () => {
    const runs = Math.max(1, Math.min(20000, Number($("runs").value) || 500));
    const first = Math.max(1, Number($("seed").value) || 1);
    const bugs = Number(document.querySelector('input[name="bug"]:checked').value);
    const btn = $("runTorture"), bar = $("bar");
    btn.disabled = true;
    bar.className = "";
    let done = 0, tx = 0, commits = 0, mid = 0, replayed = 0, discarded = 0;
    const t0 = performance.now();
    const stat = (v, label) => `<div class="meter"><b>${v.toLocaleString()}</b><span>${label}</span></div>`;
    const stats = () => `<div class="tstats">${stat(done, "power cuts")}${stat(commits, "committed transactions checked")}${stat(mid, "cuts landed mid-commit")}${stat(discarded, "unfinished log frames discarded")}</div>`;
    const step = () => {
      const until = Math.min(runs, done + 5);
      for (; done < until; done++) {
        x.g_torture(first + done, bugs);
        const r = out();
        tx += r.transactions; commits += r.committed; mid += r.midCommit ? 1 : 0;
        replayed += r.recovery.framesReplayed; discarded += r.recovery.framesDiscarded;
        if (!r.ok) {
          done++;
          btn.disabled = false;
          bar.className = "bad"; bar.style.width = "100%";
          $("tresults").innerHTML = `<p class="big-bad">Caught on power cut #${done} (seed ${r.seed}).</p>${stats()}<div class="failure">${esc(r.failure[0].toUpperCase() + r.failure.slice(1))}.</div>`;
          return;
        }
      }
      bar.style.width = `${(100 * done) / runs}%`;
      $("tresults").innerHTML = stats();
      if (done < runs) setTimeout(step, 0);
      else {
        btn.disabled = false;
        const secs = ((performance.now() - t0) / 1000).toFixed(1);
        $("tresults").innerHTML = `<p class="big-ok">All ${done.toLocaleString()} power cuts survived, in ${secs} s.</p>${stats()}<p class="hint">${tx.toLocaleString()} transactions in total; recovery replayed ${replayed.toLocaleString()} log frames.${bugs ? " This bug survived this batch: rare ones need more power cuts." : ""}</p>`;
      }
    };
    step();
  });

  // ---------------------------------------------------------------- wiring
  function showTab(name) {
    document.querySelectorAll(".tabs button").forEach((b) => b.setAttribute("aria-selected", String(b.dataset.tab === name)));
    $("play").hidden = name !== "play";
    $("torture").hidden = name !== "torture";
  }
  document.querySelectorAll(".tabs button").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab)));
  $("run").addEventListener("click", () => run($("sql").value));
  $("sql").addEventListener("keydown", (e) => { if ((e.ctrlKey || e.metaKey) && e.key === "Enter") { e.preventDefault(); run($("sql").value); } });
  $("plug").addEventListener("click", () => pullPlug(false));
  $("plugCommit").addEventListener("click", () => pullPlug(true));
  $("checkpoint").addEventListener("click", () => {
    plugged = null;
    x.g_checkpoint();
    const r = out();
    $("results").innerHTML = `<div class="res"><p class="msg">Checkpoint: ${r.frames} log page${r.frames === 1 ? "" : "s"} copied into the database file, then the log was emptied.</p></div>`;
    refresh();
  });
  $("tableSel").addEventListener("change", (e) => { table = e.target.value; lastKey = null; refresh(); });

  load().then(() => {
    seed();
    const r = exec("SELECT name FROM users WHERE id = 42");
    user42 = r.results[0].rows[0][0];
    const chips = $("chips");
    examples().forEach(([label, sql]) => {
      const b = document.createElement("button");
      b.className = "chip";
      b.textContent = label;
      b.addEventListener("click", () => { if (sql !== "__insert__") $("sql").value = sql; run(sql); });
      chips.appendChild(b);
    });
    $("sql").value = "SELECT * FROM users WHERE id = 42;";
    run($("sql").value);
  }).catch((e) => {
    console.error(e);
    $("status").innerHTML = `<h1>The database couldn't load.</h1><p>${esc(e.message || e)}. If you opened this file directly, try the single-file version, granite.html.</p>`;
  });
})();
