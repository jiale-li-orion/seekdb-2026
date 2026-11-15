/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "ob_file_scan_core.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
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

constexpr std::size_t MAX_PARQUET_BYTES = 256 * 1024 * 1024;
constexpr std::size_t MAX_PARQUET_ROWS = 10 * 1000 * 1000;

struct CompactValue {
  uint8_t type = 0;
  int64_t integer = 0;
  bool boolean = false;
  std::string binary;
  std::vector<CompactValue> list;
  std::map<int, CompactValue> fields;
};

class CompactReader {
 public:
  CompactReader(const std::vector<uint8_t>& data, std::size_t position)
      : data_(data), position_(position) {}

  CompactValue read_struct() {
    CompactValue result;
    result.type = 12;
    int last_field = 0;
    for (;;) {
      require(1);
      const uint8_t header = data_[position_++];
      if (header == 0) break;
      const int delta = header >> 4;
      const uint8_t type = header & 0x0f;
      int field = 0;
      if (delta == 0) {
        const int64_t decoded = read_zigzag();
        if (decoded < 0 || decoded > std::numeric_limits<int16_t>::max()) {
          throw std::runtime_error("invalid Parquet compact field id");
        }
        field = static_cast<int>(decoded);
      } else {
        field = last_field + delta;
      }
      last_field = field;
      result.fields[field] = read_value(type, false);
    }
    return result;
  }

  std::size_t position() const noexcept { return position_; }

 private:
  void require(std::size_t length) const {
    if (length > data_.size() - std::min(position_, data_.size())) {
      throw std::runtime_error("truncated Parquet compact structure");
    }
  }

  uint64_t read_varint() {
    uint64_t value = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 10; ++i) {
      require(1);
      const uint8_t byte = data_[position_++];
      if (shift == 63 && (byte & 0x7e) != 0) {
        throw std::runtime_error("overflow in Parquet compact integer");
      }
      value |= static_cast<uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) return value;
      shift += 7;
    }
    throw std::runtime_error("invalid Parquet compact varint");
  }

  int64_t read_zigzag() {
    const uint64_t value = read_varint();
    return static_cast<int64_t>((value >> 1) ^ (0 - (value & 1)));
  }

  CompactValue read_value(uint8_t type, bool collection_value) {
    CompactValue result;
    result.type = type;
    switch (type) {
      case 1:
      case 2:
        if (collection_value) {
          require(1);
          result.boolean = data_[position_++] == 1;
        } else {
          result.boolean = type == 1;
        }
        break;
      case 3:
        require(1);
        result.integer = static_cast<int8_t>(data_[position_++]);
        break;
      case 4:
      case 5:
      case 6:
        result.integer = read_zigzag();
        break;
      case 7: {
        require(8);
        std::memcpy(&result.integer, data_.data() + position_, 8);
        position_ += 8;
        break;
      }
      case 8: {
        const uint64_t length = read_varint();
        if (length > data_.size() - position_) {
          throw std::runtime_error("truncated Parquet compact binary value");
        }
        result.binary.assign(reinterpret_cast<const char*>(data_.data() + position_),
                             static_cast<std::size_t>(length));
        position_ += static_cast<std::size_t>(length);
        break;
      }
      case 9:
      case 10: {
        require(1);
        const uint8_t header = data_[position_++];
        uint64_t count = header >> 4;
        const uint8_t element_type = header & 0x0f;
        if (count == 15) count = read_varint();
        if (count > MAX_PARQUET_ROWS * 4) {
          throw std::runtime_error("oversized Parquet compact list");
        }
        result.list.reserve(static_cast<std::size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
          result.list.push_back(read_value(element_type, true));
        }
        break;
      }
      case 11: {
        const uint64_t count = read_varint();
        if (count > MAX_PARQUET_ROWS * 4) {
          throw std::runtime_error("oversized Parquet compact map");
        }
        if (count > 0) {
          require(1);
          const uint8_t types = data_[position_++];
          const uint8_t key_type = types >> 4;
          const uint8_t value_type = types & 0x0f;
          result.list.reserve(static_cast<std::size_t>(count * 2));
          for (uint64_t i = 0; i < count; ++i) {
            result.list.push_back(read_value(key_type, true));
            result.list.push_back(read_value(value_type, true));
          }
        }
        break;
      }
      case 12:
        result = read_struct();
        break;
      default:
        throw std::runtime_error("unsupported Parquet compact type");
    }
    return result;
  }

  const std::vector<uint8_t>& data_;
  std::size_t position_;
};

