#include "sql/engine/basic/file_sql/ob_file_scan_core.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
using oceanbase::sql::file_sql::Cell;
using oceanbase::sql::file_sql::Reader;
using oceanbase::sql::file_sql::Kind;
namespace fs = std::filesystem;
int main(int argc, char** argv) {
  assert(argc == 2);
  const fs::path root(argv[1]);
  fs::create_directories(root);
  fs::create_directories(root / "inside");
  for (const auto& n : {"a.csv", "b.csv", "c.txt", "d.csv", "01.jsonl"}) {
    std::ofstream f(root / "inside" / n); f << "fixture";
  }
  const fs::path outside = root.parent_path() / "file_glob_external.csv";
  { std::ofstream f(outside); f << "secret"; }
  fs::remove(root / "inside" / "external.csv");
  fs::create_symlink(outside, root / "inside" / "external.csv");
  Reader reader((root / "inside" / "*.csv").string(), Reader::Format::glob, root.string());
  auto schema = reader.infer();
  assert(schema.columns.size() == 1);
  assert(schema.columns[0].name == "file" && schema.columns[0].kind == Kind::varchar);
  assert(!schema.columns[0].nullable);
  std::vector<Cell> row;
  assert(reader.next(row) && fs::path(row[0].value).filename() == "a.csv");
  assert(reader.next(row) && fs::path(row[0].value).filename() == "b.csv");
  assert(reader.next(row) && fs::path(row[0].value).filename() == "d.csv");
  assert(!reader.next(row));
  assert(reader.current_record() == 3);
  reader.rewind();
  assert(reader.next(row) && fs::path(row[0].value).filename() == "a.csv");
  Reader empty((root / "inside" / "z*.csv").string(), Reader::Format::glob, root.string());
  assert(empty.infer().columns.size() == 1 && !empty.next(row));
  Reader one((root / "inside" / "?.txt").string(), Reader::Format::glob, root.string());
  assert(one.infer().columns.size() == 1);
  assert(one.next(row) && fs::path(row[0].value).filename() == "c.txt");
  assert(!one.next(row));
  bool denied = false;
  try {
    Reader blocked((outside.parent_path() / "*.csv").string(), Reader::Format::glob, (root / "inside").string());
    blocked.infer();
  } catch (const std::exception&) { denied = true; }
  assert(denied);
  fs::remove(root / "inside" / "external.csv");
  fs::remove(outside);
  std::cout << "PASS: glob filename patterns, sorted records, rewind, empty result, secure root and symlink exclusion\n";
}
