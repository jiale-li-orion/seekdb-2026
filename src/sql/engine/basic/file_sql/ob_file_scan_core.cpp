/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "ob_file_scan_core.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include <rapidjson/document.h>

namespace oceanbase::sql::file_sql {
namespace {

constexpr std::size_t MAX_RECORD_BYTES = 16 * 1024 * 1024;
constexpr std::size_t MAX_COLUMNS = 4096;

bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

bool digits(const std::string& s, std::size_t at, std::size_t n) {
  if (at + n > s.size()) return false;
  for (std::size_t i = 0; i < n; ++i) if (!std::isdigit(static_cast<unsigned char>(s[at + i]))) return false;
  return true;
}

int number(const std::string& s, std::size_t at, std::size_t n) {
  int value = 0;
  for (std::size_t i = 0; i < n; ++i) value = value * 10 + s[at + i] - '0';
  return value;
}

bool date_at(const std::string& s) {
  if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !digits(s, 0, 4) || !digits(s, 5, 2) || !digits(s, 8, 2)) return false;
  const int year = number(s, 0, 4);
  const int month = number(s, 5, 2);
  const int day = number(s, 8, 2);
  static constexpr int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return year >= 1 && month >= 1 && month <= 12 && day >= 1 && day <= days[month] + (month == 2 && leap(year));
}

bool datetime_at(const std::string& s) {
  if (s.size() != 19 || !date_at(s) || (s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':' ||
      !digits(s, 11, 2) || !digits(s, 14, 2) || !digits(s, 17, 2)) return false;
  return number(s, 11, 2) <= 23 && number(s, 14, 2) <= 59 && number(s, 17, 2) <= 59;
}

std::string normalized_datetime(std::string value) {
  if (value.size() == 19 && value[10] == 'T') value[10] = ' ';
  return value;
}

bool path_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
  auto parent = root.begin();
  auto element = candidate.begin();
  for (; parent != root.end(); ++parent, ++element) {
    if (element == candidate.end() || *parent != *element) return false;
  }
  return true;
}

// Linear-space glob matcher for filenames; the SQL glob table function
// deliberately accepts a wildcard in the last path component only.
bool glob_matches(const std::string& pattern, const std::string& name) {
  std::size_t i = 0;
  std::size_t j = 0;
  std::size_t star = std::string::npos;
  std::size_t match = 0;
  while (j < name.size()) {
    if (i < pattern.size() && (pattern[i] == '?' || pattern[i] == name[j])) {
      ++i;
      ++j;
    } else if (i < pattern.size() && pattern[i] == '*') {
      star = i++;
      match = j;
    } else if (star != std::string::npos) {
      i = star + 1;
      j = ++match;
    } else {
      return false;
    }
  }
  while (i < pattern.size() && pattern[i] == '*') ++i;
  return i == pattern.size();
}

std::string deduplicate(std::string name, std::unordered_set<std::string>& used) {
  if (name.empty()) name = "column";
  if (used.insert(name).second) return name;
  std::size_t suffix = 2;
  while (!used.insert(name + "__" + std::to_string(suffix)).second) ++suffix;
  return name + "__" + std::to_string(suffix);
}

Cell json_cell(const rapidjson::Value& value) {
  if (value.IsNull()) return {};
  if (value.IsBool()) return {value.GetBool() ? "true" : "false", false};
  if (value.IsString()) return {std::string(value.GetString(), value.GetStringLength()), false};
  if (value.IsInt64()) return {std::to_string(value.GetInt64()), false};
  if (value.IsUint64()) return {std::to_string(value.GetUint64()), false};
  if (value.IsNumber()) return {std::to_string(value.GetDouble()), false};
  throw std::runtime_error("nested JSON objects and arrays are not supported");
}

Kind json_kind(const rapidjson::Value& value) {
  if (value.IsNull()) return Kind::null_value;
  if (value.IsBool()) return Kind::boolean;
  if (value.IsInt64()) return Kind::bigint;
  if (value.IsUint64()) return value.GetUint64() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? Kind::bigint : Kind::double_value;
  if (value.IsNumber()) return Kind::double_value;
  if (value.IsString()) return infer_kind({std::string(value.GetString(), value.GetStringLength()), false});
  throw std::runtime_error("nested JSON objects and arrays are not supported");
}

} // namespace