const CompactValue* compact_field(const CompactValue& value, int id) {
  const auto found = value.fields.find(id);
  return found == value.fields.end() ? nullptr : &found->second;
}

int64_t compact_int(const CompactValue& value, int id, int64_t fallback = 0) {
  const CompactValue* field = compact_field(value, id);
  return field == nullptr ? fallback : field->integer;
}

bool compact_bool(const CompactValue& value, int id, bool fallback = false) {
  const CompactValue* field = compact_field(value, id);
  return field == nullptr ? fallback : field->boolean;
}

const std::string& compact_binary(const CompactValue& value, int id) {
  static const std::string empty;
  const CompactValue* field = compact_field(value, id);
  return field == nullptr ? empty : field->binary;
}

const std::vector<CompactValue>& compact_list(const CompactValue& value, int id) {
  static const std::vector<CompactValue> empty;
  const CompactValue* field = compact_field(value, id);
  return field == nullptr ? empty : field->list;
}

uint32_t read_u32(const std::vector<uint8_t>& data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 4) {
    throw std::runtime_error("truncated Parquet 32-bit value");
  }
  return static_cast<uint32_t>(data[offset])
      | (static_cast<uint32_t>(data[offset + 1]) << 8)
      | (static_cast<uint32_t>(data[offset + 2]) << 16)
      | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint64_t read_u64(const std::vector<uint8_t>& data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 8) {
    throw std::runtime_error("truncated Parquet 64-bit value");
  }
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[offset + i]) << (i * 8);
  return value;
}

uint64_t read_raw_varint(const uint8_t* data, std::size_t length, std::size_t& offset) {
  uint64_t value = 0;
  unsigned shift = 0;
  for (unsigned i = 0; i < 10; ++i) {
    if (offset >= length) throw std::runtime_error("truncated Parquet encoded value");
    const uint8_t byte = data[offset++];
    value |= static_cast<uint64_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) return value;
    shift += 7;
  }
  throw std::runtime_error("invalid Parquet encoded varint");
}

std::vector<uint8_t> snappy_decompress(const uint8_t* source, std::size_t source_size,
                                      std::size_t expected_size) {
  if (expected_size > MAX_PARQUET_BYTES) throw std::runtime_error("Parquet page exceeds memory limit");
  std::size_t offset = 0;
  const uint64_t declared_size = read_raw_varint(source, source_size, offset);
  if (declared_size != expected_size) throw std::runtime_error("invalid Snappy page length");
  std::vector<uint8_t> output;
  output.reserve(expected_size);
  while (offset < source_size && output.size() < expected_size) {
    const uint8_t tag = source[offset++];
    const uint8_t kind = tag & 0x03;
    if (kind == 0) {
      uint64_t literal_length = tag >> 2;
      if (literal_length < 60) {
        ++literal_length;
      } else {
        const unsigned extra_bytes = static_cast<unsigned>(literal_length - 59);
        if (extra_bytes > 4 || extra_bytes > source_size - offset) {
          throw std::runtime_error("invalid Snappy literal length");
        }
        literal_length = 0;
        for (unsigned i = 0; i < extra_bytes; ++i) {
          literal_length |= static_cast<uint64_t>(source[offset++]) << (8 * i);
        }
        ++literal_length;
      }
      if (literal_length > source_size - offset || literal_length > expected_size - output.size()) {
        throw std::runtime_error("truncated Snappy literal");
      }
      output.insert(output.end(), source + offset, source + offset + literal_length);
      offset += static_cast<std::size_t>(literal_length);
    } else {
      uint64_t copy_length = 0;
      uint64_t distance = 0;
      if (kind == 1) {
        if (offset >= source_size) throw std::runtime_error("truncated Snappy copy");
        copy_length = 4 + ((tag >> 2) & 0x07);
        distance = (static_cast<uint64_t>(tag & 0xe0) << 3) | source[offset++];
      } else if (kind == 2) {
        if (source_size - offset < 2) throw std::runtime_error("truncated Snappy copy");
        copy_length = 1 + (tag >> 2);
        distance = static_cast<uint64_t>(source[offset])
            | (static_cast<uint64_t>(source[offset + 1]) << 8);
        offset += 2;
      } else {
        if (source_size - offset < 4) throw std::runtime_error("truncated Snappy copy");
        copy_length = 1 + (tag >> 2);
        distance = static_cast<uint32_t>(source[offset])
            | (static_cast<uint32_t>(source[offset + 1]) << 8)
            | (static_cast<uint32_t>(source[offset + 2]) << 16)
            | (static_cast<uint32_t>(source[offset + 3]) << 24);
        offset += 4;
      }
      if (distance == 0 || distance > output.size() || copy_length > expected_size - output.size()) {
        throw std::runtime_error("invalid Snappy back-reference");
      }
      for (uint64_t i = 0; i < copy_length; ++i) {
        output.push_back(output[output.size() - static_cast<std::size_t>(distance)]);
      }
    }
  }
  if (output.size() != expected_size || offset != source_size) {
    throw std::runtime_error("Snappy page size mismatch");
  }
  return output;
}

