#!/usr/bin/env python3
"""Run SQL-level CSV / JSONL / GLOB assertions against a native seekdb server.

Usage (after building and starting seekdb):
  python3 -m pip install --user pymysql
  python3 tools/competition/verify_file_sql_integration.py --port 2881

Uses only the checked-in file_sql fixtures and restores enable_file_sql=OFF.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import pymysql


def run(port: int, user: str, password: str, fixture: Path) -> None:
    fixture = fixture.resolve()
    if not fixture.is_dir():
        raise RuntimeError(f"Missing fixture directory: {fixture}")
    connection = pymysql.connect(
        host="127.0.0.1", port=port, user=user, password=password,
        charset="utf8mb4", autocommit=True, connect_timeout=10,
        read_timeout=60,
    )
    def path(name: str) -> str:
        return (fixture / name).as_posix().replace("'", "''")

    count = 0
    try:
        with connection.cursor() as cursor:
            cursor.execute("SELECT @@session.enable_file_sql")
            assert cursor.fetchone()[0] in (0, "0", False)
            count += 1
            try:
                cursor.execute(f"SELECT COUNT(*) FROM read_csv('{path('basic.csv')}')")
                raise AssertionError("Disabled file SQL query unexpectedly succeeded")
            except pymysql.err.MySQLError:
                count += 1

            cursor.execute("SET SESSION enable_file_sql=ON")
            cursor.execute(f"SELECT id, name, score, active, day, created, "
                           f"REPLACE(note, CHAR(10), '<NL>') AS note, id__2 "
                           f"FROM read_csv('{path('basic.csv')}') f ORDER BY id")
            rows = cursor.fetchall()
            assert len(rows) == 3, rows
            assert [(r[0], r[1], r[7]) for r in rows] == [
                (1, 'Alice', 101), (2, 'Bob', 102), (3, 'Chen', 103)
            ], rows
            assert str(rows[0][6]) == 'hello,<NL>world', rows
            assert rows[1][4] is None and rows[1][5] is None, rows
            assert rows[2][6] == 'say "hi"', rows
            count += 1

            cursor.execute(f"SELECT COUNT(*), SUM(id), ROUND(AVG(score),2), SUM(active) "
                           f"FROM read_csv('{path('basic.csv')}', header=true, sample_size=-1)")
            summary = cursor.fetchone()
            assert int(summary[0]) == 3 and int(summary[1]) == 6, summary
            assert abs(float(summary[2]) - 91.92) < 0.01, summary
            assert int(summary[3]) == 2, summary
            count += 1

            cursor.execute(f"SELECT id, name, score, active, day, note "
                           f"FROM read_json('{path('basic.jsonl')}', format='newline_delimited') "
                           f"ORDER BY id")
            records = cursor.fetchall()
            assert len(records) == 3 and [r[0] for r in records] == [1, 2, 3], records
            assert records[1][2] is None and records[0][4] is None, records
            assert records[2][5] == 'ok', records
            count += 1

            wildcard = path('*')
            cursor.execute(f"SELECT SUBSTRING_INDEX(file, '/', -1) "
                           f"FROM glob('{wildcard}') ORDER BY file")
            actual_files = [r[0] for r in cursor.fetchall()]
            expected_files = sorted(p.name for p in fixture.iterdir() if p.is_file())
            assert actual_files == expected_files, (actual_files, expected_files)
            count += 1

            try:
                cursor.execute(f"SELECT * FROM read_csv('{path('basic.csv')}', sample_size=10)")
                raise AssertionError("Unsupported sampling unexpectedly succeeded")
            except pymysql.err.MySQLError:
                count += 1

            cursor.execute("SET SESSION enable_file_sql=OFF")
            cursor.execute("SELECT @@session.enable_file_sql")
            assert cursor.fetchone()[0] in (0, "0", False)
            count += 1
    finally:
        connection.close()
    print(f"PASS: {count} live SQL checks (disabled-by-default, CSV, aggregate, "
          "JSONL, GLOB, invalid options, session isolation)")


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=2881)
    parser.add_argument('--user', default='root')
    parser.add_argument('--password', default='')
    parser.add_argument('--fixture', type=Path, default=(
        Path(__file__).resolve().parents[2] /
        'tools/deploy/mysql_test/test_suite/file_sql/data'))
    args = parser.parse_args()
    run(args.port, args.user, args.password, args.fixture)
