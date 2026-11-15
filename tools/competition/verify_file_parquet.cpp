#include "sql/engine/basic/file_sql/ob_file_scan_core.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oceanbase::sql::file_sql;
namespace fs = std::filesystem;

static void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

static void official_fixture(const fs::path &file) {
  Reader reader(file.string(), Reader::Format::parquet, file.parent_path().string());
  const auto schema = reader.infer();
  check(schema.columns.size() == 1 && schema.columns[0].name == "c1", "Parquet schema");
  check(schema.columns[0].kind == Kind::bigint, "Parquet integer type");
  std::vector<Cell> row;
  int64_t sum = 0;
  for (int64_t i = 0; i < 7; ++i) {
    check(reader.next(row) && row.size() == 1 && !row[0].is_null, "Parquet row count");
    check(row[0].value == std::to_string(i), "Parquet row value");
    sum += std::stoll(row[0].value);
  }
  check(sum == 21 && !reader.next(row), "Parquet SUM and EOF");
  reader.rewind();
  check(reader.next(row) && row[0].value == "0", "Parquet rewind");
  bool denied = false;
  try {
    Reader outside(file.string(), Reader::Format::parquet,
                   (file.parent_path() / "not-a-directory").string());
    outside.infer();
  } catch (const fs::filesystem_error &) { denied = true; }
  check(denied, "Parquet must reject an invalid secure root");
}

static void mixed_fixture(const fs::path &file) {
  Reader reader(file.string(), Reader::Format::parquet, file.parent_path().string());
  const auto schema = reader.infer();
  check(schema.columns.size() == 5, "Parquet mixed schema width");
  check(schema.columns[0].kind == Kind::bigint &&
        schema.columns[1].kind == Kind::varchar &&
        schema.columns[2].kind == Kind::boolean &&
        schema.columns[3].kind == Kind::double_value &&
        schema.columns[4].kind == Kind::date, "Parquet mixed column types");
  const std::vector<std::vector<std::string>> expected = {
    {"0", "AA", "true", "12.5", "2026-11-01"},
    {"1", "BB", "false", "NULL", "NULL"},
    {"2", "NULL", "NULL", "13.5", "2026-11-03"},
    {"3", "BB", "true", "22", "2026-11-04"},
    {"4", "AA", "false", "-4.25", "2026-11-05"},
    {"5", "DD", "false", "0", "2026-11-06"},
    {"6", "NULL", "true", "NULL", "NULL"},
    {"7", "AA", "true", "100.5", "2026-11-08"},
  };
  std::vector<Cell> row;
  for (const auto &values : expected) {
    check(reader.next(row) && row.size() == values.size(), "Parquet mixed row missing");
    for (size_t i = 0; i < values.size(); ++i) {
      check((row[i].is_null ? "NULL" : row[i].value) == values[i], "Parquet mixed row value");
    }
  }
  check(!reader.next(row), "Parquet mixed row overflow");
}

int main(int argc, char **argv) {
  if (argc < 2) throw std::runtime_error("usage: verify_file_parquet official.parquet [mixed.parquet ...]");
  official_fixture(fs::absolute(argv[1]));
  for (int i = 2; i < argc; ++i) mixed_fixture(fs::absolute(argv[i]));
  std::cout << "PASS: Parquet official fixture, nullable flat columns, dictionary/RLE/plain, "
               "row groups and secure root" << std::endl;
}
