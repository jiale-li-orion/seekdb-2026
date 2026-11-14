#!/usr/bin/env python3
"""Validate seekdb file-table parser, dynamic schema and runtime wiring.

Checks integration invariants in committed source; native mysqltest remains the
end-to-end oracle and requires a freshly built seekdb executable.
"""
from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")

variables = json.loads(read("src/share/system_variable/ob_system_variable_init.json"))
entry = variables["enable_file_sql"]
assert entry["id"] == 10754 and entry["default_value"] == "0"
assert "SESSION" in entry["flags"] and "INFLUENCE_PLAN" in entry["flags"]

enum = read("src/query/api/query/parser/ob_item_type.h")
objit_enum = read("src/objit/include/objit/common/ob_item_type.h")
for source in (enum, objit_enum):
    assert "T_FILE_SQL_TABLE_EXPRESSION = 4921" in source

grammar = read("src/sql/parser/sql_parser_mysql_mode.y")
keywords = read("src/sql/parser/non_reserved_keywords_mysql_mode.c")
for token in ("READ_CSV", "READ_JSON", "READ_NDJSON", "READ_PARQUET", "GLOB"):
    assert token in grammar and token in keywords
assert "file_sql_table_factor" in grammar

resolver = read("src/sql/resolver/dml/ob_dml_resolver.cpp")
assert "resolve_file_sql_table_item" in resolver
assert "file_sql::Reader reader" in resolver and "Reader::Format::glob" in resolver
assert "file_sql_names_.push_back" in resolver
assert "resolve_function_table_column_item(*item, columns)" in resolver
assert "get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL" in resolver
assert "resolve_file_sql_options" in resolver and "file_sql_header_ = file_sql_header" in resolver

stmt = read("src/sql/resolver/dml/ob_dml_stmt.h")
for name in ("file_sql_format_", "file_sql_path_", "file_sql_names_", "file_sql_types_"):
    assert name in stmt
assert "file_sql_names_.assign(other.file_sql_names_)" in read("src/sql/resolver/dml/ob_dml_stmt.cpp")
assert "file_sql_header_ = other.file_sql_header_" in read("src/sql/resolver/dml/ob_dml_stmt.cpp")

logical = read("src/sql/optimizer/ob_log_function_table.h")
plan = read("src/sql/optimizer/ob_log_plan.cpp")
assert "file_sql_format_" in logical and "set_file_sql_format" in plan
assert "set_file_sql_header" in plan and "file_sql_header_" in logical
cg = read("src/sql/code_generator/ob_static_engine_cg.cpp")
assert "file_column_indices_.push_back" in cg
assert "spec.file_sql_header_ = op.get_file_sql_header()" in cg
spec = read("src/sql/engine/basic/ob_function_table_op.h")
assert "file_column_indices_" in spec and "file_sql_format_" in spec
assert "file_sql_header_" in spec
runtime = read("src/sql/engine/basic/ob_function_table_op.cpp")
assert "inner_get_next_row_file" in runtime and "file_reader_->next" in runtime
assert "SYS_VAR_ENABLE_FILE_SQL" in runtime and "get_secure_file_priv" in runtime
assert "release_file_reader" in runtime
assert "file_sql_format_" in read("src/sql/engine/basic/ob_function_table_op.cpp")
assert "ob_file_scan_core.cpp" in read("src/sql/sql_source_inventory.bzl")
assert "ob_file_scan_core.h" in read("src/sql/sql_header_inventory.bzl")

for path in (
    "src/share/system_variable/ob_sys_var_class_type.h",
    "src/share/system_variable/ob_system_variable_init.cpp",
    "src/share/system_variable/ob_sys_var_meta.cpp",
    "src/sql/session/ob_system_variable_factory.cpp",
):
    assert "SYS_VAR_ENABLE_FILE_SQL" in read(path), path

print("PASS: parser, scope gate, system-variable registry, dynamic column and physical file scan wiring")
