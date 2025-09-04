#!/usr/bin/env python3
"""
Batching query streamer for commonswiki_p → fastcci_build_db

- Finds MAX(page_id)
- Queries in [start, start+batch) windows
- Streams rows as TSV into fastcci_build_db's stdin
"""

import argparse
import os
import sys
import math
import signal
from contextlib import closing

import pymysql
from pymysql.cursors import SSCursor     # server-side cursor for streaming
import subprocess


def parse_args():
    p = argparse.ArgumentParser(description="Stream batched DB results into fastcci_build_db")
    p.add_argument("--defaults-file", default=os.path.expanduser("~/replica.my.cnf"),
                   help="MySQL defaults file (same as --defaults-file for mysql client)")
    p.add_argument("--host", default="commonswiki.analytics.db.svc.eqiad.wmflabs")
    p.add_argument("--db", default="commonswiki_p")
    p.add_argument("--batch-size", type=int, default=100_000,
                   help="page_id window size")
    p.add_argument("--fetch-size", type=int, default=10_000,
                   help="rows per fetchmany() from server (streaming)")
    p.add_argument("--start-id", type=int, default=0,
                   help="optional lower bound for page_id (inclusive)")
    p.add_argument("--end-id", type=int, default=None,
                   help="optional upper bound for page_id (exclusive); default = MAX(page_id)+1")
    p.add_argument("--dry-run", action="store_true",
                   help="only compute ranges and print summary, don't run query/pipe")
    return p.parse_args()


def connect(args):
    # PyMySQL can read credentials from a my.cnf-style file
    # e.g.:
    # [client]
    # user=...
    # password=...
    # host=...
    # (explicit host from args still applied)
    return pymysql.connect(
        host=args.host,
        db=args.db,
        read_default_file=args.defaults_file,
        charset="utf8mb4",
        autocommit=True,         # SELECTs don't need transactions here
        cursorclass=SSCursor     # server-side cursor for streaming
    )


def get_max_page_id(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT MAX(page_id) FROM page;")
        row = cur.fetchone()
        return int(row[0]) if row and row[0] is not None else 0


def stream_batch(conn, lo, hi, fetch_size, sink_stdin):
    """
    Execute one batched SELECT and stream rows (TSV) to sink_stdin.
    """
    sql = (
        "SELECT /* SLOW_OK */ cl_from, page_id, cl_type "
        "FROM categorylinks "
        "JOIN page ON page_id >= %s AND page_id < %s "
        "  AND page_namespace = 14 "
        "  AND page_title = cl_to "
        "WHERE cl_type != 'page' "
        "ORDER BY page_id;"
    )
    # Note: the range predicate lives in the JOIN to keep optimizer behavior
    # close to your original implicit join (but explicit JOIN is clearer).

    with conn.cursor() as cur:
        cur.execute(sql, (lo, hi))
        total = 0
        while True:
            rows = cur.fetchmany(fetch_size)
            if not rows:
                break
            # Stream as TSV lines (no header), matching mysql --batch --silent
            out = []
            for cl_from, page_id, cl_type in rows:
                out.append(f"{cl_from}\t{page_id}\t{cl_type}\n")
            sink_stdin.write("".join(out).encode("utf-8", errors="strict"))
            sink_stdin.flush()
            total += len(rows)
        return total


def main():
    args = parse_args()

    # Start fastcci_build_db as a child process and write to its stdin
    if args.dry_run:
        fastcci = None
        sink = sys.stdout.buffer  # dry run: write to stdout
    else:
        try:
            fastcci = subprocess.Popen(
                ["fastcci_build_db"],
                stdin=subprocess.PIPE,
                stdout=sys.stderr,   # forward any messages to stderr
                stderr=sys.stderr,
                bufsize=0
            )
            sink = fastcci.stdin
        except FileNotFoundError:
            print("ERROR: fastcci_build_db not found in PATH", file=sys.stderr)
            return 1

    # Ensure child is terminated on SIGINT/SIGTERM
    def _terminate_child(*_):
        try:
            if fastcci and fastcci.poll() is None:
                fastcci.terminate()
        finally:
            sys.exit(1)

    signal.signal(signal.SIGINT, _terminate_child)
    signal.signal(signal.SIGTERM, _terminate_child)

    # Connect and determine range
    with closing(connect(args)) as conn:
        max_id = get_max_page_id(conn) if args.end_id is None else args.end_id - 1
        start_id = max(args.start_id, 0)
        end_excl = (max_id + 1) if args.end_id is None else args.end_id

        if end_excl <= start_id:
            print("Nothing to do: end <= start", file=sys.stderr)
            if fastcci:
                fastcci.stdin.close()
                return fastcci.wait()
            return 0

        total_batches = math.ceil((end_excl - start_id) / args.batch_size)
        print(f"Streaming page_id in [{start_id}, {end_excl}) "
              f"in {total_batches} batches (size={args.batch_size}), fetch_size={args.fetch_size}",
              file=sys.stderr)

        grand_total = 0
        for i, lo in enumerate(range(start_id, end_excl, args.batch_size), start=1):
            hi = min(lo + args.batch_size, end_excl)
            print(f"[{i}/{total_batches}] batch: [{lo}, {hi})", file=sys.stderr)
            sent = stream_batch(conn, lo, hi, args.fetch_size, sink)
            grand_total += sent
            print(f"[{i}/{total_batches}] rows streamed: {sent} (cumulative {grand_total})",
                  file=sys.stderr)

    # Close stdin so fastcci_build_db knows input is complete
    if not args.dry_run and fastcci and fastcci.stdin:
        fastcci.stdin.close()
        rc = fastcci.wait()
        if rc != 0:
            print(f"fastcci_build_db exited with {rc}", file=sys.stderr)
            return rc

    print("Done.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

