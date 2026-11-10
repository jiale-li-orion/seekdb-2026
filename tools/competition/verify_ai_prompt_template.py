#!/usr/bin/env python3
"""Compile the production template scanner in isolation and exercise edge cases.

The fixture extracts the production C++ scanner verbatim. It checks the two-pass
rendering logic without needing a full OceanBase build, and is complementary to
mysqltest/SQL integration testing with a patched server binary.
"""
from __future__ import annotations

import pathlib
import subprocess
import tempfile

PROJECT = pathlib.Path(__file__).resolve().parents[2]
PRODUCTION = PROJECT / 'src/sql/engine/expr/ob_expr_ai/ob_ai_func_utils.cpp'
text = PRODUCTION.read_text()
start = text.index('int write_ai_prompt_template(')
end = text.index('\nint expand_ai_prompt_object(', start)
production_scanner = text[start:end]

header = r'''#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#define OB_SUCCESS 0
#define OB_INVALID_ARGUMENT 1
#define OB_SIZE_OVERFLOW 2
#define OB_FAIL(x) ((ret = (x)) != OB_SUCCESS)
#define OB_SUCC(x) ((x) == OB_SUCCESS)
#define MEMCPY std::memcpy
class ObString {
public:
  explicit ObString(std::string v): value_(std::move(v)) {}
  int64_t length() const {return static_cast<int64_t>(value_.size());}
  const char *ptr() const {return value_.data();}
private:
  std::string value_;
};
template <typename T> class ObArray {
public:
  ObArray(std::initializer_list<T> elements): values_(elements) {}
  int64_t count() const {return static_cast<int64_t>(values_.size());}
  const T &at(int64_t index) const {return values_.at(index);}
private:
  std::vector<T> values_;
};
'''
body = r'''
int test_count = 0;
void expect(const std::string &pattern, const ObArray<ObString> &args, const std::string &expected) {
  int64_t count = -1;
  int ret = write_ai_prompt_template(ObString(pattern), args, nullptr, 0, count);
  if (ret != OB_SUCCESS || count != static_cast<int64_t>(expected.size())) std::abort();
  std::string output(static_cast<size_t>(count), '?');
  int64_t written = -1;
  ret = write_ai_prompt_template(ObString(pattern), args, output.data(), count, written);
  if (ret != OB_SUCCESS || written != count || output != expected) {
    std::cerr << "Mismatch pattern: " << pattern << " output: " << output << "\n";
    std::abort();
  }
  ++test_count;
}
void invalid(const std::string &pattern, const ObArray<ObString> &args) {
  int64_t count = -1;
  if (write_ai_prompt_template(ObString(pattern), args, nullptr, 0, count) != OB_INVALID_ARGUMENT) std::abort();
  ++test_count;
}
int main() {
  expect("{0}-{0}-{1}", {ObString("abcdefghijkl"), ObString("9")}, "abcdefghijkl-abcdefghijkl-9");
  expect("{0}/{0}/{1}", {ObString("[1,true]"), ObString("{\"k\":2}")}, "[1,true]/[1,true]/{\"k\":2}");
  expect("{{{0}}}", {ObString("X")}, "{X}");
  expect("我爱{0}，{0}", {ObString("数学")}, "我爱数学，数学");
  expect("{01}", {ObString("a"), ObString("b")}, "b");
  expect("{{}}", {}, "{}");
  expect("plain", {}, "plain");
  expect("A{0}B", {ObString(std::string(1000, 'z'))}, "A"+std::string(1000,'z')+"B");
  invalid("{3}", {ObString("a")});
  invalid("{abc}", {});
  invalid("{0", {ObString("a")});
  invalid("hello }", {});
  invalid("{999999999999999999999999999}", {ObString("a")});
  int64_t count = 0;
  std::string output(1, ' ');
  if (write_ai_prompt_template(ObString("xx"), {}, output.data(), 1, count) != OB_SIZE_OVERFLOW) std::abort();
  ++test_count;
  std::cout << "PASS: " << test_count << " native production scanner checks\n";
}
'''
with tempfile.TemporaryDirectory() as d:
    src = pathlib.Path(d) / 'fixture.cpp'
    executable = pathlib.Path(d) / 'fixture'
    src.write_text(header + production_scanner + body)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', str(src), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