Kind infer_kind(const Cell& cell) {
  if (cell.is_null) return Kind::null_value;
  if (cell.value.empty()) return Kind::varchar;
  const std::string& value = cell.value;
  if (value == "true" || value == "false") return Kind::boolean;
  int64_t integer = 0;
  const auto [next, err] = std::from_chars(value.data(), value.data() + value.size(), integer);
  if (err == std::errc{} && next == value.data() + value.size()) return Kind::bigint;
  char* end = nullptr;
  const double real = std::strtod(value.c_str(), &end);
  if (end == value.c_str() + value.size() && end != value.c_str() && std::isfinite(real)) return Kind::double_value;
  if (value.size() == 10 && date_at(value)) return Kind::date;
  if (datetime_at(value)) return Kind::datetime;
  return Kind::varchar;
}

Kind widen_kind(Kind left, Kind right) {
  if (left == Kind::null_value) return right;
  if (right == Kind::null_value || right == left) return left;
  if ((left == Kind::bigint && right == Kind::double_value) || (left == Kind::double_value && right == Kind::bigint)) return Kind::double_value;
  if ((left == Kind::date && right == Kind::datetime) || (left == Kind::datetime && right == Kind::date)) return Kind::datetime;
  return Kind::varchar;
}

const char* kind_name(Kind kind) {
  switch (kind) {
    case Kind::null_value: return "NULL";
    case Kind::boolean: return "BOOLEAN";
    case Kind::bigint: return "BIGINT";
    case Kind::double_value: return "DOUBLE";
    case Kind::date: return "DATE";
    case Kind::datetime: return "DATETIME";
    case Kind::varchar: return "VARCHAR";
  }
  return "VARCHAR";
}

Reader::Reader(std::string path, Format format, std::string secure_root, bool header)
    : path_(std::move(path)), secure_root_(std::move(secure_root)),
      format_(format), header_(header) {
  if (secure_root_.empty()) throw std::invalid_argument("secure_file_priv root required");
}

void Reader::reset_stream() {
  {
    // Canonicalize both paths and compare components, not string prefixes.
    // A sibling named /allowed-evil must never inherit /allowed privileges.
    const auto root = std::filesystem::canonical(secure_root_);
    const auto path = std::filesystem::canonical(path_);
    auto root_it = root.begin();
    auto path_it = path.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
      if (path_it == path.end() || *root_it != *path_it) {
        throw std::runtime_error("file path escapes secure_file_priv");
      }
    }
    if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error("source must be a regular file");
    }
  }
  stream_.close();
  stream_.clear();
  stream_.open(path_, std::ios::binary);
  if (!stream_.good()) throw std::runtime_error("unable to open input file");
  record_number_ = 0;
}

bool Reader::next_csv(std::vector<Cell>& row) {
  row.clear();
  std::string field;
  enum class State { start, plain, quoted, quote_end } state = State::start;
  bool consumed = false;
  bool quoted_field = false;
  std::size_t bytes = 0;
  const auto push_field = [&]() {
    if (row.size() >= MAX_COLUMNS) throw std::runtime_error("too many CSV columns");
    row.push_back({field, field.empty() && !quoted_field});
    field.clear();
    quoted_field = false;
  };
  for (;;) {
    const int raw = stream_.get();
    if (raw == std::char_traits<char>::eof()) {
      if (!consumed) return false;
      if (state == State::quoted) throw std::runtime_error("unclosed CSV quote at record " + std::to_string(record_number_ + 1));
      push_field();
      ++record_number_;
      return true;
    }
    consumed = true;
    if (++bytes > MAX_RECORD_BYTES) throw std::runtime_error("CSV record exceeds memory limit");
    const char c = static_cast<char>(raw);
    if (state == State::quoted) {
      if (c == '"') state = State::quote_end;
      else field.push_back(c);
    } else if (state == State::quote_end) {
      if (c == '"') { field.push_back('"'); state = State::quoted; }
      else if (c == ',') { push_field(); state = State::start; }
      else if (c == '\n' || c == '\r') {
        if (c == '\r' && stream_.peek() == '\n') stream_.get();
        push_field(); ++record_number_; return true;
      } else throw std::runtime_error("unexpected characters after CSV quote");
    } else if (c == ',' ) { push_field(); state = State::start; }
    else if (c == '\n' || c == '\r') {
      if (c == '\r' && stream_.peek() == '\n') stream_.get();
      push_field(); ++record_number_; return true;
    } else if (c == '"') {
      if (state != State::start) throw std::runtime_error("unexpected CSV quote");
      state = State::quoted;
      quoted_field = true;
    } else { field.push_back(c); state = State::plain; }
  }
}

