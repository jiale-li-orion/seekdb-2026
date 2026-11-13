#include "sql/engine/basic/file_sql/ob_file_scan_core.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <stdexcept>
using namespace oceanbase::sql::file_sql;
int main(int argc, char** argv) {
 assert(argc == 3);
 Reader csv(argv[1], Reader::Format::csv, std::filesystem::path(argv[1]).parent_path().string());
 Schema s = csv.infer();
 assert(s.columns.size() == 8);
 assert(s.columns[0].name == "id" && s.columns[7].name == "id__2");
 assert(s.columns[0].kind == Kind::bigint && s.columns[2].kind == Kind::double_value);
 assert(s.columns[3].kind == Kind::boolean && s.columns[4].kind == Kind::date);
 assert(s.columns[5].kind == Kind::datetime && s.columns[4].nullable);
 std::vector<Cell> row;
 assert(csv.next(row) && row[0].value == "1" && row[6].value == "hello,\nworld");
 assert(csv.next(row) && row[4].is_null && row[5].is_null);
 assert(csv.next(row) && row[5].value == "2026-08-03 12:13:14" && row[6].value == "say \"hi\"");
 assert(!csv.next(row));
 csv.rewind(); assert(csv.next(row) && row[0].value == "1");
 Reader json(argv[2], Reader::Format::jsonl, std::filesystem::path(argv[2]).parent_path().string());
 s = json.infer(); assert(s.columns.size() == 6);
 assert(s.columns[0].name == "id" && s.columns[2].name == "score");
 assert(s.columns[2].kind == Kind::double_value && s.columns[2].nullable);
 assert(json.next(row) && row[0].value == "1" && row[4].is_null);
 assert(json.next(row) && row[2].is_null && row[4].value == "2026-08-02");
 assert(json.next(row) && row[5].value == "ok" && row[2].value == "90");
 assert(!json.next(row));
 assert(infer_kind({"9223372036854775807", false}) == Kind::bigint);
 assert(infer_kind({"9223372036854775808", false}) == Kind::double_value);
 assert(infer_kind({"2026-02-29", false}) == Kind::varchar);
 assert(infer_kind({"2024-02-29", false}) == Kind::date);
 assert(widen_kind(Kind::date, Kind::datetime) == Kind::datetime);
 assert(widen_kind(Kind::boolean, Kind::bigint) == Kind::varchar);
 std::cout << "PASS: CSV and JSONL schema inference, streaming and typed cells\n";
}