unsigned parquet_bit_width(uint32_t maximum) {
  unsigned width = 0;
  while (maximum != 0) {
    ++width;
    maximum >>= 1;
  }
  return width;
}

std::vector<uint32_t> decode_hybrid(const uint8_t* data, std::size_t length,
                                    unsigned bit_width, std::size_t count) {
  std::vector<uint32_t> values;
  values.reserve(count);
  if (bit_width == 0) return std::vector<uint32_t>(count, 0);
  std::size_t offset = 0;
  while (values.size() < count) {
    const uint64_t header = read_raw_varint(data, length, offset);
    if ((header & 1) == 0) {
      const uint64_t run_length = header >> 1;
      const std::size_t value_bytes = (bit_width + 7) / 8;
      if (run_length == 0 || value_bytes > length - offset) {
        throw std::runtime_error("invalid Parquet RLE run");
      }
      uint32_t value = 0;
      for (std::size_t i = 0; i < value_bytes; ++i) {
        value |= static_cast<uint32_t>(data[offset++]) << (i * 8);
      }
      const std::size_t append_count = std::min<std::size_t>(run_length, count - values.size());
      values.insert(values.end(), append_count, value);
    } else {
      const uint64_t groups = header >> 1;
      if (groups == 0 || groups > (count + 7) / 8 + 1) {
        throw std::runtime_error("invalid Parquet bit-packed run");
      }
      const std::size_t packed_values = static_cast<std::size_t>(groups) * 8;
      const std::size_t packed_bytes = static_cast<std::size_t>(groups) * bit_width;
      if (packed_bytes > length - offset) throw std::runtime_error("truncated Parquet bit-packed run");
      for (std::size_t i = 0; i < packed_values && values.size() < count; ++i) {
        uint32_t value = 0;
        const std::size_t first_bit = i * bit_width;
        for (unsigned bit = 0; bit < bit_width; ++bit) {
          const std::size_t absolute = first_bit + bit;
          value |= static_cast<uint32_t>((data[offset + absolute / 8] >> (absolute % 8)) & 1) << bit;
        }
        values.push_back(value);
      }
      offset += packed_bytes;
    }
  }
  return values;
}

std::string format_parquet_date(int32_t days) {
  using namespace std::chrono;
  const year_month_day date{sys_days{year{1970}/January/1} + std::chrono::days{days}};
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u",
      static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
      static_cast<unsigned>(date.day()));
  return buffer;
}

std::string format_parquet_timestamp(int64_t value, bool micros) {
  using namespace std::chrono;
  const auto time = micros
      ? sys_time<microseconds>{microseconds{value}}
      : time_point_cast<microseconds>(sys_time<milliseconds>{milliseconds{value}});
  const auto day = floor<std::chrono::days>(time);
  const year_month_day date{day};
  const hh_mm_ss<microseconds> tod{time - day};
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02lld:%02lld:%02lld",
      static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
      static_cast<unsigned>(date.day()), static_cast<long long>(tod.hours().count()),
      static_cast<long long>(tod.minutes().count()), static_cast<long long>(tod.seconds().count()));
  return buffer;
}

