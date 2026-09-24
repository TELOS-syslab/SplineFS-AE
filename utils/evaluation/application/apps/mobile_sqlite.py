#!/usr/bin/env python3
"""A4: per-app SQLite databases and media caches, as in a phone's /data/data:
many app directories, each with a WAL-mode database and small cache files.

  build <root> <n_apps> <rows_per_db> <cache_files>
      create the per-app tree with real sqlite3 databases, populated.
  run <root>
      the cold metadata-bound stage: open every app DB and run a real
      query that touches the schema (what a launcher/backup/indexer does
      when it wakes every app), and stat every media-cache file.  Prints
      'seconds=<wall>'.

"""
import os, sys, time, sqlite3, random


def build(root, n_apps, rows, cache_files):
    random.seed(1)
    for a in range(n_apps):
        d = os.path.join(root, f"com.app{a:05d}", "databases")
        cache = os.path.join(root, f"com.app{a:05d}", "cache")
        os.makedirs(d, exist_ok=True)
        os.makedirs(cache, exist_ok=True)
        db = os.path.join(d, "app.db")
        con = sqlite3.connect(db)
        con.execute("PRAGMA journal_mode=WAL")
        con.execute("CREATE TABLE IF NOT EXISTS msgs("
                    "id INTEGER PRIMARY KEY, ts INTEGER, who TEXT, body TEXT)")
        con.execute("CREATE TABLE IF NOT EXISTS kv("
                    "k TEXT PRIMARY KEY, v TEXT)")
        con.executemany("INSERT INTO msgs(ts,who,body) VALUES(?,?,?)",
                        [(random.randint(0, 1 << 30), f"u{random.randint(0,99)}",
                          "x" * 64) for _ in range(rows)])
        con.commit()
        con.execute("PRAGMA wal_checkpoint(PASSIVE)")
        con.close()
        for c in range(cache_files):
            with open(os.path.join(cache, f"img_{c:04d}.bin"), "wb") as f:
                f.write(b"\0" * 256)
    print(f"built {n_apps} apps", file=sys.stderr)


def run(root):
    t0 = time.time()
    nopen = nrow = ncache = 0
    for app in sorted(os.scandir(root), key=lambda e: e.name):
        if not app.is_dir():
            continue
        db = os.path.join(app.path, "databases", "app.db")
        if os.path.exists(db):
            # open read-only/immutable: a backup/launcher/indexer scanning
            # per-app DBs does not write, so no -wal/-shm is created (which
            # would mutate the tree); this is the cold metadata-bound read.
            con = sqlite3.connect(f"file:{db}?immutable=1", uri=True)
            try:
                cur = con.execute("SELECT count(*) FROM msgs")
                nrow += cur.fetchone()[0]
                con.execute("SELECT who, count(*) FROM msgs GROUP BY who")
            except sqlite3.Error:
                pass
            con.close()
            nopen += 1
        cdir = os.path.join(app.path, "cache")
        if os.path.isdir(cdir):
            for e in os.scandir(cdir):
                os.stat(e.path)                        # stat every cache file
                ncache += 1
    dt = time.time() - t0
    print(f"dbs={nopen} rows={nrow} cache={ncache} seconds={dt:.3f}", file=sys.stderr)
    print(f"seconds={dt:.3f}")


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "build":
        build(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]))
    elif cmd == "run":
        run(sys.argv[2])
