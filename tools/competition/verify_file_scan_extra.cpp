#include "sql/engine/basic/file_sql/ob_file_scan_core.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace oceanbase::sql::file_sql;
int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path root = argv[1];
  std::filesystem::create_directories(root);
  const auto csv_path = root / "cases.csv";
  { std::ofstream f(csv_path); f << "k,v,allnull\r\n1,\"\",\r\n2,,\r\n3,\"line1\nline2\",\r\n"; }
  Reader csv(csv_path.string(), Reader::Format::csv);
  Schema s = csv.infer();
  assert(s.columns.size() == 3 && s.columns[2].nullable && s.columns[2].kind == Kind::null_value);
  std::vector<Cell> r;
  assert(csv.next(r) && !r[1].is_null && r[1].value.empty());
  assert(csv.next(r) && r[1].is_null);
  assert(csv.next(r) && r[1].value == "line1\nline2");
  assert(!csv.next(r));
  csv.rewind();
  { std::ofstream f(csv_path, std::ios::app); f << "4,x,\n"; }
  bool detected = false;
  try { csv.next(r); } catch (const std::runtime_error&) { detected = true; }
  assert(detected);
  { std::ofstream f(csv_path); f << "x,y\n1,\"unterminated\n"; }
  detected = false;
  try { Reader malformed(csv_path.string(), Reader::Format::csv); malformed.infer(); } catch (const std::runtime_error&) { detected = true; }
  assert(detected);
  const auto json_path = root / "nested.jsonl";
  { std::ofstream f(json_path); f << "{\"x\":1}\n{\"x\":2,\"y\":{\"z\":3}}\n"; }
  detected = false;
  try { Reader nested(json_path.string(), Reader::Format::jsonl); nested.infer(); } catch (const std::runtime_error&) { detected = true; }
  assert(detected);
  { std::ofstream f(json_path); f << "{\"x\":1}\n{\"x\":2,\"x\":3}\n"; }
  detected = false;
  try { Reader duplicate(json_path.string(), Reader::Format::jsonl); duplicate.infer(); } catch (const std::runtime_error&) { detected = true; }
  assert(detected);
  std::cout << "PASS: quoted empty, NULL, malformed CSV/JSONL, rescan, file mutation\n";
}