std::string format_parquet_double(double value) {
  char buffer[64];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
      std::chars_format::general, std::numeric_limits<double>::max_digits10);
  if (result.ec != std::errc{}) throw std::runtime_error("unable to format Parquet floating value");
  return std::string(buffer, result.ptr);
}

struct ParquetColumn {
  std::string name;
  int physical_type = -1;
  int type_length = 0;
  int converted_type = -1;
  int max_definition_level = 0;
  int max_repetition_level = 0;
};

Kind parquet_kind(const ParquetColumn& column) {
  switch (column.physical_type) {
    case 0: return Kind::boolean;
    case 1: return column.converted_type == 6 ? Kind::date : Kind::bigint;
    case 2: return (column.converted_type == 9 || column.converted_type == 10)
        ? Kind::datetime : Kind::bigint;
    case 4:
    case 5: return Kind::double_value;
    case 6:
    case 7: return Kind::varchar;
    default: return Kind::varchar;
  }
}

std::vector<Cell> decode_plain_values(const std::vector<uint8_t>& data, std::size_t count,
                                      const ParquetColumn& column) {
  std::vector<Cell> values;
  values.reserve(count);
  std::size_t offset = 0;
  if (column.physical_type == 0) {
    const std::size_t bytes = (count + 7) / 8;
    if (bytes > data.size()) throw std::runtime_error("truncated Parquet boolean values");
    for (std::size_t i = 0; i < count; ++i) {
      values.push_back({(data[i / 8] >> (i % 8)) & 1 ? "true" : "false", false});
    }
    return values;
  }
  for (std::size_t i = 0; i < count; ++i) {
    switch (column.physical_type) {
      case 1: {
        const int32_t value = static_cast<int32_t>(read_u32(data, offset));
        offset += 4;
        values.push_back({column.converted_type == 6 ? format_parquet_date(value) : std::to_string(value), false});
        break;
      }
      case 2: {
        const int64_t value = static_cast<int64_t>(read_u64(data, offset));
        offset += 8;
        if (column.converted_type == 9 || column.converted_type == 10) {
          values.push_back({format_parquet_timestamp(value, column.converted_type == 10), false});
        } else {
          values.push_back({std::to_string(value), false});
        }
        break;
      }
      case 4: {
        if (offset > data.size() || data.size() - offset < 4) throw std::runtime_error("truncated Parquet float");
        float value = 0;
        std::memcpy(&value, data.data() + offset, 4);
        offset += 4;
        values.push_back({format_parquet_double(value), false});
        break;
      }
      case 5: {
        if (offset > data.size() || data.size() - offset < 8) throw std::runtime_error("truncated Parquet double");
        double value = 0;
        std::memcpy(&value, data.data() + offset, 8);
        offset += 8;
        values.push_back({format_parquet_double(value), false});
        break;
      }
      case 6: {
        const int32_t length = static_cast<int32_t>(read_u32(data, offset));
        offset += 4;
        if (length < 0 || static_cast<std::size_t>(length) > data.size() - offset) {
          throw std::runtime_error("invalid Parquet byte-array length");
        }
        values.push_back({std::string(reinterpret_cast<const char*>(data.data() + offset), length), false});
        offset += static_cast<std::size_t>(length);
        break;
      }
      case 7: {
        if (column.type_length <= 0 || static_cast<std::size_t>(column.type_length) > data.size() - offset) {
          throw std::runtime_error("invalid Parquet fixed byte-array length");
        }
        values.push_back({std::string(reinterpret_cast<const char*>(data.data() + offset), column.type_length), false});
        offset += static_cast<std::size_t>(column.type_length);
        break;
      }
      default:
        throw std::runtime_error("unsupported Parquet physical type");
    }
  }
  return values;
}

