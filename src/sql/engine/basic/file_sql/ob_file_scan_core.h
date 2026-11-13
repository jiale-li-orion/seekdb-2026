/*
 * Copyright (c) 2026 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

namespace oceanbase::sql::file_sql {

enum class Kind { null_value, boolean, bigint, double_value, date, datetime, varchar };

struct Column {
  std::string name;
  Kind kind = Kind::null_value;
  bool nullable = false;
};

struct Cell {
  std::string value;
  bool is_null = true;
};

struct Schema {
  std::vector<Column> columns;
};

// The reader owns an input stream and retains only its current record.
// Schema discovery is a separate full-file pass; scan() reopens the source.
class Reader {
 public:
  enum class Format { csv, jsonl };
  Reader(std::string path, Format format, bool header = true);
  Schema infer();
  void open();
  bool next(std::vector<Cell>& row);
  void rewind();
  std::size_t current_record() const noexcept { return record_number_; }

 private:
  bool next_raw(std::vector<Cell>& row);
  bool next_csv(std::vector<Cell>& row);
  bool next_jsonl(std::vector<Cell>& row);
  void reset_stream();
  std::string path_;
  Format format_;
  bool header_;
  std::ifstream stream_;
  Schema schema_;
  std::vector<std::string> jsonl_names_;
  std::size_t record_number_ = 0;
  bool prepared_ = false;
  std::filesystem::file_time_type fingerprint_time_;
  std::uintmax_t fingerprint_size_ = 0;
};

Kind infer_kind(const Cell& value);
Kind widen_kind(Kind left, Kind right);
const char* kind_name(Kind kind);

} // namespace oceanbase::sql::file_sql
