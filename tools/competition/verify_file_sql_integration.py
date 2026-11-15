#!/usr/bin/env python3
"""Execute real SQL assertions against a freshly built seekdb server.

Run with PYTHONPATH pointing at the isolated PyMySQL installation if needed:
  python3 tools/competition/verify_file_sql_integration.py --port 2881

The test copies tracked fixtures into a private temporary directory, exercises
file SQL and COPY, and removes only this directory when complete.
"""
from __future__ import annotations

import argparse
import base64
from pathlib import Path
import shutil
import tempfile

import pymysql


def run(port: int, user: str, password: str, fixture: Path) -> None:
    fixture = fixture.resolve()
    if not fixture.is_dir():
        raise RuntimeError(f"Missing fixture directory: {fixture}")

    with tempfile.TemporaryDirectory(prefix="seekdb-file-sql-check-") as temp:
        root = Path(temp).resolve()
        for filename in ("basic.csv", "basic.jsonl", "invalid.txt"):
            shutil.copyfile(fixture / filename, root / filename)
        (root / "simple.parquet").write_bytes(base64.b64decode(
            (fixture / "simple.parquet.b64").read_text()))

        def path(name: str) -> str:
            return (root / name).as_posix().replace("'", "''")

        connection = pymysql.connect(
            host="127.0.0.1", port=port, user=user, password=password,
            charset="utf8mb4", autocommit=True, connect_timeout=10,
            read_timeout=60,
        )
        checks = 0
        try:
            with connection.cursor() as cursor:
                cursor.execute("SELECT @@session.enable_file_sql")
                assert cursor.fetchone()[0] in (0, "0", False)
                checks += 1
                try:
                    cursor.execute(f"SELECT COUNT(*) FROM read_csv('{path('basic.csv')}')")
                    raise AssertionError("Disabled file SQL query unexpectedly succeeded")
                except pymysql.err.MySQLError:
                    checks += 1

                cursor.execute("SET SESSION enable_file_sql=ON")
                cursor.execute(f"SELECT id, name, score, active, day, created, "
                               f"REPLACE(note, CHAR(10), '<NL>') AS note, id__2 "
                               f"FROM read_csv('{path('basic.csv')}') f ORDER BY id")
                rows = cursor.fetchall()
                assert len(rows) == 3, rows
                assert [(int(r[0]), r[1], int(r[7])) for r in rows] == [
                    (1, 'Alice', 101), (2, 'Bob', 102), (3, 'Chen', 103)
                ], rows
                assert str(rows[0][6]) == 'hello,<NL>world', rows
                assert rows[1][4] is None and rows[1][5] is None, rows
                assert rows[2][6] == 'say "hi"', rows
                checks += 1

                cursor.execute(f"SELECT COUNT(*), SUM(id), ROUND(AVG(score),2), SUM(active) "
                               f"FROM read_csv('{path('basic.csv')}', header=true, sample_size=-1)")
                count, id_sum, avg, active_count = cursor.fetchone()
                assert int(count) == 3 and int(id_sum) == 6
                assert abs(float(avg) - 91.92) < 0.01 and int(active_count) == 2
                checks += 1

                cursor.execute(f"SELECT id, name, score, active, day, note "
                               f"FROM read_json('{path('basic.jsonl')}', format='newline_delimited') "
                               f"ORDER BY id")
                records = cursor.fetchall()
                assert len(records) == 3 and [int(r[0]) for r in records] == [1, 2, 3]
                assert records[1][2] is None and records[0][4] is None
                assert records[2][5] == 'ok'
                checks += 1

                cursor.execute(f"SELECT COUNT(*), MIN(c1), MAX(c1), SUM(c1) "
                               f"FROM read_parquet('{path('simple.parquet')}')")
                values = cursor.fetchone()
                assert tuple(map(int, values)) == (7, 0, 6, 21), values
                checks += 1

                cursor.execute(f"DESCRIBE SELECT * FROM read_csv('{path('basic.csv')}')")
                info = cursor.fetchall()
                assert len(info) == 8, info
                assert [(r[0], r[1], r[2]) for r in info] == [
                    ('id', 'BIGINT', 'NO'), ('name', 'VARCHAR', 'NO'),
                    ('score', 'DOUBLE', 'NO'), ('active', 'BOOLEAN', 'NO'),
                    ('day', 'DATE', 'YES'), ('created', 'DATETIME', 'YES'),
                    ('note', 'VARCHAR', 'NO'), ('id__2', 'BIGINT', 'NO')
                ], info
                assert all(r[3:] == (None, None, None) for r in info), info
                checks += 1

                cursor.execute(f"SELECT SUBSTRING_INDEX(file, '/', -1) "
                               f"FROM glob('{path('*')}') ORDER BY file")
                actual_files = [r[0] for r in cursor.fetchall()]
                assert actual_files == ['basic.csv', 'basic.jsonl', 'invalid.txt', 'simple.parquet'], actual_files
                checks += 1

                cursor.execute(f"COPY (SELECT id, name, score "
                               f"FROM read_csv('{path('basic.csv')}') ORDER BY id) "
                               f"TO '{path('export.csv')}' (FORMAT csv, HEADER, NULLSTR 'NULL')")
                assert (root / 'export.csv').is_file(), 'COPY failed to create export.csv'
                cursor.execute(f"SELECT id, name, score FROM read_csv('{path('export.csv')}') ORDER BY id")
                exported = cursor.fetchall()
                assert len(exported) == 3 and [int(r[0]) for r in exported] == [1, 2, 3], exported
                assert [r[1] for r in exported] == ['Alice', 'Bob', 'Chen'], exported
                checks += 1

                try:
                    cursor.execute(f"SELECT * FROM read_csv('{path('basic.csv')}', sample_size=10)")
                    raise AssertionError("Unsupported sampling unexpectedly succeeded")
                except pymysql.err.MySQLError:
                    checks += 1

                cursor.execute("SET SESSION enable_file_sql=OFF")
                cursor.execute("SELECT @@session.enable_file_sql")
                assert cursor.fetchone()[0] in (0, "0", False)
                checks += 1
        finally:
            connection.close()
        print(f"PASS: {checks} live SQL checks (CSV, JSONL, Parquet, DESCRIBE, "
              "GLOB, COPY, disabled-by-default and invalid options)")


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