std::vector<uint8_t> decompress_parquet_body(const std::vector<uint8_t>& body,
                                             std::size_t uncompressed_size, int codec) {
  if (codec == 0) {
    if (body.size() != uncompressed_size) throw std::runtime_error("Parquet uncompressed page size mismatch");
    return body;
  }
  if (codec == 1) return snappy_decompress(body.data(), body.size(), uncompressed_size);
  throw std::runtime_error("Parquet compression codec is unsupported");
}

void append_parquet_data_page(const CompactValue& page_header, bool version_two,
                              const std::vector<uint8_t>& body, std::size_t uncompressed_size,
                              int codec, const ParquetColumn& column,
                              const std::vector<Cell>& dictionary, std::vector<Cell>& output) {
  const CompactValue* page = compact_field(page_header, version_two ? 8 : 5);
  if (page == nullptr) throw std::runtime_error("Parquet data page header is missing");
  const std::size_t num_values = static_cast<std::size_t>(compact_int(*page, 1));
  const int encoding = static_cast<int>(compact_int(*page, version_two ? 4 : 2));
  const std::size_t repetition_length = version_two
      ? static_cast<std::size_t>(compact_int(*page, 6)) : 0;
  const std::size_t definition_length = version_two
      ? static_cast<std::size_t>(compact_int(*page, 5)) : 0;
  if (num_values > MAX_PARQUET_ROWS || column.max_repetition_level != 0) {
    throw std::runtime_error("repeated or oversized Parquet data pages are unsupported");
  }
  std::vector<uint8_t> values_data;
  std::vector<uint32_t> definitions;
  std::size_t value_offset = 0;
  if (version_two) {
    if (repetition_length != 0 || repetition_length + definition_length > body.size()) {
      throw std::runtime_error("invalid Parquet V2 level lengths");
    }
    if (column.max_definition_level > 0) {
      definitions = decode_hybrid(body.data() + repetition_length, definition_length,
          parquet_bit_width(column.max_definition_level), num_values);
    } else if (definition_length != 0) {
      throw std::runtime_error("unexpected Parquet definition levels");
    }
    value_offset = repetition_length + definition_length;
    if (value_offset > uncompressed_size) throw std::runtime_error("invalid Parquet V2 uncompressed size");
    const std::size_t value_uncompressed = uncompressed_size - value_offset;
    if (compact_bool(*page, 7, true) && codec != 0) {
      values_data = snappy_decompress(body.data() + value_offset, body.size() - value_offset,
                                      value_uncompressed);
    } else {
      values_data.assign(body.begin() + static_cast<std::ptrdiff_t>(value_offset), body.end());
      if (values_data.size() != value_uncompressed) throw std::runtime_error("Parquet V2 value size mismatch");
    }
  } else {
    std::vector<uint8_t> decoded_body = decompress_parquet_body(body, uncompressed_size, codec);
    if (column.max_definition_level > 0) {
      const std::size_t length = read_u32(decoded_body, 0);
      if (length > decoded_body.size() - 4) throw std::runtime_error("invalid Parquet definition level length");
      definitions = decode_hybrid(decoded_body.data() + 4, length,
          parquet_bit_width(column.max_definition_level), num_values);
      value_offset = 4 + length;
    }
    values_data.assign(decoded_body.begin() + static_cast<std::ptrdiff_t>(value_offset), decoded_body.end());
  }
  if (definitions.empty()) definitions.assign(num_values, 0);
  std::size_t present_count = 0;
  for (uint32_t level : definitions) if (level == static_cast<uint32_t>(column.max_definition_level)) ++present_count;
  std::vector<Cell> decoded_values;
  if (encoding == 0) {
    decoded_values = decode_plain_values(values_data, present_count, column);
  } else if (encoding == 3 && column.physical_type == 0) {
    // Parquet RLE boolean pages carry a four-byte byte-count followed by
    // hybrid RLE/bit-packed data. Definition levels have already been split.
    const uint32_t rle_bytes = read_u32(values_data, 0);
    if (rle_bytes != values_data.size() - 4) {
      throw std::runtime_error("Parquet boolean RLE length mismatch");
    }
    const auto bits = decode_hybrid(values_data.data() + 4, rle_bytes, 1, present_count);
    decoded_values.reserve(bits.size());
    for (uint32_t bit : bits) {
      if (bit > 1) throw std::runtime_error("invalid Parquet boolean RLE value");
      decoded_values.push_back({bit == 1 ? "true" : "false", false});
    }
  } else if (encoding == 2 || encoding == 8) {
    if (dictionary.empty() || values_data.empty()) throw std::runtime_error("Parquet dictionary page is missing");
    const unsigned bit_width = values_data[0];
    const auto indices = decode_hybrid(values_data.data() + 1, values_data.size() - 1,
                                        bit_width, present_count);
    decoded_values.reserve(present_count);
    for (uint32_t index : indices) {
      if (index >= dictionary.size()) throw std::runtime_error("invalid Parquet dictionary index");
      decoded_values.push_back(dictionary[index]);
    }
  } else {
    throw std::runtime_error("Parquet data page encoding is unsupported");
  }
  std::size_t value_index = 0;
  for (uint32_t level : definitions) {
    if (level == static_cast<uint32_t>(column.max_definition_level)) {
      if (value_index >= decoded_values.size()) throw std::runtime_error("Parquet value count mismatch");
      output.push_back(std::move(decoded_values[value_index++]));
    } else {
      output.emplace_back();
    }
  }
}