bool Reader::next_jsonl(std::vector<Cell>& row) {
  row.assign(jsonl_names_.size(), Cell{});
  std::string line;
  for (;;) {
    if (!std::getline(stream_, line)) return false;
    ++record_number_;
    if (line.size() > MAX_RECORD_BYTES) throw std::runtime_error("JSONL record exceeds memory limit");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) break;
  }
  rapidjson::Document document;
  document.Parse(line.c_str());
  if (document.HasParseError() || !document.IsObject()) throw std::runtime_error("invalid JSONL object at record " + std::to_string(record_number_));
  std::unordered_set<std::string> observed;
  for (auto it = document.MemberBegin(); it != document.MemberEnd(); ++it) {
    std::string name(it->name.GetString(), it->name.GetStringLength());
    if (!observed.insert(name).second) throw std::runtime_error("duplicate JSONL key");
    if (it->value.IsArray() || it->value.IsObject()) throw std::runtime_error("nested JSONL value");
  }
  for (std::size_t i = 0; i < jsonl_names_.size(); ++i) {
    const auto found = document.FindMember(jsonl_names_[i].c_str());
    if (found != document.MemberEnd()) row[i] = json_cell(found->value);
  }
  return true;
}

bool Reader::next_glob(std::vector<Cell>& row) {
  row.clear();
  if (glob_index_ >= glob_files_.size()) return false;
  row.push_back({glob_files_[glob_index_++], false});
  ++record_number_;
  return true;
}

bool Reader::next_raw(std::vector<Cell>& row) {
  switch (format_) {
    case Format::csv: return next_csv(row);
    case Format::jsonl: return next_jsonl(row);
    case Format::glob: return next_glob(row);
  }
  return false;
}

