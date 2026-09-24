#!/usr/bin/env python3
"""A5: a cold DuckDB scan of a Hive-partitioned Parquet lake, which opens
every partition file before reading data.

  build <lake> <n_files> <rows>   write n_files small parquet files into
                                  year=/month=/day=/part-*.parquet partitions
  run   <lake>                    cold DuckDB scan over the whole lake
                                  (footer + metadata of every partition file)

Prints 'seconds=<wall>'.  pyarrow builds the lake; duckdb queries it (falls
back to a pyarrow.dataset scan if duckdb is unavailable)."""
import os, sys, time


def build(lake, n_files, rows):
    import pyarrow as pa, pyarrow.parquet as pq
    os.makedirs(lake, exist_ok=True)
    made = 0
    y = 2015
    while made < n_files:
        for m in range(1, 13):
            for d in range(1, 29):
                part = os.path.join(lake, f"year={y}", f"month={m:02d}", f"day={d:02d}")
                os.makedirs(part, exist_ok=True)
                # a few partition files per day (real lakes shard per ingest)
                for s in range(8):
                    if made >= n_files:
                        break
                    t = pa.table({"id": list(range(rows)),
                                  "v": [(made + i) % 997 for i in range(rows)]})
                    pq.write_table(t, os.path.join(part, f"part-{s}.parquet"))
                    made += 1
                if made >= n_files:
                    break
            if made >= n_files:
                break
        y += 1
    print(f"lake: {made} parquet files", file=sys.stderr)


def run(lake):
    t0 = time.time()
    try:
        import duckdb
        con = duckdb.connect()
        con.execute(f"SELECT count(*), sum(v) FROM "
                    f"read_parquet('{lake}/**/*.parquet', hive_partitioning=1)").fetchall()
        eng = "duckdb"
    except Exception:
        import pyarrow.dataset as ds
        d = ds.dataset(lake, format="parquet", partitioning="hive")
        d.count_rows()
        eng = "pyarrow"
    dt = time.time() - t0
    print(f"engine={eng} seconds={dt:.3f}", file=sys.stderr)
    print(f"seconds={dt:.3f}")


if __name__ == "__main__":
    if sys.argv[1] == "build":
        build(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    else:
        run(sys.argv[2])