std::vector<Cell> decode_parquet_column(const std::vector<uint8_t>& file,
                                        std::size_t chunk_start, std::size_t chunk_size,
                                        std::size_t expected_rows, int codec,
                                        const ParquetColumn& column) {
  if (chunk_start > file.size() || chunk_size > file.size() - chunk_start) {
    throw std::runtime_error("Parquet column chunk is outside the file");
  }
  const std::size_t chunk_end = chunk_start + chunk_size;
  std::size_t offset = chunk_start;
  std::vector<Cell> output;
  output.reserve(expected_rows);
  std::vector<Cell> dictionary;
  while (offset < chunk_end) {
    CompactReader page_reader(file, offset);
    const CompactValue header = page_reader.read_struct();
    offset = page_reader.position();
    const int page_type = static_cast<int>(compact_int(header, 1, -1));
    const std::size_t uncompressed_size = static_cast<std::size_t>(compact_int(header, 2));
    const std::size_t compressed_size = static_cast<std::size_t>(compact_int(header, 3));
    if (uncompressed_size > MAX_PARQUET_BYTES || compressed_size > chunk_end - offset) {
      throw std::runtime_error("invalid Parquet page size");
    }
    std::vector<uint8_t> body(file.begin() + static_cast<std::ptrdiff_t>(offset),
        file.begin() + static_cast<std::ptrdiff_t>(offset + compressed_size));
    offset += compressed_size;
    if (page_type == 2) {
      const CompactValue* dictionary_header = compact_field(header, 7);
      const int dict_encoding = dictionary_header == nullptr ? -1
          : static_cast<int>(compact_int(*dictionary_header, 2, -1));
      if (dict_encoding != 0 && dict_encoding != 2) {
        throw std::runtime_error("Parquet dictionary page encoding is unsupported");
      }
      const std::size_t dictionary_size = static_cast<std::size_t>(compact_int(*dictionary_header, 1));
      dictionary = decode_plain_values(decompress_parquet_body(body, uncompressed_size, codec),
                                       dictionary_size, column);
    } else if (page_type == 0) {
      append_parquet_data_page(header, false, body, uncompressed_size, codec, column, dictionary, output);
    } else if (page_type == 3) {
      append_parquet_data_page(header, true, body, uncompressed_size, codec, column, dictionary, output);
    } else if (page_type != 1) {
      throw std::runtime_error("Parquet page type is unsupported");
    }
  }
  if (output.size() != expected_rows) throw std::runtime_error("Parquet column row count differs from row group");
  return output;
}