Schema Reader::infer() {
  if (format_ == Format::glob) {
    const auto root = std::filesystem::canonical(secure_root_);
    const std::filesystem::path pattern_path(path_);
    const std::filesystem::path dir = std::filesystem::canonical(
        pattern_path.has_parent_path() ? pattern_path.parent_path() : std::filesystem::path("."));
    if (!path_within(root, dir)) throw std::runtime_error("glob path escapes secure_file_priv");
    const std::string wildcard = pattern_path.filename().string();
    glob_files_.clear();
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      if (glob_matches(wildcard, entry.path().filename().string()) && entry.is_regular_file()) {
        const auto resolved = std::filesystem::canonical(entry.path());
        // A link to an outside file must never disclose its filename.
        if (path_within(root, resolved)) {
          glob_files_.push_back(entry.path().lexically_normal().string());
        }
      }
    }
    std::sort(glob_files_.begin(), glob_files_.end());
    schema_.columns = {{"file", Kind::varchar, false}};
    prepared_ = true;
    open();
    return schema_;
  }
  reset_stream();
  Schema found;
  std::vector<Cell> row;
  if (format_ == Format::csv) {
    if (!next_csv(row)) throw std::runtime_error("CSV input has no header or rows");
    std::unordered_set<std::string> used;
    if (header_) {
      for (const Cell& cell : row) {
        if (found.columns.size() == MAX_COLUMNS) throw std::runtime_error("too many file columns");
        found.columns.push_back({deduplicate(cell.value, used), Kind::null_value, false});
      }
    } else {
      for (std::size_t i = 0; i < row.size(); ++i) found.columns.push_back({"column" + std::to_string(i + 1), Kind::null_value, false});
      reset_stream();
    }
    while (next_csv(row)) {
      if (row.size() != found.columns.size()) throw std::runtime_error("CSV row width differs at record " + std::to_string(record_number_));
      for (std::size_t i = 0; i < row.size(); ++i) {
        found.columns[i].nullable |= row[i].is_null;
        found.columns[i].kind = widen_kind(found.columns[i].kind, infer_kind(row[i]));
      }
    }
  } else {
    std::unordered_map<std::string, std::size_t> columns;
    std::string line;
    while (std::getline(stream_, line)) {
      ++record_number_;
      if (line.size() > MAX_RECORD_BYTES) throw std::runtime_error("JSONL record exceeds memory limit");
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;
      rapidjson::Document document;
      document.Parse(line.c_str());
      if (document.HasParseError() || !document.IsObject()) throw std::runtime_error("invalid JSONL object at record " + std::to_string(record_number_));
      std::unordered_set<std::size_t> present;
      for (auto it = document.MemberBegin(); it != document.MemberEnd(); ++it) {
        std::string name(it->name.GetString(), it->name.GetStringLength());
        const auto match = columns.find(name);
        std::size_t index;
        if (match == columns.end()) {
          if (found.columns.size() == MAX_COLUMNS) throw std::runtime_error("too many JSONL columns");
          index = found.columns.size();
          columns.emplace(name, index);
          found.columns.push_back({name, Kind::null_value, record_number_ > 1});
        } else index = match->second;
        if (!present.insert(index).second) throw std::runtime_error("duplicate JSONL key");
        found.columns[index].nullable |= it->value.IsNull();
        found.columns[index].kind = widen_kind(found.columns[index].kind, json_kind(it->value));
      }
      for (std::size_t i = 0; i < found.columns.size(); ++i) if (!present.count(i)) found.columns[i].nullable = true;
    }
    jsonl_names_.clear();
    for (const auto& col : found.columns) jsonl_names_.push_back(col.name);
  }
  for (auto& col : found.columns) if (col.kind == Kind::null_value) col.nullable = true;
  schema_ = found;
  fingerprint_time_ = std::filesystem::last_write_time(path_);
  fingerprint_size_ = std::filesystem::file_size(path_);
  prepared_ = true;
  open();
  return found;
}

void Reader::open() {
  if (!prepared_) throw std::runtime_error("infer schema before opening reader");
  if (format_ == Format::glob) {
    glob_index_ = 0;
    record_number_ = 0;
    return;
  }
  if (std::filesystem::last_write_time(path_) != fingerprint_time_ ||
      std::filesystem::file_size(path_) != fingerprint_size_) {
    throw std::runtime_error("file changed since schema inference");
  }
  reset_stream();
  if (format_ == Format::csv && header_) {
    std::vector<Cell> header;
    if (!next_csv(header)) throw std::runtime_error("CSV header disappeared");
    if (header.size() != schema_.columns.size()) throw std::runtime_error("CSV schema changed");
  }
}

bool Reader::next(std::vector<Cell>& row) {
  if (!prepared_) throw std::runtime_error("infer schema before scanning");
  if (format_ == Format::glob) return next_glob(row);
  if (std::filesystem::last_write_time(path_) != fingerprint_time_ ||
      std::filesystem::file_size(path_) != fingerprint_size_) {
    throw std::runtime_error("file changed during scan");
  }
  if (!next_raw(row)) return false;
  if (row.size() != schema_.columns.size()) throw std::runtime_error("file row width differs from schema");
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (row[i].is_null) continue;
    if (schema_.columns[i].kind == Kind::datetime) row[i].value = normalized_datetime(row[i].value);
  }
  return true;
}

void Reader::rewind() { open(); }

} // namespace oceanbase::sql::file_sql