void decode_parquet_file(const std::vector<uint8_t>& file, Schema& schema,
                         std::vector<std::vector<Cell>>& rows) {
  if (file.size() < 12 || std::memcmp(file.data(), "PAR1", 4) != 0
      || std::memcmp(file.data() + file.size() - 4, "PAR1", 4) != 0) {
    throw std::runtime_error("invalid Parquet file signature");
  }
  const uint32_t footer_size = read_u32(file, file.size() - 8);
  if (footer_size > file.size() - 12) throw std::runtime_error("invalid Parquet footer size");
  const std::size_t footer_offset = file.size() - 8 - footer_size;
  CompactReader footer_reader(file, footer_offset);
  const CompactValue metadata = footer_reader.read_struct();
  if (footer_reader.position() != file.size() - 8) throw std::runtime_error("Parquet footer length mismatch");
  const auto& schema_elements = compact_list(metadata, 2);
  if (schema_elements.size() < 2 || compact_int(schema_elements.front(), 5) != static_cast<int64_t>(schema_elements.size() - 1)
      || schema_elements.size() > MAX_COLUMNS + 1) {
    throw std::runtime_error("nested or invalid Parquet schemas are unsupported");
  }
  std::vector<ParquetColumn> parquet_columns;
  parquet_columns.reserve(schema_elements.size() - 1);
  schema.columns.clear();
  for (std::size_t i = 1; i < schema_elements.size(); ++i) {
    const CompactValue& element = schema_elements[i];
    if (compact_int(element, 5) != 0) throw std::runtime_error("nested Parquet columns are unsupported");
    ParquetColumn column;
    column.name = compact_binary(element, 4);
    column.physical_type = static_cast<int>(compact_int(element, 1, -1));
    column.type_length = static_cast<int>(compact_int(element, 2));
    column.converted_type = static_cast<int>(compact_int(element, 6, -1));
    const int repetition = static_cast<int>(compact_int(element, 3, 0));
    if (repetition == 2) throw std::runtime_error("repeated Parquet columns are unsupported");
    column.max_definition_level = repetition == 1 ? 1 : 0;
    column.max_repetition_level = 0;
    if (column.name.empty() || column.physical_type < 0 || column.physical_type == 3) {
      throw std::runtime_error("invalid or unsupported Parquet column schema");
    }
    parquet_columns.push_back(column);
    schema.columns.push_back({column.name, parquet_kind(column), column.max_definition_level > 0});
  }

  const auto& row_groups = compact_list(metadata, 4);
  const int64_t declared_rows = compact_int(metadata, 3, -1);
  if (declared_rows < 0 || static_cast<uint64_t>(declared_rows) > MAX_PARQUET_ROWS) {
    throw std::runtime_error("Parquet row count exceeds scan limit");
  }
  rows.clear();
  rows.reserve(static_cast<std::size_t>(declared_rows));
  for (const CompactValue& row_group : row_groups) {
    const int64_t row_count = compact_int(row_group, 3, -1);
    const auto& chunks = compact_list(row_group, 1);
    if (row_count < 0 || static_cast<uint64_t>(row_count) > MAX_PARQUET_ROWS
        || chunks.size() != parquet_columns.size()) {
      throw std::runtime_error("invalid Parquet row group");
    }
    std::vector<std::vector<Cell>> column_rows(parquet_columns.size());
    std::vector<bool> seen(parquet_columns.size(), false);
    for (const CompactValue& chunk : chunks) {
      const CompactValue* column_metadata = compact_field(chunk, 3);
      if (column_metadata == nullptr) throw std::runtime_error("external Parquet column chunks are unsupported");
      const auto& path = compact_list(*column_metadata, 3);
      if (path.size() != 1) throw std::runtime_error("nested Parquet column paths are unsupported");
      const std::string& name = path[0].binary;
      const auto column_it = std::find_if(parquet_columns.begin(), parquet_columns.end(),
          [&](const ParquetColumn& column) { return column.name == name; });
      if (column_it == parquet_columns.end()) throw std::runtime_error("Parquet column path differs from schema");
      const std::size_t column_index = static_cast<std::size_t>(column_it - parquet_columns.begin());
      if (seen[column_index]) throw std::runtime_error("duplicate Parquet column chunk");
      seen[column_index] = true;
      const int physical_type = static_cast<int>(compact_int(*column_metadata, 1, -1));
      const int codec = static_cast<int>(compact_int(*column_metadata, 4, -1));
      const std::size_t compressed_size = static_cast<std::size_t>(compact_int(*column_metadata, 7));
      const std::size_t data_offset = static_cast<std::size_t>(compact_int(*column_metadata, 9));
      const int64_t dictionary_offset = compact_int(*column_metadata, 11, -1);
      const std::size_t chunk_start = dictionary_offset >= 0
          ? std::min(data_offset, static_cast<std::size_t>(dictionary_offset)) : data_offset;
      if (physical_type != parquet_columns[column_index].physical_type) {
        throw std::runtime_error("Parquet physical type differs from schema");
      }
      const std::size_t num_values = static_cast<std::size_t>(compact_int(*column_metadata, 5));
      if (num_values != static_cast<std::size_t>(row_count)) {
        throw std::runtime_error("repeated Parquet column values are unsupported");
      }
      column_rows[column_index] = decode_parquet_column(file, chunk_start, compressed_size,
          static_cast<std::size_t>(row_count), codec, parquet_columns[column_index]);
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
      if (!seen[i]) throw std::runtime_error("Parquet row group is missing a column");
      for (const Cell& value : column_rows[i]) schema.columns[i].nullable |= value.is_null;
    }
    for (int64_t row = 0; row < row_count; ++row) {
      std::vector<Cell> record;
      record.reserve(parquet_columns.size());
      for (std::size_t column = 0; column < parquet_columns.size(); ++column) {
        record.push_back(std::move(column_rows[column][static_cast<std::size_t>(row)]));
      }
      rows.push_back(std::move(record));
    }
  }
  if (rows.size() != static_cast<std::size_t>(declared_rows)) {
    throw std::runtime_error("Parquet row groups differ from file row count");
  }
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

bool Reader::next_buffered(std::vector<Cell>& row) {
  if (buffered_index_ >= buffered_rows_.size()) return false;
  row = buffered_rows_[buffered_index_++];
  ++record_number_;
  return true;
}

bool Reader::next_raw(std::vector<Cell>& row) {
  switch (format_) {
    case Format::csv: return next_csv(row);
    case Format::jsonl: return next_jsonl(row);
    case Format::parquet:
    case Format::csv_schema: return next_buffered(row);
    case Format::glob: return next_glob(row);
  }
  return false;
}

Schema Reader::infer() {
  if (format_ == Format::csv_schema) {
    Reader input(path_, Format::csv, secure_root_, true);
    const Schema source = input.infer();
    schema_.columns = {
        {"column_name", Kind::varchar, false},
        {"column_type", Kind::varchar, false},
        {"null", Kind::varchar, false},
        {"key", Kind::varchar, true},
        {"default", Kind::varchar, true},
        {"extra", Kind::varchar, true},
    };
    buffered_rows_.clear();
    buffered_rows_.reserve(source.columns.size());
    for (const Column &column : source.columns) {
      buffered_rows_.push_back({
          {column.name, false},
          {kind_name(column.kind), false},
          {column.nullable ? "YES" : "NO", false},
          {}, {}, {},
      });
    }
    fingerprint_time_ = std::filesystem::last_write_time(path_);
    fingerprint_size_ = std::filesystem::file_size(path_);
    prepared_ = true;
    open();
    return schema_;
  }
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
  if (format_ == Format::parquet) {
    reset_stream();
    stream_.seekg(0, std::ios::end);
    const std::streamoff size = stream_.tellg();
    if (size < 12 || static_cast<uint64_t>(size) > MAX_PARQUET_BYTES) {
      throw std::runtime_error("Parquet file size is invalid or exceeds memory limit");
    }
    stream_.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream_.read(reinterpret_cast<char*>(bytes.data()), size)) {
      throw std::runtime_error("unable to read Parquet input");
    }
    Schema found;
    std::vector<std::vector<Cell>> rows;
    decode_parquet_file(bytes, found, rows);
    schema_ = std::move(found);
    buffered_rows_ = std::move(rows);
    fingerprint_time_ = std::filesystem::last_write_time(path_);
    fingerprint_size_ = std::filesystem::file_size(path_);
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
  if (format_ == Format::parquet || format_ == Format::csv_schema) {
    buffered_index_ = 0;
    record_number_ = 0;
    return;
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
